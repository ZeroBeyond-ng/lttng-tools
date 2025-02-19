/*
 * Copyright (C) 2014 David Goulet <dgoulet@efficios.com>
 * Copyright (C) 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_EVENT_H
#define LTTNG_EVENT_H

#include <lttng/handle.h>
#include <lttng/lttng-export.h>
#include <lttng/userspace-probe.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Instrumentation type of tracing event.
 */
enum lttng_event_type {
	LTTNG_EVENT_ALL                       = -1,
	LTTNG_EVENT_TRACEPOINT                = 0,
	LTTNG_EVENT_PROBE                     = 1,
	LTTNG_EVENT_FUNCTION                  = 2,
	LTTNG_EVENT_FUNCTION_ENTRY            = 3,
	LTTNG_EVENT_NOOP                      = 4,
	LTTNG_EVENT_SYSCALL                   = 5,
	LTTNG_EVENT_USERSPACE_PROBE           = 6,
};

/*
 * Loglevel information.
 */
enum lttng_loglevel_type {
	LTTNG_EVENT_LOGLEVEL_ALL              = 0,
	LTTNG_EVENT_LOGLEVEL_RANGE            = 1,
	LTTNG_EVENT_LOGLEVEL_SINGLE           = 2,
};

/*
 * Available loglevels.
 */
enum lttng_loglevel {
	LTTNG_LOGLEVEL_EMERG                  = 0,
	LTTNG_LOGLEVEL_ALERT                  = 1,
	LTTNG_LOGLEVEL_CRIT                   = 2,
	LTTNG_LOGLEVEL_ERR                    = 3,
	LTTNG_LOGLEVEL_WARNING                = 4,
	LTTNG_LOGLEVEL_NOTICE                 = 5,
	LTTNG_LOGLEVEL_INFO                   = 6,
	LTTNG_LOGLEVEL_DEBUG_SYSTEM           = 7,
	LTTNG_LOGLEVEL_DEBUG_PROGRAM          = 8,
	LTTNG_LOGLEVEL_DEBUG_PROCESS          = 9,
	LTTNG_LOGLEVEL_DEBUG_MODULE           = 10,
	LTTNG_LOGLEVEL_DEBUG_UNIT             = 11,
	LTTNG_LOGLEVEL_DEBUG_FUNCTION         = 12,
	LTTNG_LOGLEVEL_DEBUG_LINE             = 13,
	LTTNG_LOGLEVEL_DEBUG                  = 14,
};

/*
 * Available loglevels for the JUL domain. Those are an exact map from the
 * class java.util.logging.Level.
 */
enum lttng_loglevel_jul {
	LTTNG_LOGLEVEL_JUL_OFF                = INT32_MAX,
	LTTNG_LOGLEVEL_JUL_SEVERE             = 1000,
	LTTNG_LOGLEVEL_JUL_WARNING            = 900,
	LTTNG_LOGLEVEL_JUL_INFO               = 800,
	LTTNG_LOGLEVEL_JUL_CONFIG             = 700,
	LTTNG_LOGLEVEL_JUL_FINE               = 500,
	LTTNG_LOGLEVEL_JUL_FINER              = 400,
	LTTNG_LOGLEVEL_JUL_FINEST             = 300,
	LTTNG_LOGLEVEL_JUL_ALL                = INT32_MIN,
};

/*
 * Available loglevels for the LOG4j domain. Those are an exact map from the
 * class org.apache.log4j.Level.
 */
enum lttng_loglevel_log4j {
	LTTNG_LOGLEVEL_LOG4J_OFF              = INT32_MAX,
	LTTNG_LOGLEVEL_LOG4J_FATAL            = 50000,
	LTTNG_LOGLEVEL_LOG4J_ERROR            = 40000,
	LTTNG_LOGLEVEL_LOG4J_WARN             = 30000,
	LTTNG_LOGLEVEL_LOG4J_INFO             = 20000,
	LTTNG_LOGLEVEL_LOG4J_DEBUG            = 10000,
	LTTNG_LOGLEVEL_LOG4J_TRACE            = 5000,
	LTTNG_LOGLEVEL_LOG4J_ALL              = INT32_MIN,
};

