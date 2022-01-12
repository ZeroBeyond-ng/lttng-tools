/*
 * lttng-ctl.c
 *
 * Linux Trace Toolkit Control Library
 *
 * Copyright (C) 2011 EfficiOS Inc.
 * Copyright (C) 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#define _LGPL_SOURCE
#include <assert.h>
#include <grp.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <common/buffer-view.h>
#include <common/common.h>
#include <common/compat/string.h>
#include <common/defaults.h>
#include <common/dynamic-array.h>
#include <common/dynamic-buffer.h>
#include <common/macros.h>
#include <common/sessiond-comm/sessiond-comm.h>
#include <common/tracker.h>
#include <common/uri.h>
#include <common/utils.h>
#include <lttng/channel-internal.h>
#include <lttng/destruction-handle.h>
#include <lttng/endpoint.h>
#include <lttng/event-internal.h>
#include <lttng/health-internal.h>
#include <lttng/lttng.h>
#include <lttng/session-descriptor-internal.h>
#include <lttng/session-internal.h>
#include <lttng/trigger/trigger-internal.h>
#include <lttng/userspace-probe-internal.h>

#include "filter/filter-ast.h"
#include "filter/filter-parser.h"
#include "filter/filter-bytecode.h"
#include "filter/memstream.h"
#include "lttng-ctl-helper.h"

#ifdef DEBUG
static const int print_xml = 1;
#define dbg_printf(fmt, args...)	\
	printf("[debug liblttng-ctl] " fmt, ## args)
#else
static const int print_xml = 0;
#define dbg_printf(fmt, args...)				\
do {								\
	/* do nothing but check printf format */		\
	if (0)							\
		printf("[debug liblttnctl] " fmt, ## args);	\
} while (0)
#endif

#define COPY_DOMAIN_PACKED(dst, src)				\
do {								\
	struct lttng_domain _tmp_domain;			\
								\
	lttng_ctl_copy_lttng_domain(&_tmp_domain, &src);	\
	dst = _tmp_domain;					\
} while (0)

/* Socket to session daemon for communication */
static int sessiond_socket = -1;
static char sessiond_sock_path[PATH_MAX];

/* Variables */
static char *tracing_group;
static int connected;

/* Global */

/*
 * Those two variables are used by error.h to silent or control the verbosity of
 * error message. They are global to the library so application linking with it
 * are able to compile correctly and also control verbosity of the library.
 */
int lttng_opt_quiet;
int lttng_opt_verbose;
int lttng_opt_mi;

/*
 * Copy domain to lttcomm_session_msg domain.
 *
 * If domain is unknown, default domain will be the kernel.
 */
LTTNG_HIDDEN
void lttng_ctl_copy_lttng_domain(struct lttng_domain *dst,
		struct lttng_domain *src)
{
	if (src && dst) {
		switch (src->type) {
		case LTTNG_DOMAIN_KERNEL:
		case LTTNG_DOMAIN_UST:
		case LTTNG_DOMAIN_JUL:
		case LTTNG_DOMAIN_LOG4J:
		case LTTNG_DOMAIN_PYTHON:
			memcpy(dst, src, sizeof(struct lttng_domain));
			break;
		default:
			memset(dst, 0, sizeof(struct lttng_domain));
			break;
		}
	}
}

/*
 * Send lttcomm_session_msg to the session daemon.
 *
 * On success, returns the number of bytes sent (>=0)
 * On error, returns -1
 */
static int send_session_msg(struct lttcomm_session_msg *lsm)
{
	int ret;

	if (!connected) {
		ret = -LTTNG_ERR_NO_SESSIOND;
		goto end;
	}

	DBG("LSM cmd type : %d", lsm->cmd_type);

	ret = lttcomm_send_creds_unix_sock(sessiond_socket, lsm,
			sizeof(struct lttcomm_session_msg));
	if (ret < 0) {
		ret = -LTTNG_ERR_FATAL;
	}

end:
	return ret;
}

/*
 * Send var len data to the session daemon.
 *
 * On success, returns the number of bytes sent (>=0)
 * On error, returns -1
 */
static int send_session_varlen(const void *data, size_t len)
{
	int ret;

	if (!connected) {
		ret = -LTTNG_ERR_NO_SESSIOND;
		goto end;
	}

	if (!data || !len) {
		ret = 0;
		goto end;
	}

	ret = lttcomm_send_unix_sock(sessiond_socket, data, len);
	if (ret < 0) {
		ret = -LTTNG_ERR_FATAL;
	}

end:
	return ret;
}

/*
 * Send file descriptors to the session daemon.
 *
 * On success, returns the number of bytes sent (>=0)
 * On error, returns -1
 */
static int send_session_fds(const int *fds, size_t nb_fd)
{
	int ret;

	if (!connected) {
		ret = -LTTNG_ERR_NO_SESSIOND;
		goto end;
	}

	if (!fds || !nb_fd) {
		ret = 0;
		goto end;
	}

	ret = lttcomm_send_fds_unix_sock(sessiond_socket, fds, nb_fd);
	if (ret < 0) {
		ret = -LTTNG_ERR_FATAL;
	}

end:
	return ret;
}

/*
 * Receive data from the sessiond socket.
 *
 * On success, returns the number of bytes received (>=0)
 * On error, returns -1 (recvmsg() error) or -ENOTCONN
 */
static int recv_data_sessiond(void *buf, size_t len)
{
	int ret;

	if (!connected) {
		ret = -LTTNG_ERR_NO_SESSIOND;
		goto end;
	}

	ret = lttcomm_recv_unix_sock(sessiond_socket, buf, len);
	if (ret < 0) {
		ret = -LTTNG_ERR_FATAL;
	}

end:
	return ret;
}

/*
 * Check if we are in the specified group.
 *
 * If yes return 1, else return -1.
 */
LTTNG_HIDDEN
int lttng_check_tracing_group(void)
{
	gid_t *grp_list, tracing_gid;
	int grp_list_size, grp_id, i;
	int ret = -1;
	const char *grp_name = tracing_group;

	/* Get GID of group 'tracing' */
	if (utils_get_group_id(grp_name, false, &tracing_gid)) {
		/* If grp_tracing is NULL, the group does not exist. */
		goto end;
	}

	/* Get number of supplementary group IDs */
	grp_list_size = getgroups(0, NULL);
	if (grp_list_size < 0) {
		PERROR("getgroups");
		goto end;
	}

	/* Alloc group list of the right size */
	grp_list = zmalloc(grp_list_size * sizeof(gid_t));
	if (!grp_list) {
		PERROR("malloc");
		goto end;
	}
	grp_id = getgroups(grp_list_size, grp_list);
	if (grp_id < 0) {
		PERROR("getgroups");
		goto free_list;
	}

	for (i = 0; i < grp_list_size; i++) {
		if (grp_list[i] == tracing_gid) {
			ret = 1;
			break;
		}
	}

free_list:
	free(grp_list);

end:
	return ret;
}

static enum lttng_error_code check_enough_available_memory(
		uint64_t num_bytes_requested_per_cpu)
{
	int ret;
	long num_cpu;
	uint64_t best_mem_info;
	uint64_t num_bytes_requested_total;

	/*
	 * Get the number of CPU currently online to compute the amount of
	 * memory needed to create a buffer for every CPU.
	 */
	num_cpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpu == -1) {
		ret = LTTNG_ERR_FATAL;
		goto end;
	}

	if (num_bytes_requested_per_cpu > UINT64_MAX / (uint64_t) num_cpu) {
		/* Overflow */
		ret = LTTNG_ERR_OVERFLOW;
		goto end;
	}

	num_bytes_requested_total =
			num_bytes_requested_per_cpu * (uint64_t) num_cpu;

	/*
	 * Try to get the `MemAvail` field of `/proc/meminfo`. This is the most
	 * reliable estimate we can get but it is only exposed by the kernel
	 * since 3.14. (See Linux kernel commit:
	 * 34e431b0ae398fc54ea69ff85ec700722c9da773)
	 */
	ret = utils_get_memory_available(&best_mem_info);
	if (ret >= 0) {
		goto success;
	}

	/*
	 * As a backup plan, use `MemTotal` field of `/proc/meminfo`. This
	 * is a sanity check for obvious user error.
	 */
	ret = utils_get_memory_total(&best_mem_info);
	if (ret >= 0) {
		goto success;
	}

	/* No valid source of information. */
	ret = LTTNG_ERR_NOMEM;
	goto end;

success:
	if (best_mem_info >= num_bytes_requested_total) {
		ret = LTTNG_OK;
	} else {
		ret = LTTNG_ERR_NOMEM;
	}
end:
	return ret;
}