/*
 * Available loglevels for the Python domain. Those are an exact map from the
 * Level class.
 */
enum lttng_loglevel_python {
	LTTNG_LOGLEVEL_PYTHON_CRITICAL        = 50,
	LTTNG_LOGLEVEL_PYTHON_ERROR           = 40,
	LTTNG_LOGLEVEL_PYTHON_WARNING         = 30,
	LTTNG_LOGLEVEL_PYTHON_INFO            = 20,
	LTTNG_LOGLEVEL_PYTHON_DEBUG           = 10,
	LTTNG_LOGLEVEL_PYTHON_NOTSET          = 0,
};

/*
 * LTTng consumer mode
 */
enum lttng_event_output {
	LTTNG_EVENT_SPLICE                    = 0,
	LTTNG_EVENT_MMAP                      = 1,
};

/* Event context possible type */
enum lttng_event_context_type {
	LTTNG_EVENT_CONTEXT_PID			= 0,
	LTTNG_EVENT_CONTEXT_PERF_COUNTER	= 1,	/* Backward compat. */
	LTTNG_EVENT_CONTEXT_PROCNAME		= 2,
	LTTNG_EVENT_CONTEXT_PRIO		= 3,
	LTTNG_EVENT_CONTEXT_NICE		= 4,
	LTTNG_EVENT_CONTEXT_VPID		= 5,
	LTTNG_EVENT_CONTEXT_TID			= 6,
	LTTNG_EVENT_CONTEXT_VTID		= 7,
	LTTNG_EVENT_CONTEXT_PPID		= 8,
	LTTNG_EVENT_CONTEXT_VPPID		= 9,
	LTTNG_EVENT_CONTEXT_PTHREAD_ID		= 10,
	LTTNG_EVENT_CONTEXT_HOSTNAME		= 11,
	LTTNG_EVENT_CONTEXT_IP			= 12,
	LTTNG_EVENT_CONTEXT_PERF_CPU_COUNTER	= 13,
	LTTNG_EVENT_CONTEXT_PERF_THREAD_COUNTER = 14,
	LTTNG_EVENT_CONTEXT_APP_CONTEXT		= 15,
	LTTNG_EVENT_CONTEXT_INTERRUPTIBLE	= 16,
	LTTNG_EVENT_CONTEXT_PREEMPTIBLE		= 17,
	LTTNG_EVENT_CONTEXT_NEED_RESCHEDULE	= 18,
	LTTNG_EVENT_CONTEXT_MIGRATABLE		= 19,
	LTTNG_EVENT_CONTEXT_CALLSTACK_KERNEL	= 20,
	LTTNG_EVENT_CONTEXT_CALLSTACK_USER	= 21, /* Supported on x86_32 and x86_64 only. */
	LTTNG_EVENT_CONTEXT_CGROUP_NS		= 22,
	LTTNG_EVENT_CONTEXT_IPC_NS		= 23,
	LTTNG_EVENT_CONTEXT_MNT_NS		= 24,
	LTTNG_EVENT_CONTEXT_NET_NS		= 25,
	LTTNG_EVENT_CONTEXT_PID_NS		= 26,
	LTTNG_EVENT_CONTEXT_USER_NS		= 27,
	LTTNG_EVENT_CONTEXT_UTS_NS		= 28,
	LTTNG_EVENT_CONTEXT_UID			= 29,
	LTTNG_EVENT_CONTEXT_EUID		= 30,
	LTTNG_EVENT_CONTEXT_SUID		= 31,
	LTTNG_EVENT_CONTEXT_GID			= 32,
	LTTNG_EVENT_CONTEXT_EGID		= 33,
	LTTNG_EVENT_CONTEXT_SGID		= 34,
	LTTNG_EVENT_CONTEXT_VUID		= 35,
	LTTNG_EVENT_CONTEXT_VEUID		= 36,
	LTTNG_EVENT_CONTEXT_VSUID		= 37,
	LTTNG_EVENT_CONTEXT_VGID		= 38,
	LTTNG_EVENT_CONTEXT_VEGID		= 39,
	LTTNG_EVENT_CONTEXT_VSGID		= 40,
	LTTNG_EVENT_CONTEXT_TIME_NS		= 41,
};

enum lttng_event_field_type {
	LTTNG_EVENT_FIELD_OTHER               = 0,
	LTTNG_EVENT_FIELD_INTEGER             = 1,
	LTTNG_EVENT_FIELD_ENUM                = 2,
	LTTNG_EVENT_FIELD_FLOAT               = 3,
	LTTNG_EVENT_FIELD_STRING              = 4,
};

enum lttng_event_flag {
	LTTNG_EVENT_FLAG_SYSCALL_32           = (1U << 0),
	LTTNG_EVENT_FLAG_SYSCALL_64           = (1U << 1),
};

/*
 * Perf counter attributes
 *
 * The structures should be initialized to zero before use.
 */
#define LTTNG_PERF_EVENT_PADDING1          16
struct lttng_event_perf_counter_ctx {
	uint32_t type;
	uint64_t config;
	char name[LTTNG_SYMBOL_NAME_LEN];

	char padding[LTTNG_PERF_EVENT_PADDING1];
};

/*
 * Event/channel context
 *
 * The structures should be initialized to zero before use.
 */
#define LTTNG_EVENT_CONTEXT_PADDING1       16
#define LTTNG_EVENT_CONTEXT_PADDING2       LTTNG_SYMBOL_NAME_LEN + 32
struct lttng_event_context {
	enum lttng_event_context_type ctx;
	char padding[LTTNG_EVENT_CONTEXT_PADDING1];

	union {
		struct lttng_event_perf_counter_ctx perf_counter;
		struct {
			char *provider_name;
			char *ctx_name;
		} app_ctx;
		char padding[LTTNG_EVENT_CONTEXT_PADDING2];
	} u;
};

/*
 * Event probe.
 *
 * Either addr is used or symbol_name and offset.
 *
 * The structures should be initialized to zero before use.
 */
#define LTTNG_EVENT_PROBE_PADDING1         16
struct lttng_event_probe_attr {
	uint64_t addr;

	uint64_t offset;
	char symbol_name[LTTNG_SYMBOL_NAME_LEN];

	char padding[LTTNG_EVENT_PROBE_PADDING1];
};

/*
 * Function tracer
 *
 * The structures should be initialized to zero before use.
 */
#define LTTNG_EVENT_FUNCTION_PADDING1      16
struct lttng_event_function_attr {
	char symbol_name[LTTNG_SYMBOL_NAME_LEN];

	char padding[LTTNG_EVENT_FUNCTION_PADDING1];
};

/*
 * Generic lttng event
 *
 * The structures should be initialized to zero before use.
 */
#define LTTNG_EVENT_PADDING1               12
#define LTTNG_EVENT_PADDING2               LTTNG_SYMBOL_NAME_LEN + 32
struct lttng_event {
	/* Offset 0 */
	enum lttng_event_type type;

	/* Offset 4 */
	char name[LTTNG_SYMBOL_NAME_LEN];

	/* Offset 260 */
	enum lttng_loglevel_type loglevel_type;

	/* Offset 264 */
	int loglevel;

	/* Offset 268 */
	int32_t enabled;	/* Does not apply: -1 */

	/* Offset 272 */
	pid_t pid;

	/* Offset 276 */
	unsigned char filter;	/* filter enabled ? */

	/* Offset 277 */
	unsigned char exclusion; /* exclusions added ? */

	/* Offset 278 */
	char padding2[2];

	/* Offset 280 */
	/* Event flag, from 2.6 and above. */
	enum lttng_event_flag flags;

	/* Offset 284 */
	char padding[4];