/*
 * Try connect to session daemon with sock_path.
 *
 * Return 0 on success, else -1
 */
static int try_connect_sessiond(const char *sock_path)
{
	int ret;

	/* If socket exist, we check if the daemon listens for connect. */
	ret = access(sock_path, F_OK);
	if (ret < 0) {
		/* Not alive */
		goto error;
	}

	ret = lttcomm_connect_unix_sock(sock_path);
	if (ret < 0) {
		/* Not alive. */
		goto error;
	}

	ret = lttcomm_close_unix_sock(ret);
	if (ret < 0) {
		PERROR("lttcomm_close_unix_sock");
	}

	return 0;

error:
	return -1;
}

/*
 * Set sessiond socket path by putting it in the global sessiond_sock_path
 * variable.
 *
 * Returns 0 on success, negative value on failure (the sessiond socket path
 * is somehow too long or ENOMEM).
 */
static int set_session_daemon_path(void)
{
	int in_tgroup = 0;	/* In tracing group. */
	uid_t uid;

	uid = getuid();

	if (uid != 0) {
		/* Are we in the tracing group ? */
		in_tgroup = lttng_check_tracing_group();
	}

	if ((uid == 0) || in_tgroup == 1) {
		const int ret = lttng_strncpy(sessiond_sock_path,
				DEFAULT_GLOBAL_CLIENT_UNIX_SOCK,
				sizeof(sessiond_sock_path));

		if (ret) {
			goto error;
		}
	}

	if (uid != 0) {
		int ret;

		if (in_tgroup) {
			/* Tracing group. */
			ret = try_connect_sessiond(sessiond_sock_path);
			if (ret >= 0) {
				goto end;
			}
			/* Global session daemon not available... */
		}
		/* ...or not in tracing group (and not root), default */

		/*
		 * With GNU C <  2.1, snprintf returns -1 if the target buffer
		 * is too small;
		 * With GNU C >= 2.1, snprintf returns the required size
		 * (excluding closing null)
		 */
		ret = snprintf(sessiond_sock_path, sizeof(sessiond_sock_path),
				DEFAULT_HOME_CLIENT_UNIX_SOCK, utils_get_home_dir());
		if ((ret < 0) || (ret >= sizeof(sessiond_sock_path))) {
			goto error;
		}
	}
end:
	return 0;

error:
	return -1;
}

/*
 * Connect to the LTTng session daemon.
 *
 * On success, return the socket's file descriptor. On error, return -1.
 */
LTTNG_HIDDEN int connect_sessiond(void)
{
	int ret;

	ret = set_session_daemon_path();
	if (ret < 0) {
		goto error;
	}

	/* Connect to the sesssion daemon. */
	ret = lttcomm_connect_unix_sock(sessiond_sock_path);
	if (ret < 0) {
		goto error;
	}

	return ret;

error:
	return -1;
}

static void reset_global_sessiond_connection_state(void)
{
	sessiond_socket = -1;
	connected = 0;
}

/*
 *  Clean disconnect from the session daemon.
 *
 *  On success, return 0. On error, return -1.
 */
static int disconnect_sessiond(void)
{
	int ret = 0;

	if (connected) {
		ret = lttcomm_close_unix_sock(sessiond_socket);
		reset_global_sessiond_connection_state();
	}

	return ret;
}

static int recv_sessiond_optional_data(size_t len, void **user_buf,
	size_t *user_len)
{
	int ret = 0;
	void *buf = NULL;

	if (len) {
		if (!user_len) {
			ret = -LTTNG_ERR_INVALID;
			goto end;
		}

		buf = zmalloc(len);
		if (!buf) {
			ret = -ENOMEM;
			goto end;
		}

		ret = recv_data_sessiond(buf, len);
		if (ret < 0) {
			goto end;
		}

		if (!user_buf) {
			ret = -LTTNG_ERR_INVALID;
			goto end;
		}

		/* Move ownership of command header buffer to user. */
		*user_buf = buf;
		buf = NULL;
		*user_len = len;
	} else {
		/* No command header. */
		if (user_len) {
			*user_len = 0;
		}

		if (user_buf) {
			*user_buf = NULL;
		}
	}

end:
	free(buf);
	return ret;
}

/*
 * Ask the session daemon a specific command and put the data into buf.
 * Takes extra var. len. data and file descriptors as input to send to the
 * session daemon.
 *
 * Return size of data (only payload, not header) or a negative error code.
 */
LTTNG_HIDDEN
int lttng_ctl_ask_sessiond_fds_varlen(struct lttcomm_session_msg *lsm,
		const int *fds, size_t nb_fd, const void *vardata,
		size_t vardata_len, void **user_payload_buf,
		void **user_cmd_header_buf, size_t *user_cmd_header_len)
{
	int ret;
	size_t payload_len;
	struct lttcomm_lttng_msg llm;

	ret = connect_sessiond();
	if (ret < 0) {
		ret = -LTTNG_ERR_NO_SESSIOND;
		goto end;
	} else {
		sessiond_socket = ret;
		connected = 1;
	}

	/* Send command to session daemon */
	ret = send_session_msg(lsm);
	if (ret < 0) {
		/* Ret value is a valid lttng error code. */
		goto end;
	}
	/* Send var len data */
	ret = send_session_varlen(vardata, vardata_len);
	if (ret < 0) {
		/* Ret value is a valid lttng error code. */
		goto end;
	}

	/* Send fds */
	ret = send_session_fds(fds, nb_fd);
	if (ret < 0) {
		/* Ret value is a valid lttng error code. */
		goto end;
	}

	/* Get header from data transmission */
	ret = recv_data_sessiond(&llm, sizeof(llm));
	if (ret < 0) {
		/* Ret value is a valid lttng error code. */
		goto end;
	}

	/* Check error code if OK */
	if (llm.ret_code != LTTNG_OK) {
		ret = -llm.ret_code;
		goto end;
	}

	/* Get command header from data transmission */
	ret = recv_sessiond_optional_data(llm.cmd_header_size,
		user_cmd_header_buf, user_cmd_header_len);
	if (ret < 0) {
		goto end;
	}

	/* Get payload from data transmission */
	ret = recv_sessiond_optional_data(llm.data_size, user_payload_buf,
		&payload_len);
	if (ret < 0) {
		goto end;
	}

	ret = llm.data_size;

end:
	disconnect_sessiond();
	return ret;
}

/*
 * Create lttng handle and return pointer.
 *
 * The returned pointer will be NULL in case of malloc() error.
 */
struct lttng_handle *lttng_create_handle(const char *session_name,
		struct lttng_domain *domain)
{
	int ret;
	struct lttng_handle *handle = NULL;

	handle = zmalloc(sizeof(struct lttng_handle));
	if (handle == NULL) {
		PERROR("malloc handle");
		goto end;
	}

	/* Copy session name */
	ret = lttng_strncpy(handle->session_name, session_name ? : "",
			    sizeof(handle->session_name));
	if (ret) {
		goto error;
	}

	/* Copy lttng domain or leave initialized to 0. */
	if (domain) {
		lttng_ctl_copy_lttng_domain(&handle->domain, domain);
	}

end:
	return handle;
error:
	free(handle);
	return NULL;
}

/*
 * Destroy handle by free(3) the pointer.
 */
void lttng_destroy_handle(struct lttng_handle *handle)
{
	free(handle);
}

/*
 * Register an outside consumer.
 *
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_register_consumer(struct lttng_handle *handle,
		const char *socket_path)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (handle == NULL || socket_path == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_REGISTER_CONSUMER;
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_strncpy(lsm.u.reg.path, socket_path,
			    sizeof(lsm.u.reg.path));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
end:
	return ret;
}

/*
 * Start tracing for all traces of the session.
 *
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_start_tracing(const char *session_name)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (session_name == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_START_TRACE;

	ret = lttng_strncpy(lsm.session.name, session_name,
			    sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
end:
	return ret;
}

/*
 * Stop tracing for all traces of the session.
 */