	/* Offset 288 */
	union {
		uint64_t padding;
		void *ptr;
	} extended;

	/* Offset 296 */
	/* Per event type configuration */
	union {
		struct lttng_event_probe_attr probe;
		struct lttng_event_function_attr ftrace;

		char padding[LTTNG_EVENT_PADDING2];
	} attr;
};

#define LTTNG_EVENT_FIELD_PADDING	LTTNG_SYMBOL_NAME_LEN + 32
struct lttng_event_field {
	char field_name[LTTNG_SYMBOL_NAME_LEN];
	enum lttng_event_field_type type;
	char padding[LTTNG_EVENT_FIELD_PADDING];
	struct lttng_event event;
	int nowrite;
};

/*
 * List the event(s) of a session channel.
 *
 * Both handle and channel_name CAN NOT be NULL.
 *
 * Return the size (number of entries) of the "lttng_event" array. Caller must
 * free events. On error a negative LTTng error code is returned.
 */
LTTNG_EXPORT extern int lttng_list_events(struct lttng_handle *handle,
		const char *channel_name, struct lttng_event **events);

/*
 * Create an lttng_event.
 *
 * This creation function, introduced in LTTng 2.11, works around
 * the fact that the layout of the 'lttng_event' is publicly exposed.
 *
 * It allocates a larger object which exposes the same public fields
 * as a 'struct lttng_event', but also allows the use of the following extended
 * attribute setters:
 *   - lttng_event_set_userspace_probe_location();
 *
 * Events created through this function must be destroyed using
 * lttng_event_destroy().
 *
 * Returns a zeroed lttng_event on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_event *lttng_event_create(void);

/*
 * Destroy an lttng_event.
 *
 * This destruction function, introduced in LTTng 2.11, should only
 * be used with events created by lttng_event_create().
  */
LTTNG_EXPORT extern void lttng_event_destroy(struct lttng_event *event);

/*
 * Get the filter expression of a specific LTTng event.
 *
 * If the call is successful, then the filter expression's address is put
 * in *filter_expression. If the event has no filter expression,
 * *filter_expression is set to NULL. The caller does NOT own
 * *filter_expression.
 *
 * Returns 0 on success, or a negative LTTng error code on error.
 */
LTTNG_EXPORT extern int lttng_event_get_filter_expression(struct lttng_event *event,
		const char **filter_string);

/*
 * Get the number of exclusion names of a specific LTTng event.
 *
 * Returns the number of exclusion names on success, or a negative
 * LTTng error code on error.
 */
LTTNG_EXPORT extern int lttng_event_get_exclusion_name_count(struct lttng_event *event);

/*
 * Get an LTTng event's exclusion name at a given index.
 *
 * If the call is successful, then the exclusion name string's address
 * is put in *exclusion_name. The caller does NOT own *exclusion_name.
 *
 * Returns 0 on success, or a negative LTTng error code on error.
 */
LTTNG_EXPORT extern int lttng_event_get_exclusion_name(struct lttng_event *event,
		size_t index, const char **exclusion_name);

/*
 * Get the userspace probe location of a specific LTTng event.
 * If the call is successful, then a pointer to the probe location is returned.
 * If the event has no probe location a NULL pointer is returned. The caller
 * does not own the returned probe location.
 */
LTTNG_EXPORT extern const struct lttng_userspace_probe_location *
lttng_event_get_userspace_probe_location(const struct lttng_event *event);

/*
 * Set an LTTng event's userspace probe location.
 *
 * If the call is successful, then the probe location is set to the event. The
 * ownership of the probe_location is given to the event.
 *
 * Note that the event must have been created using 'lttng_event_create()' in
 * order for this call to succeed.
 *
 * Returns 0 on success, or a negative LTTng error code on error.
 */
LTTNG_EXPORT extern int lttng_event_set_userspace_probe_location(struct lttng_event *event,
		struct lttng_userspace_probe_location *probe_location);

/*
 * List the available tracepoints of a specific lttng domain.
 *
 * The handle CAN NOT be NULL.
 *
 * Return the size (number of entries) of the "lttng_event" array.  Caller must
 * free events. On error a negative LTTng error code is returned.
 */
LTTNG_EXPORT extern int lttng_list_tracepoints(struct lttng_handle *handle,
		struct lttng_event **events);

/*
 * List the available tracepoints fields of a specific lttng domain.
 *
 * The handle CAN NOT be NULL.
 *
 * Return the size (number of entries) of the "lttng_event_field" array.
 * Caller must free fields. On error a negative LTTng error code is
 * returned.
 */
LTTNG_EXPORT extern int lttng_list_tracepoint_fields(struct lttng_handle *handle,
		struct lttng_event_field **fields);

/*
 * List the available kernel syscall.
 *
 * Return the size (number of entries) of the allocated "lttng_event" array.
 * All events in will be of type syscall. Caller must free events. On error a
 * negative LTTng error code is returned.
 */
LTTNG_EXPORT extern int lttng_list_syscalls(struct lttng_event **events);

/*
 * Add context to event(s) for a specific channel (or for all).
 *
 * If the channel_name is NULL and they are no channel for the domain, the
 * default channel is created (channel0). The context is then added on ALL
 * channels since no name was specified.
 *
 * The event_name is ignored since adding a context to an event is not possible
 * for now.
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_add_context(struct lttng_handle *handle,
		struct lttng_event_context *ctx, const char *event_name,
		const char *channel_name);

/*
 * Create or enable an event (or events) for a channel.
 *
 * If the event you are trying to enable does not exist, it will be created,
 * else it is enabled. If channel_name is NULL, the default channel is used
 * (channel0).
 *
 * The handle and ev params can not be NULL.
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_enable_event(struct lttng_handle *handle,
		struct lttng_event *ev, const char *channel_name);

/*
 * Create or enable an event with a specific filter.
 *
 * If the event you are trying to enable does not exist, it will be created,
 * else it is enabled.
 * If ev is NULL, all events are enabled with that filter.
 * If channel_name is NULL, the default channel is used (channel0) and created
 * if not found.
 * If filter_expression is NULL, an event without associated filter is
 * created.
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_enable_event_with_filter(struct lttng_handle *handle,
		struct lttng_event *event, const char *channel_name,
		const char *filter_expression);

/*
 * Create or enable an event with a filter and/or exclusions.
 *
 * If the event you are trying to enable does not exist, it will be created,
 * else it is enabled.
 * If ev is NULL, all events are enabled with the filter and exclusion options.
 * If channel_name is NULL, the default channel is used (channel0) and created
 * if not found.
 * If filter_expression is NULL, an event without associated filter is
 * created.
 * If exclusion count is zero, the event will be created without exclusions.
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_enable_event_with_exclusions(struct lttng_handle *handle,
		struct lttng_event *event, const char *channel_name,
		const char *filter_expression,
		int exclusion_count, char **exclusion_names);

/*
 * Disable event(s) of a channel and domain.
 *
 * If name is NULL, all events are disabled.
 * If channel_name is NULL, the default channel is used (channel0).
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_disable_event(struct lttng_handle *handle,
		const char *name, const char *channel_name);

/*
 * Disable event(s) of a channel and domain.
 *
 * Takes a struct lttng_event as parameter.
 * If channel_name is NULL, the default channel is used (channel0).
 *
 * Currently, @filter_expression must be NULL. (disabling specific
 * filter expressions not implemented)
 * Currently, only LTTNG_EVENT_ALL and LTTNG_EVENT_SYSCALL event types
 * are implemented for field @ev.
 *
 * Return 0 on success else a negative LTTng error code.
 */
LTTNG_EXPORT extern int lttng_disable_event_ext(struct lttng_handle *handle,
		struct lttng_event *ev, const char *channel_name,
		const char *filter_expression);

#ifdef __cplusplus
}
#endif

#endif /* LTTNG_EVENT_H */