static int _lttng_stop_tracing(const char *session_name, int wait)
{
	int ret, data_ret;
	struct lttcomm_session_msg lsm;

	if (session_name == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_STOP_TRACE;

	ret = lttng_strncpy(lsm.session.name, session_name,
			    sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
	if (ret < 0 && ret != -LTTNG_ERR_TRACE_ALREADY_STOPPED) {
		goto error;
	}

	if (!wait) {
		goto end;
	}

	/* Check for data availability */
	do {
		data_ret = lttng_data_pending(session_name);
		if (data_ret < 0) {
			/* Return the data available call error. */
			ret = data_ret;
			goto error;
		}

		/*
		 * Data sleep time before retrying (in usec). Don't sleep if the
		 * call returned value indicates availability.
		 */
		if (data_ret) {
			usleep(DEFAULT_DATA_AVAILABILITY_WAIT_TIME_US);
		}
	} while (data_ret != 0);

end:
error:
	return ret;
}

/*
 * Stop tracing and wait for data availability.
 */
int lttng_stop_tracing(const char *session_name)
{
	return _lttng_stop_tracing(session_name, 1);
}

/*
 * Stop tracing but _don't_ wait for data availability.
 */
int lttng_stop_tracing_no_wait(const char *session_name)
{
	return _lttng_stop_tracing(session_name, 0);
}

/*
 * Add context to a channel.
 *
 * If the given channel is NULL, add the contexts to all channels.
 * The event_name param is ignored.
 *
 * Returns the size of the returned payload data or a negative error code.
 */
int lttng_add_context(struct lttng_handle *handle,
		struct lttng_event_context *ctx, const char *event_name,
		const char *channel_name)
{
	int ret;
	struct lttcomm_session_msg lsm;
	struct lttng_dynamic_buffer buffer;

	lttng_dynamic_buffer_init(&buffer);

	/* Safety check. Both are mandatory. */
	if (handle == NULL || ctx == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ADD_CONTEXT;

	/* If no channel name, send empty string. */
	ret = lttng_strncpy(lsm.u.context.channel_name, channel_name ?: "",
			sizeof(lsm.u.context.channel_name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_event_context_serialize(ctx, &buffer);
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	lsm.u.context.length = buffer.size;

	ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(
			&lsm, buffer.data, buffer.size, NULL);
end:
	lttng_dynamic_buffer_reset(&buffer);
	return ret;
}

/*
 * Enable event(s) for a channel.
 *
 * If no event name is specified, all events are enabled.
 * If no channel name is specified, the default 'channel0' is used.
 *
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_enable_event(struct lttng_handle *handle,
		struct lttng_event *ev, const char *channel_name)
{
	return lttng_enable_event_with_exclusions(handle, ev, channel_name,
			NULL, 0, NULL);
}

/*
 * Create or enable an event with a filter expression.
 *
 * Return negative error value on error.
 * Return size of returned session payload data if OK.
 */
int lttng_enable_event_with_filter(struct lttng_handle *handle,
		struct lttng_event *event, const char *channel_name,
		const char *filter_expression)
{
	return lttng_enable_event_with_exclusions(handle, event, channel_name,
			filter_expression, 0, NULL);
}

/*
 * Depending on the event, return a newly allocated agent filter expression or
 * NULL if not applicable.
 *
 * An event with NO loglevel and the name is * will return NULL.
 */
static char *set_agent_filter(const char *filter, struct lttng_event *ev)
{
	int err;
	char *agent_filter = NULL;

	assert(ev);

	/* Don't add filter for the '*' event. */
	if (strcmp(ev->name, "*") != 0) {
		if (filter) {
			err = asprintf(&agent_filter, "(%s) && (logger_name == \"%s\")", filter,
					ev->name);
		} else {
			err = asprintf(&agent_filter, "logger_name == \"%s\"", ev->name);
		}
		if (err < 0) {
			PERROR("asprintf");
			goto error;
		}
	}

	/* Add loglevel filtering if any for the JUL domain. */
	if (ev->loglevel_type != LTTNG_EVENT_LOGLEVEL_ALL) {
		const char *op;

		if (ev->loglevel_type == LTTNG_EVENT_LOGLEVEL_RANGE) {
			op = ">=";
		} else {
			op = "==";
		}

		if (filter || agent_filter) {
			char *new_filter;

			err = asprintf(&new_filter, "(%s) && (int_loglevel %s %d)",
					agent_filter ? agent_filter : filter, op,
					ev->loglevel);
			if (agent_filter) {
				free(agent_filter);
			}
			agent_filter = new_filter;
		} else {
			err = asprintf(&agent_filter, "int_loglevel %s %d", op,
					ev->loglevel);
		}
		if (err < 0) {
			PERROR("asprintf");
			goto error;
		}
	}

	return agent_filter;
error:
	free(agent_filter);
	return NULL;
}

/*
 * Generate the filter bytecode from a given filter expression string. Put the
 * newly allocated parser context in ctxp and populate the lsm object with the
 * expression len.
 *
 * Return 0 on success else a LTTNG_ERR_* code and ctxp is untouched.
 */
static int generate_filter(char *filter_expression,
		size_t *bytecode_len,
		struct filter_parser_ctx **ctxp)
{
	int ret;
	struct filter_parser_ctx *ctx = NULL;
	FILE *fmem = NULL;

	assert(filter_expression);
	assert(ctxp);

	/*
	 * Casting const to non-const, as the underlying function will use it in
	 * read-only mode.
	 */
	fmem = lttng_fmemopen((void *) filter_expression,
			strlen(filter_expression), "r");
	if (!fmem) {
		fprintf(stderr, "Error opening memory as stream\n");
		ret = -LTTNG_ERR_FILTER_NOMEM;
		goto error;
	}
	ctx = filter_parser_ctx_alloc(fmem);
	if (!ctx) {
		fprintf(stderr, "Error allocating parser\n");
		ret = -LTTNG_ERR_FILTER_NOMEM;
		goto filter_alloc_error;
	}
	ret = filter_parser_ctx_append_ast(ctx);
	if (ret) {
		fprintf(stderr, "Parse error\n");
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}
	if (print_xml) {
		ret = filter_visitor_print_xml(ctx, stdout, 0);
		if (ret) {
			fflush(stdout);
			fprintf(stderr, "XML print error\n");
			ret = -LTTNG_ERR_FILTER_INVAL;
			goto parse_error;
		}
	}

	dbg_printf("Generating IR... ");
	fflush(stdout);
	ret = filter_visitor_ir_generate(ctx);
	if (ret) {
		fprintf(stderr, "Generate IR error\n");
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}
	dbg_printf("done\n");

	dbg_printf("Validating IR... ");
	fflush(stdout);
	ret = filter_visitor_ir_check_binary_op_nesting(ctx);
	if (ret) {
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}

	/* Normalize globbing patterns in the expression. */
	ret = filter_visitor_ir_normalize_glob_patterns(ctx);
	if (ret) {
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}

	/* Validate strings used as literals in the expression. */
	ret = filter_visitor_ir_validate_string(ctx);
	if (ret) {
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}

	/* Validate globbing patterns in the expression. */
	ret = filter_visitor_ir_validate_globbing(ctx);
	if (ret) {
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}

	dbg_printf("done\n");

	dbg_printf("Generating bytecode... ");
	fflush(stdout);
	ret = filter_visitor_bytecode_generate(ctx);
	if (ret) {
		fprintf(stderr, "Generate bytecode error\n");
		ret = -LTTNG_ERR_FILTER_INVAL;
		goto parse_error;
	}
	dbg_printf("done\n");
	dbg_printf("Size of bytecode generated: %u bytes.\n",
			bytecode_get_len(&ctx->bytecode->b));

	*bytecode_len = sizeof(ctx->bytecode->b) +
			bytecode_get_len(&ctx->bytecode->b);

	/* No need to keep the memory stream. */
	if (fclose(fmem) != 0) {
		PERROR("fclose");
	}

	*ctxp = ctx;
	return 0;

parse_error:
	filter_ir_free(ctx);
	filter_parser_ctx_free(ctx);
filter_alloc_error:
	if (fclose(fmem) != 0) {
		PERROR("fclose");
	}
error:
	return ret;
}

/*
 * Enable event(s) for a channel, possibly with exclusions and a filter.
 * If no event name is specified, all events are enabled.
 * If no channel name is specified, the default name is used.
 * If filter expression is not NULL, the filter is set for the event.
 * If exclusion count is not zero, the exclusions are set for the event.
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_enable_event_with_exclusions(struct lttng_handle *handle,
		struct lttng_event *ev, const char *channel_name,
		const char *original_filter_expression,
		int exclusion_count, char **exclusion_list)
{
	struct lttcomm_session_msg lsm;
	struct lttng_dynamic_buffer send_buffer;
	int ret = 0, fd_to_send = -1;
	bool send_fd = false;
	unsigned int free_filter_expression = 0;
	struct filter_parser_ctx *ctx = NULL;
	size_t bytecode_len = 0;

	/*
	 * We have either a filter or some exclusions, so we need to set up
	 * a variable-length memory block from where to send the data.
	 */
	lttng_dynamic_buffer_init(&send_buffer);

	/*
	 * Cast as non-const since we may replace the filter expression
	 * by a dynamically allocated string. Otherwise, the original
	 * string is not modified.
	 */
	char *filter_expression = (char *) original_filter_expression;

	if (handle == NULL || ev == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/*
	 * Empty filter string will always be rejected by the parser
	 * anyway, so treat this corner-case early to eliminate
	 * lttng_fmemopen error for 0-byte allocation.
	 */
	if (filter_expression && filter_expression[0] == '\0') {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	if (ev->name[0] == '\0') {
		/* Enable all events. */
		ret = lttng_strncpy(ev->name, "*", sizeof(ev->name));
		assert(ret == 0);
	}

	/* Parse filter expression. */
	if (filter_expression != NULL || handle->domain.type == LTTNG_DOMAIN_JUL
			|| handle->domain.type == LTTNG_DOMAIN_LOG4J
			|| handle->domain.type == LTTNG_DOMAIN_PYTHON) {
		if (handle->domain.type == LTTNG_DOMAIN_JUL ||
				handle->domain.type == LTTNG_DOMAIN_LOG4J ||
				handle->domain.type == LTTNG_DOMAIN_PYTHON) {
			char *agent_filter;

			/* Setup agent filter if needed. */
			agent_filter = set_agent_filter(filter_expression, ev);
			if (!agent_filter) {
				if (!filter_expression) {
					/*
					 * No JUL and no filter, just skip
					 * everything below.
					 */
					goto serialize;
				}
			} else {
				/*
				 * With an agent filter, the original filter has
				 * been added to it thus replace the filter
				 * expression.
				 */
				filter_expression = agent_filter;
				free_filter_expression = 1;
			}
		}

		if (strnlen(filter_expression, LTTNG_FILTER_MAX_LEN) ==
				LTTNG_FILTER_MAX_LEN) {
			ret = -LTTNG_ERR_FILTER_INVAL;
			goto error;
		}

		ret = generate_filter(filter_expression, &bytecode_len, &ctx);
		if (ret) {
			goto error;
		}

		if (bytecode_len > LTTNG_FILTER_MAX_LEN) {
			ret = -LTTNG_ERR_FILTER_INVAL;
			goto error;
		}
	}

serialize:
	ret = lttng_event_serialize(ev, exclusion_count, exclusion_list,
			filter_expression, bytecode_len,
			ctx && (bytecode_len != 0) ? &ctx->bytecode->b : NULL,
			&send_buffer, &fd_to_send);
	if (ret) {
		goto error;
	}

	if (fd_to_send >= 0) {
		send_fd = true;
	}

	/* Prepare the command header */
	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ENABLE_EVENT;

	/* If no channel name, send empty string. */
	ret = lttng_strncpy(lsm.u.enable.channel_name, channel_name ?: "",
			sizeof(lsm.u.enable.channel_name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/* Domain */
	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	/* Session name */
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/* Length of the serialized event */
	lsm.u.enable.length = (uint32_t) send_buffer.size;

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm,
			send_fd ? &fd_to_send : NULL,
			send_fd ? 1 : 0,
			send_buffer.size ? send_buffer.data : NULL,
			send_buffer.size, NULL, NULL, 0);
error:
	if (filter_expression && ctx) {
		filter_bytecode_free(ctx);
		filter_ir_free(ctx);
		filter_parser_ctx_free(ctx);
	}

	if (free_filter_expression) {
		/*
		 * The filter expression has been replaced and must be freed as
		 * it is not the original filter expression received as a
		 * parameter.
		 */
		free(filter_expression);
	}

	lttng_dynamic_buffer_reset(&send_buffer);
	return ret;
}

int lttng_disable_event_ext(struct lttng_handle *handle,
		struct lttng_event *ev, const char *channel_name,
		const char *original_filter_expression)
{
	struct lttcomm_session_msg lsm;
	struct lttng_dynamic_buffer buf;
	int ret = 0;
	unsigned int free_filter_expression = 0;
	struct filter_parser_ctx *ctx = NULL;
	size_t bytecode_len = 0;
	int fd_to_send = -1;
	bool send_fd = false;
	/*
	 * Cast as non-const since we may replace the filter expression
	 * by a dynamically allocated string. Otherwise, the original
	 * string is not modified.
	 */
	char *filter_expression = (char *) original_filter_expression;

	lttng_dynamic_buffer_init(&buf);

	if (handle == NULL || ev == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/*
	 * Empty filter string will always be rejected by the parser
	 * anyway, so treat this corner-case early to eliminate
	 * lttng_fmemopen error for 0-byte allocation.
	 */
	if (filter_expression && filter_expression[0] == '\0') {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/* Parse filter expression */
	if (filter_expression != NULL || handle->domain.type == LTTNG_DOMAIN_JUL
			|| handle->domain.type == LTTNG_DOMAIN_LOG4J
			|| handle->domain.type == LTTNG_DOMAIN_PYTHON) {
		if (handle->domain.type == LTTNG_DOMAIN_JUL ||
				handle->domain.type == LTTNG_DOMAIN_LOG4J ||
				handle->domain.type == LTTNG_DOMAIN_PYTHON) {
			char *agent_filter;

			/* Setup agent filter if needed. */
			agent_filter = set_agent_filter(filter_expression, ev);
			if (!agent_filter) {
				if (!filter_expression) {
					/*
					 * No JUL and no filter, just skip
					 * everything below.
					 */
					goto serialize;
				}
			} else {
				/*
				 * With a JUL filter, the original filter has
				 * been added to it thus replace the filter
				 * expression.
				 */
				filter_expression = agent_filter;
				free_filter_expression = 1;
			}
		}

		ret = generate_filter(filter_expression, &bytecode_len, &ctx);
		if (ret) {
			ret = -1;
			goto error;
		}
	}

serialize:
	ret = lttng_event_serialize(ev, 0, NULL, filter_expression,
			bytecode_len,
			ctx && (bytecode_len != 0) ? &ctx->bytecode->b : NULL,
			&buf, &fd_to_send);
	if (ret) {
		ret = -1;
		goto error;
	}

	if (fd_to_send >= 0) {
		send_fd = true;
	}

	/* Prepare command header */
	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_DISABLE_EVENT;

	/* If no channel name, send empty string. */
	ret = lttng_strncpy(lsm.u.disable.channel_name, channel_name ?: "",
			sizeof(lsm.u.disable.channel_name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}
	/* Domain */
	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	/* Session name */
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	/* Length of the serialized event */
	lsm.u.disable.length = (uint32_t) buf.size;

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm,
			send_fd ? &fd_to_send : NULL, send_fd ? 1 : 0,
			buf.size ? buf.data : NULL, buf.size, NULL, NULL, 0);

error:
	if (filter_expression && ctx) {
		filter_bytecode_free(ctx);
		filter_ir_free(ctx);
		filter_parser_ctx_free(ctx);
	}

	if (free_filter_expression) {
		/*
		 * The filter expression has been replaced and must be freed as
		 * it is not the original filter expression received as a
		 * parameter.
		 */
		free(filter_expression);
	}

	lttng_dynamic_buffer_reset(&buf);
	return ret;
}

/*
 * Disable event(s) of a channel and domain.
 * If no event name is specified, all events are disabled.
 * If no channel name is specified, the default 'channel0' is used.
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_disable_event(struct lttng_handle *handle, const char *name,
		const char *channel_name)
{
	int ret;
	struct lttng_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.loglevel = -1;
	ev.type = LTTNG_EVENT_ALL;
	ret = lttng_strncpy(ev.name, name ?: "", sizeof(ev.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_disable_event_ext(handle, &ev, channel_name, NULL);
end:
	return ret;
}

struct lttng_channel *lttng_channel_create(struct lttng_domain *domain)
{
	struct lttng_channel *channel = NULL;

	if (!domain) {
		goto end;
	}

	/* Validate domain. */
	switch (domain->type) {
	case LTTNG_DOMAIN_UST:
		switch (domain->buf_type) {
		case LTTNG_BUFFER_PER_UID:
		case LTTNG_BUFFER_PER_PID:
			break;
		default:
			goto end;
		}
		break;
	case LTTNG_DOMAIN_KERNEL:
		if (domain->buf_type != LTTNG_BUFFER_GLOBAL) {
			goto end;
		}
		break;
	default:
		goto end;
	}

	channel = lttng_channel_create_internal();
	if (!channel) {
		goto end;
	}

	lttng_channel_set_default_attr(domain, &channel->attr);
end:
	return channel;
}

void lttng_channel_destroy(struct lttng_channel *channel)
{
	if (!channel) {
		return;
	}

	if (channel->attr.extended.ptr) {
		free(channel->attr.extended.ptr);
	}
	free(channel);
}

/*
 * Enable channel per domain
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_enable_channel(struct lttng_handle *handle,
		struct lttng_channel *in_chan)
{
	enum lttng_error_code ret_code;
	int ret;
	struct lttng_dynamic_buffer buffer;
	struct lttcomm_session_msg lsm;
	uint64_t total_buffer_size_needed_per_cpu = 0;
	struct lttng_channel *channel = NULL;

	lttng_dynamic_buffer_init(&buffer);

	/* NULL arguments are forbidden. No default values. */
	if (handle == NULL || in_chan == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	/*
	 * Verify that the amount of memory required to create the requested
	 * buffer is available on the system at the moment.
	 */
	if (in_chan->attr.num_subbuf >
			UINT64_MAX / in_chan->attr.subbuf_size) {
		/* Overflow */
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	total_buffer_size_needed_per_cpu =
			in_chan->attr.num_subbuf * in_chan->attr.subbuf_size;
	ret_code = check_enough_available_memory(
			total_buffer_size_needed_per_cpu);
	if (ret_code != LTTNG_OK) {
		ret = -ret_code;
		goto end;
	}

	/* Copy the channel for easier manipulation. */
	channel = lttng_channel_copy(in_chan);
	if (!channel) {
		ret = -LTTNG_ERR_NOMEM;
		goto end;
	}

	/* Populate the channel extended attribute if necessary. */
	if (!channel->attr.extended.ptr) {
		struct lttng_channel_extended *extended =
				zmalloc(sizeof(*extended));

		if (!extended) {
			ret = -LTTNG_ERR_NOMEM;
			goto end;
		}
		lttng_channel_set_default_extended_attr(
				&handle->domain, extended);
		channel->attr.extended.ptr = extended;
	}

	/* Prepare the payload */
	memset(&lsm, 0, sizeof(lsm));

	lsm.cmd_type = LTTNG_ENABLE_CHANNEL;
	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_strncpy(lsm.session.name, handle->session_name,
				    sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_channel_serialize(channel, &buffer);
	if (ret) {
		ret = -LTTNG_ERR_FATAL;
		goto end;
	}

	lsm.u.channel.length = buffer.size;

	ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(
			&lsm, buffer.data, buffer.size, NULL);
end:
	lttng_channel_destroy(channel);
	lttng_dynamic_buffer_reset(&buffer);
	return ret;
}

/*
 * All tracing will be stopped for registered events of the channel.
 * Returns size of returned session payload data or a negative error code.
 */
int lttng_disable_channel(struct lttng_handle *handle, const char *name)
{
	int ret;
	struct lttcomm_session_msg lsm;

	/* Safety check. Both are mandatory. */
	if (handle == NULL || name == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	memset(&lsm, 0, sizeof(lsm));

	lsm.cmd_type = LTTNG_DISABLE_CHANNEL;

	ret = lttng_strncpy(lsm.u.disable.channel_name, name,
			sizeof(lsm.u.disable.channel_name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			    sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
end:
	return ret;
}

/*
 * Lists all available tracepoints of domain.
 * Sets the contents of the events array.
 * Returns the number of lttng_event entries in events;
 * on error, returns a negative value.
 */
int lttng_list_tracepoints(struct lttng_handle *handle,
		struct lttng_event **events)
{
	enum lttng_error_code ret_code;
	int ret, total_payload_received;
	char *reception_buffer = NULL;
	struct lttcomm_session_msg lsm;
	struct lttcomm_list_command_header *cmd_header = NULL;
	size_t cmd_header_len;
	unsigned int nb_events = 0;

	if (handle == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_TRACEPOINTS;
	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm, NULL, 0, NULL, 0,
			(void **) &reception_buffer, (void **) &cmd_header,
			&cmd_header_len);
	if (ret < 0) {
		goto end;
	}
	total_payload_received = ret;

	if (!cmd_header) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	if (cmd_header->count > INT_MAX) {
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	nb_events = (unsigned int) cmd_header->count;

	{
		const struct lttng_buffer_view events_view =
				lttng_buffer_view_init(reception_buffer, 0,
						total_payload_received);

		ret_code = lttng_events_create_and_flatten_from_buffer(
				&events_view, nb_events, events);
		if (ret_code != LTTNG_OK) {
			ret = -ret_code;
			goto end;
		}
	}

	ret = (int) nb_events;

end:
	free(cmd_header);
	free(reception_buffer);
	return ret;
}

/*
 * Lists all available tracepoint fields of domain.
 * Sets the contents of the event field array.
 * Returns the number of lttng_event_field entries in events;
 * on error, returns a negative value.
 */
int lttng_list_tracepoint_fields(struct lttng_handle *handle,
		struct lttng_event_field **fields)
{
	enum lttng_error_code ret_code;
	int ret, total_payload_received;
	struct lttcomm_session_msg lsm;
	char *reception_buffer = NULL;
	struct lttcomm_list_command_header *cmd_header = NULL;
	size_t cmd_header_len;
	unsigned int nb_event_fields = 0;

	if (handle == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_TRACEPOINT_FIELDS;
	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm, NULL, 0, NULL, 0,
			(void **) &reception_buffer, (void **) &cmd_header,
			&cmd_header_len);
	if (ret < 0) {
		goto end;
	}

	total_payload_received = ret;

	if (!cmd_header) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	if (cmd_header->count > INT_MAX) {
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	nb_event_fields = cmd_header->count;

	{
		const struct lttng_buffer_view view =
			lttng_buffer_view_init(reception_buffer, 0, total_payload_received);

		ret_code = lttng_event_fields_create_and_flatten_from_buffer(
				&view, nb_event_fields, fields);
		if (ret_code != LTTNG_OK) {
			ret = -ret_code;
			goto end;
		}
	}

	ret = nb_event_fields;

end:
	free(cmd_header);
	free(reception_buffer);
	return ret;
}

/*
 * Lists all available kernel system calls. Allocates and sets the contents of
 * the events array.
 *
 * Returns the number of lttng_event entries in events; on error, returns a
 * negative value.
 */
int lttng_list_syscalls(struct lttng_event **events)
{
	enum lttng_error_code ret_code;
	int ret, total_payload_received;
	char *reception_buffer = NULL;
	struct lttcomm_session_msg lsm;
	struct lttcomm_list_command_header *cmd_header = NULL;
	size_t cmd_header_len;
	uint32_t nb_events = 0;

	if (!events) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_SYSCALLS;
	/* Force kernel domain for system calls. */
	lsm.domain.type = LTTNG_DOMAIN_KERNEL;

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm, NULL, 0, NULL, 0,
			(void **) &reception_buffer, (void **) &cmd_header,
			&cmd_header_len);
	if (ret < 0) {
		goto end;
	}
	total_payload_received = ret;

	if (!cmd_header) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	if (cmd_header->count > INT_MAX) {
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	nb_events = (unsigned int) cmd_header->count;

	{
		const struct lttng_buffer_view events_view =
				lttng_buffer_view_init(reception_buffer, 0,
						total_payload_received);

		ret_code = lttng_events_create_and_flatten_from_buffer(
				&events_view, nb_events, events);
		if (ret_code != LTTNG_OK) {
			ret = -ret_code;
			goto end;
		}
	}

	ret = (int) nb_events;

end:
	free(reception_buffer);
	free(cmd_header);
	return ret;
}

/*
 * Returns a human readable string describing
 * the error code (a negative value).
 */
const char *lttng_strerror(int code)
{
	return error_get_str(code);
}

enum lttng_error_code lttng_create_session_ext(
		struct lttng_session_descriptor *session_descriptor)
{
	enum lttng_error_code ret_code;
	struct lttcomm_session_msg lsm = {
		.cmd_type = LTTNG_CREATE_SESSION_EXT,
	};
	void *reply = NULL;
	struct lttng_buffer_view reply_view;
	int reply_ret;
	bool sessiond_must_generate_ouput;
	struct lttng_dynamic_buffer payload;
	int ret;
	size_t descriptor_size;
	struct lttng_session_descriptor *descriptor_reply = NULL;

	lttng_dynamic_buffer_init(&payload);
	if (!session_descriptor) {
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}

	sessiond_must_generate_ouput =
			!lttng_session_descriptor_is_output_destination_initialized(
				session_descriptor);
	if (sessiond_must_generate_ouput) {
		const char *home_dir = utils_get_home_dir();
		size_t home_dir_len = home_dir ? strlen(home_dir) + 1 : 0;

		if (!home_dir || home_dir_len > LTTNG_PATH_MAX) {
			ret_code = LTTNG_ERR_FATAL;
			goto end;
		}

		lsm.u.create_session.home_dir_size = (uint16_t) home_dir_len;
		ret = lttng_dynamic_buffer_append(&payload, home_dir,
				home_dir_len);
		if (ret) {
			ret_code = LTTNG_ERR_NOMEM;
			goto end;
		}
	}

	descriptor_size = payload.size;
	ret = lttng_session_descriptor_serialize(session_descriptor,
			&payload);
	if (ret) {
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}
	descriptor_size = payload.size - descriptor_size;
	lsm.u.create_session.session_descriptor_size = descriptor_size;

	/* Command returns a session descriptor on success. */
	reply_ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(&lsm, payload.data,
			payload.size, &reply);
	if (reply_ret < 0) {
		ret_code = -reply_ret;
		goto end;
	} else if (reply_ret == 0) {
		/* Socket unexpectedly closed by the session daemon. */
		ret_code = LTTNG_ERR_FATAL;
		goto end;
	}

	reply_view = lttng_buffer_view_init(reply, 0, reply_ret);
	ret = lttng_session_descriptor_create_from_buffer(&reply_view,
			&descriptor_reply);
	if (ret < 0) {
		ret_code = LTTNG_ERR_FATAL;
		goto end;
	}
	ret_code = LTTNG_OK;
	lttng_session_descriptor_assign(session_descriptor, descriptor_reply);
end:
	free(reply);
	lttng_dynamic_buffer_reset(&payload);
	lttng_session_descriptor_destroy(descriptor_reply);
	return ret_code;
}

/*
 * Create a new session using name and url for destination.
 *
 * Return 0 on success else a negative LTTng error code.
 */
int lttng_create_session(const char *name, const char *url)
{
	int ret;
	ssize_t size;
	struct lttng_uri *uris = NULL;
	struct lttng_session_descriptor *descriptor = NULL;
	enum lttng_error_code ret_code;

	if (!name) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	size = uri_parse_str_urls(url, NULL, &uris);
	if (size < 0) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	switch (size) {
	case 0:
		descriptor = lttng_session_descriptor_create(name);
		break;
	case 1:
		if (uris[0].dtype != LTTNG_DST_PATH) {
			ret = -LTTNG_ERR_INVALID;
			goto end;
		}
		descriptor = lttng_session_descriptor_local_create(name,
				uris[0].dst.path);
		break;
	case 2:
		descriptor = lttng_session_descriptor_network_create(name, url,
				NULL);
		break;
	default:
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	if (!descriptor) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	ret_code = lttng_create_session_ext(descriptor);
	ret = ret_code == LTTNG_OK ? 0 : -ret_code;
end:
	lttng_session_descriptor_destroy(descriptor);
	free(uris);
	return ret;
}

/*
 * Create a session exclusively used for snapshot.
 *
 * Return 0 on success else a negative LTTng error code.
 */
int lttng_create_session_snapshot(const char *name, const char *snapshot_url)
{
	int ret;
	enum lttng_error_code ret_code;
	ssize_t size;
	struct lttng_uri *uris = NULL;
	struct lttng_session_descriptor *descriptor = NULL;

	if (!name) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	size = uri_parse_str_urls(snapshot_url, NULL, &uris);
	if (size < 0) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	/*
	 * If the user does not specify a custom subdir, use the session name.
	 */
	if (size > 0 && uris[0].dtype != LTTNG_DST_PATH &&
			strlen(uris[0].subdir) == 0) {
		ret = snprintf(uris[0].subdir, sizeof(uris[0].subdir), "%s",
				name);
		if (ret < 0) {
			PERROR("Failed to set session name as network destination sub-directory");
			ret = -LTTNG_ERR_FATAL;
			goto end;
		} else if (ret >= sizeof(uris[0].subdir)) {
			/* Truncated output. */
			ret = -LTTNG_ERR_INVALID;
			goto end;
		}
	}

	switch (size) {
	case 0:
		descriptor = lttng_session_descriptor_snapshot_create(name);
		break;
	case 1:
		if (uris[0].dtype != LTTNG_DST_PATH) {
			ret = -LTTNG_ERR_INVALID;
			goto end;
		}
		descriptor = lttng_session_descriptor_snapshot_local_create(
				name,
				uris[0].dst.path);
		break;
	case 2:
		descriptor = lttng_session_descriptor_snapshot_network_create(
				name,
				snapshot_url,
				NULL);
		break;
	default:
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	if (!descriptor) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	ret_code = lttng_create_session_ext(descriptor);
	ret = ret_code == LTTNG_OK ? 0 : -ret_code;
end:
	lttng_session_descriptor_destroy(descriptor);
	free(uris);
	return ret;
}

/*
 * Create a session exclusively used for live.
 *
 * Return 0 on success else a negative LTTng error code.
 */
int lttng_create_session_live(const char *name, const char *url,
		unsigned int timer_interval)
{
	int ret;
	enum lttng_error_code ret_code;
	struct lttng_session_descriptor *descriptor = NULL;

	if (!name) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (url) {
		descriptor = lttng_session_descriptor_live_network_create(
				name, url, NULL, timer_interval);
	} else {
		descriptor = lttng_session_descriptor_live_create(
				name, timer_interval);
	}
	if (!descriptor) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}
	ret_code = lttng_create_session_ext(descriptor);
	ret = ret_code == LTTNG_OK ? 0 : -ret_code;
end:
	lttng_session_descriptor_destroy(descriptor);
	return ret;
}

/*
 * Stop the session and wait for the data before destroying it
 *
 * Return 0 on success else a negative LTTng error code.
 */
int lttng_destroy_session(const char *session_name)
{
	int ret;
	enum lttng_error_code ret_code;
	enum lttng_destruction_handle_status status;
	struct lttng_destruction_handle *handle = NULL;

	/*
	 * Stop the tracing and wait for the data to be
	 * consumed.
	 */
	ret = _lttng_stop_tracing(session_name, 1);
	if (ret && ret != -LTTNG_ERR_TRACE_ALREADY_STOPPED) {
		goto end;
	}

	ret_code = lttng_destroy_session_ext(session_name, &handle);
	if (ret_code != LTTNG_OK) {
		ret = (int) -ret_code;
		goto end;
	}
	assert(handle);

	/* Block until the completion of the destruction of the session. */
	status = lttng_destruction_handle_wait_for_completion(handle, -1);
	if (status != LTTNG_DESTRUCTION_HANDLE_STATUS_COMPLETED) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	status = lttng_destruction_handle_get_result(handle, &ret_code);
	if (status != LTTNG_DESTRUCTION_HANDLE_STATUS_OK) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}
	ret = ret_code == LTTNG_OK ? 0 : -ret_code;
end:
	lttng_destruction_handle_destroy(handle);
	return ret;
}

/*
 * Destroy the session without waiting for the data.
 */
int lttng_destroy_session_no_wait(const char *session_name)
{
	enum lttng_error_code ret_code;

	ret_code = lttng_destroy_session_ext(session_name, NULL);
	return ret_code == LTTNG_OK ? 0 : -ret_code;
}

/*
 * Ask the session daemon for all available sessions.
 * Sets the contents of the sessions array.
 * Returns the number of lttng_session entries in sessions;
 * on error, returns a negative value.
 */
int lttng_list_sessions(struct lttng_session **out_sessions)
{
	int ret;
	struct lttcomm_session_msg lsm;
	const size_t session_size = sizeof(struct lttng_session) +
			sizeof(struct lttng_session_extended);
	size_t session_count, i;
	struct lttng_session_extended *sessions_extended_begin;
	struct lttng_session *sessions = NULL;

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_SESSIONS;
	/*
	 * Initialize out_sessions to NULL so it is initialized when
	 * lttng_list_sessions returns 0, thus allowing *out_sessions to
	 * be subsequently freed.
	 */
	*out_sessions = NULL;
	ret = lttng_ctl_ask_sessiond(&lsm, (void**) &sessions);
	if (ret <= 0) {
		goto end;
	}
	if (!sessions) {
		ret = -LTTNG_ERR_FATAL;
		goto end;
	}

	if (ret % session_size) {
		ret = -LTTNG_ERR_UNK;
		free(sessions);
		goto end;
	}
	session_count = (size_t) ret / session_size;
	sessions_extended_begin = (struct lttng_session_extended *)
			(&sessions[session_count]);

	/* Set extended session info pointers. */
	for (i = 0; i < session_count; i++) {
		struct lttng_session *session = &sessions[i];
		struct lttng_session_extended *extended =
				&(sessions_extended_begin[i]);

		session->extended.ptr = extended;
	}

	ret = (int) session_count;
	*out_sessions = sessions;
end:
	return ret;
}

enum lttng_error_code lttng_session_get_creation_time(
		const struct lttng_session *session, uint64_t *creation_time)
{
	enum lttng_error_code ret = LTTNG_OK;
	struct lttng_session_extended *extended;

	if (!session || !creation_time || !session->extended.ptr) {
		ret = LTTNG_ERR_INVALID;
		goto end;
	}

	extended = session->extended.ptr;
	if (!extended->creation_time.is_set) {
		/* Not created on the session daemon yet. */
		ret = LTTNG_ERR_SESSION_NOT_EXIST;
		goto end;
	}
	*creation_time = extended->creation_time.value;
end:
	return ret;
}

int lttng_set_session_shm_path(const char *session_name,
		const char *shm_path)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (session_name == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_SET_SESSION_SHM_PATH;

	ret = lttng_strncpy(lsm.session.name, session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_strncpy(lsm.u.set_shm_path.shm_path, shm_path ?: "",
			sizeof(lsm.u.set_shm_path.shm_path));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
end:
	return ret;
}

/*
 * Ask the session daemon for all available domains of a session.
 * Sets the contents of the domains array.
 * Returns the number of lttng_domain entries in domains;
 * on error, returns a negative value.
 */
int lttng_list_domains(const char *session_name,
		struct lttng_domain **domains)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (session_name == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_DOMAINS;

	ret = lttng_strncpy(lsm.session.name, session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, (void**) domains);
	if (ret < 0) {
		goto error;
	}

	return ret / sizeof(struct lttng_domain);
error:
	return ret;
}

/*
 * Ask the session daemon for all available channels of a session.
 * Sets the contents of the channels array.
 * Returns the number of lttng_channel entries in channels;
 * on error, returns a negative value.
 */
int lttng_list_channels(struct lttng_handle *handle,
		struct lttng_channel **channels)
{
	int ret, total_payload_received;
	struct lttcomm_session_msg lsm;
	char *reception_buffer = NULL;
	size_t cmd_header_len = 0;
	struct lttcomm_list_command_header *cmd_header = NULL;
	struct lttng_dynamic_buffer tmp_buffer;

	lttng_dynamic_buffer_init(&tmp_buffer);

	if (handle == NULL) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_CHANNELS;
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm, NULL, 0, NULL, 0,
			(void **) &reception_buffer, (void **) &cmd_header,
			&cmd_header_len);
	if (ret < 0) {
		goto end;
	}

	total_payload_received = ret;

	if (cmd_header_len != sizeof(*cmd_header)) {
		ret = -LTTNG_ERR_FATAL;
		goto end;
	}

	if (!cmd_header) {
		ret = LTTNG_ERR_UNK;
		goto end;
	}

	if (cmd_header->count > INT_MAX) {
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	{
		enum lttng_error_code ret_code;
		const struct lttng_buffer_view events_view =
				lttng_buffer_view_init(reception_buffer, 0,
						total_payload_received);

		ret_code = lttng_channels_create_and_flatten_from_buffer(
				&events_view, cmd_header->count, channels);
		if (ret_code != LTTNG_OK) {
			ret = -ret_code;
			goto end;
		}
	}

	ret = (int) cmd_header->count;
end:
	free(cmd_header);
	free(reception_buffer);
	return ret;
}

/*
 * Ask the session daemon for all available events of a session channel.
 * Sets the contents of the events array.
 * Returns the number of lttng_event entries in events;
 * on error, returns a negative value.
 */
int lttng_list_events(struct lttng_handle *handle,
		const char *channel_name, struct lttng_event **events)
{
	enum lttng_error_code ret_code;
	int ret;
	struct lttcomm_session_msg lsm;
	struct lttcomm_list_command_header *cmd_header = NULL;
	size_t cmd_header_len;
	unsigned int nb_events;
	char *reception_buffer = NULL;
	struct lttng_dynamic_buffer tmp_buffer;
	ssize_t total_payload_received;

	lttng_dynamic_buffer_init(&tmp_buffer);

	/* Safety check. An handle and channel name are mandatory */
	if (handle == NULL || channel_name == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_LIST_EVENTS;
	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_strncpy(lsm.u.list.channel_name, channel_name,
			sizeof(lsm.u.list.channel_name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	ret = lttng_ctl_ask_sessiond_fds_varlen(&lsm, NULL, 0, NULL, 0,
		(void **) &reception_buffer, (void **) &cmd_header,
		&cmd_header_len);
	if (ret < 0) {
		goto end;
	}

	total_payload_received = ret;

	if (!cmd_header) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	if (cmd_header->count > INT_MAX) {
		ret = -LTTNG_ERR_OVERFLOW;
		goto end;
	}

	nb_events = (unsigned int) cmd_header->count;

	{
		const struct lttng_buffer_view events_view =
				lttng_buffer_view_init(reception_buffer, 0,
						total_payload_received);
		ret_code = lttng_events_create_and_flatten_from_buffer(
				&events_view, nb_events, events);
		if (ret_code != LTTNG_OK) {
			ret = -ret_code;
			goto end;
		}
	}

	ret = (int) nb_events;
end:
	free(cmd_header);
	free(reception_buffer);
	return ret;
}

/*
 * Sets the tracing_group variable with name.
 * This function allocates memory pointed to by tracing_group.
 * On success, returns 0, on error, returns -1 (null name) or -ENOMEM.
 */
int lttng_set_tracing_group(const char *name)
{
	char *new_group;
	if (name == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	if (asprintf(&new_group, "%s", name) < 0) {
		return -LTTNG_ERR_FATAL;
	}

	free(tracing_group);
	tracing_group = new_group;
	new_group = NULL;

	return 0;
}

int lttng_calibrate(struct lttng_handle *handle,
		struct lttng_calibrate *calibrate)
{
	/*
	 * This command was removed in LTTng 2.9.
	 */
	return -LTTNG_ERR_UND;
}

/*
 * Set default channel attributes.
 * If either or both of the arguments are null, attr content is zeroe'd.
 */
void lttng_channel_set_default_attr(struct lttng_domain *domain,
		struct lttng_channel_attr *attr)
{
	struct lttng_channel_extended *extended;

	/* Safety check */
	if (attr == NULL || domain == NULL) {
		return;
	}

	/* Save the pointer for later use */
	extended = (struct lttng_channel_extended *) attr->extended.ptr;
	memset(attr, 0, sizeof(struct lttng_channel_attr));

	/* Same for all domains. */
	attr->overwrite = DEFAULT_CHANNEL_OVERWRITE;
	attr->tracefile_size = DEFAULT_CHANNEL_TRACEFILE_SIZE;
	attr->tracefile_count = DEFAULT_CHANNEL_TRACEFILE_COUNT;

	switch (domain->type) {
	case LTTNG_DOMAIN_KERNEL:
		attr->switch_timer_interval =
				DEFAULT_KERNEL_CHANNEL_SWITCH_TIMER;
		attr->read_timer_interval = DEFAULT_KERNEL_CHANNEL_READ_TIMER;
		attr->subbuf_size = default_get_kernel_channel_subbuf_size();
		attr->num_subbuf = DEFAULT_KERNEL_CHANNEL_SUBBUF_NUM;
		attr->output = DEFAULT_KERNEL_CHANNEL_OUTPUT;
		break;
	case LTTNG_DOMAIN_UST:
		switch (domain->buf_type) {
		case LTTNG_BUFFER_PER_UID:
			attr->subbuf_size = default_get_ust_uid_channel_subbuf_size();
			attr->num_subbuf = DEFAULT_UST_UID_CHANNEL_SUBBUF_NUM;
			attr->output = DEFAULT_UST_UID_CHANNEL_OUTPUT;
			attr->switch_timer_interval =
					DEFAULT_UST_UID_CHANNEL_SWITCH_TIMER;
			attr->read_timer_interval =
					DEFAULT_UST_UID_CHANNEL_READ_TIMER;
			break;
		case LTTNG_BUFFER_PER_PID:
		default:
			attr->subbuf_size = default_get_ust_pid_channel_subbuf_size();
			attr->num_subbuf = DEFAULT_UST_PID_CHANNEL_SUBBUF_NUM;
			attr->output = DEFAULT_UST_PID_CHANNEL_OUTPUT;
			attr->switch_timer_interval =
					DEFAULT_UST_PID_CHANNEL_SWITCH_TIMER;
			attr->read_timer_interval =
					DEFAULT_UST_PID_CHANNEL_READ_TIMER;
			break;
		}
	default:
		/* Default behavior: leave set to 0. */
		break;
	}

	if (extended) {
		lttng_channel_set_default_extended_attr(domain, extended);
	}

	/* Reassign the extended pointer. */
	attr->extended.ptr = extended;
}

int lttng_channel_get_discarded_event_count(struct lttng_channel *channel,
		uint64_t *discarded_events)
{
	int ret = 0;
	struct lttng_channel_extended *chan_ext;

	if (!channel || !discarded_events) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	chan_ext = channel->attr.extended.ptr;
	if (!chan_ext) {
		/*
		 * This can happen since the lttng_channel structure is
		 * used for other tasks where this pointer is never set.
		 */
		*discarded_events = 0;
		goto end;
	}

	*discarded_events = chan_ext->discarded_events;
end:
	return ret;
}

int lttng_channel_get_lost_packet_count(struct lttng_channel *channel,
		uint64_t *lost_packets)
{
	int ret = 0;
	struct lttng_channel_extended *chan_ext;

	if (!channel || !lost_packets) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	chan_ext = channel->attr.extended.ptr;
	if (!chan_ext) {
		/*
		 * This can happen since the lttng_channel structure is
		 * used for other tasks where this pointer is never set.
		 */
		*lost_packets = 0;
		goto end;
	}

	*lost_packets = chan_ext->lost_packets;
end:
	return ret;
}

int lttng_channel_get_monitor_timer_interval(struct lttng_channel *chan,
		uint64_t *monitor_timer_interval)
{
	int ret = 0;

	if (!chan || !monitor_timer_interval) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (!chan->attr.extended.ptr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	*monitor_timer_interval = ((struct lttng_channel_extended *)
			chan->attr.extended.ptr)->monitor_timer_interval;
end:
	return ret;
}

int lttng_channel_set_monitor_timer_interval(struct lttng_channel *chan,
		uint64_t monitor_timer_interval)
{
	int ret = 0;

	if (!chan || !chan->attr.extended.ptr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	((struct lttng_channel_extended *)
			chan->attr.extended.ptr)->monitor_timer_interval =
			monitor_timer_interval;
end:
	return ret;
}

int lttng_channel_get_blocking_timeout(struct lttng_channel *chan,
		int64_t *blocking_timeout)
{
	int ret = 0;

	if (!chan || !blocking_timeout) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (!chan->attr.extended.ptr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	*blocking_timeout = ((struct lttng_channel_extended *)
			chan->attr.extended.ptr)->blocking_timeout;
end:
	return ret;
}

int lttng_channel_set_blocking_timeout(struct lttng_channel *chan,
		int64_t blocking_timeout)
{
	int ret = 0;
	int64_t msec_timeout;

	if (!chan || !chan->attr.extended.ptr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (blocking_timeout < 0 && blocking_timeout != -1) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	/*
	 * LTTng-ust's use of poll() to implement this timeout mechanism forces
	 * us to accept a narrower range of values (msecs expressed as a signed
	 * 32-bit integer).
	 */
	msec_timeout = blocking_timeout / 1000;
	if (msec_timeout != (int32_t) msec_timeout) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	((struct lttng_channel_extended *)
			chan->attr.extended.ptr)->blocking_timeout =
			blocking_timeout;
end:
	return ret;
}

/*
 * Check if session daemon is alive.
 *
 * Return 1 if alive or 0 if not.
 * On error returns a negative value.
 */
int lttng_session_daemon_alive(void)
{
	int ret;

	ret = set_session_daemon_path();
	if (ret < 0) {
		/* Error. */
		return ret;
	}

	if (*sessiond_sock_path == '\0') {
		/*
		 * No socket path set. Weird error which means the constructor
		 * was not called.
		 */
		assert(0);
	}

	ret = try_connect_sessiond(sessiond_sock_path);
	if (ret < 0) {
		/* Not alive. */
		return 0;
	}

	/* Is alive. */
	return 1;
}

/*
 * Set URL for a consumer for a session and domain.
 *
 * Return 0 on success, else a negative value.
 */
int lttng_set_consumer_url(struct lttng_handle *handle,
		const char *control_url, const char *data_url)
{
	int ret;
	ssize_t size;
	struct lttcomm_session_msg lsm;
	struct lttng_uri *uris = NULL;

	if (handle == NULL || (control_url == NULL && data_url == NULL)) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	memset(&lsm, 0, sizeof(lsm));

	lsm.cmd_type = LTTNG_SET_CONSUMER_URI;

	ret = lttng_strncpy(lsm.session.name, handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	COPY_DOMAIN_PACKED(lsm.domain, handle->domain);

	size = uri_parse_str_urls(control_url, data_url, &uris);
	if (size < 0) {
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	lsm.u.uri.size = size;

	ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(&lsm, uris,
			sizeof(struct lttng_uri) * size, NULL);

	free(uris);
error:
	return ret;
}

/*
 * [OBSOLETE]
 */
int lttng_enable_consumer(struct lttng_handle *handle);
int lttng_enable_consumer(struct lttng_handle *handle)
{
	return -ENOSYS;
}

/*
 * [OBSOLETE]
 */
int lttng_disable_consumer(struct lttng_handle *handle);
int lttng_disable_consumer(struct lttng_handle *handle)
{
	return -ENOSYS;
}

/*
 * [OBSOLETE]
 */
int _lttng_create_session_ext(const char *name, const char *url,
		const char *datetime);
int _lttng_create_session_ext(const char *name, const char *url,
		const char *datetime)
{
	return -ENOSYS;
}

/*
 * For a given session name, this call checks if the data is ready to be read
 * or is still being extracted by the consumer(s) hence not ready to be used by
 * any readers.
 */
int lttng_data_pending(const char *session_name)
{
	int ret;
	struct lttcomm_session_msg lsm;
	uint8_t *pending = NULL;

	if (session_name == NULL) {
		return -LTTNG_ERR_INVALID;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_DATA_PENDING;

	ret = lttng_strncpy(lsm.session.name, session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, (void **) &pending);
	if (ret < 0) {
		goto end;
	} else if (ret != 1) {
		/* Unexpected payload size */
		ret = -LTTNG_ERR_INVALID;
		goto end;
	} else if (!pending) {
		/* Internal error. */
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	ret = (int) *pending;
end:
	free(pending);
	return ret;
}

/*
 * Regenerate the metadata for a session.
 * Return 0 on success, a negative error code on error.
 */
int lttng_regenerate_metadata(const char *session_name)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (!session_name) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_REGENERATE_METADATA;

	ret = lttng_strncpy(lsm.session.name, session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
	if (ret < 0) {
		goto end;
	}

	ret = 0;
end:
	return ret;
}

/*
 * Deprecated, replaced by lttng_regenerate_metadata.
 */
int lttng_metadata_regenerate(const char *session_name)
{
	return lttng_regenerate_metadata(session_name);
}

/*
 * Regenerate the statedump of a session.
 * Return 0 on success, a negative error code on error.
 */
int lttng_regenerate_statedump(const char *session_name)
{
	int ret;
	struct lttcomm_session_msg lsm;

	if (!session_name) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_REGENERATE_STATEDUMP;

	ret = lttng_strncpy(lsm.session.name, session_name,
			sizeof(lsm.session.name));
	if (ret) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
	if (ret < 0) {
		goto end;
	}

	ret = 0;
end:
	return ret;
}

int lttng_register_trigger(struct lttng_trigger *trigger)
{
	int ret;
	struct lttcomm_session_msg lsm;
	struct lttng_dynamic_buffer buffer;

	lttng_dynamic_buffer_init(&buffer);
	if (!trigger) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (!lttng_trigger_validate(trigger)) {
		ret = -LTTNG_ERR_INVALID_TRIGGER;
		goto end;
	}

	ret = lttng_trigger_serialize(trigger, &buffer);
	if (ret < 0) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_REGISTER_TRIGGER;
	lsm.u.trigger.length = (uint32_t) buffer.size;
	ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(&lsm, buffer.data,
			buffer.size, NULL);
end:
	lttng_dynamic_buffer_reset(&buffer);
	return ret;
}

int lttng_unregister_trigger(struct lttng_trigger *trigger)
{
	int ret;
	struct lttcomm_session_msg lsm;
	struct lttng_dynamic_buffer buffer;

	lttng_dynamic_buffer_init(&buffer);
	if (!trigger) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	if (!lttng_trigger_validate(trigger)) {
		ret = -LTTNG_ERR_INVALID_TRIGGER;
		goto end;
	}

	ret = lttng_trigger_serialize(trigger, &buffer);
	if (ret < 0) {
		ret = -LTTNG_ERR_UNK;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_UNREGISTER_TRIGGER;
	lsm.u.trigger.length = (uint32_t) buffer.size;
	ret = lttng_ctl_ask_sessiond_varlen_no_cmd_header(&lsm, buffer.data,
			buffer.size, NULL);
end:
	lttng_dynamic_buffer_reset(&buffer);
	return ret;
}

/*
 * lib constructor.
 */
static void __attribute__((constructor)) init(void)
{
	/* Set default session group */
	lttng_set_tracing_group(DEFAULT_TRACING_GROUP);
}

/*
 * lib destructor.
 */
static void __attribute__((destructor)) lttng_ctl_exit(void)
{
	free(tracing_group);
}
