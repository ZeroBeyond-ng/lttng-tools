/*
 * Copyright (C) 2022 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "ust-field-convert.hpp"

#include <common/make-unique.hpp>

#include <unordered_map>

namespace lst = lttng::sessiond::trace;
namespace lsu = lttng::sessiond::ust;
namespace {

/*
 * Type enclosing the session information that may be required during the decoding
 * of the lttng_ust_ctl_field array provided by applications on registration of
 * an event.
 */
class session_attributes {
public:
	using registry_enum_getter_fn =
			std::function<lsu::registry_enum::const_rcu_protected_reference(
					const char *name, uint64_t id)>;

	session_attributes(registry_enum_getter_fn reg_enum_getter,
			lst::byte_order native_trace_byte_order) :
		get_registry_enum{reg_enum_getter}, _native_trace_byte_order{native_trace_byte_order}
	{
	}

	const registry_enum_getter_fn get_registry_enum;
	const lst::byte_order _native_trace_byte_order;
};

/* Used to publish fields on which a field being decoded has an implicit dependency. */
using publish_field_fn = std::function<void(lst::field::cuptr)>;

lst::type::cuptr create_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field);

void create_field_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field);

template <class UstCtlEncodingType>
enum lst::null_terminated_string_type::encoding ust_ctl_encoding_to_string_field_encoding(UstCtlEncodingType encoding)
{
	static const std::unordered_map<UstCtlEncodingType, enum lst::null_terminated_string_type::encoding>
			encoding_conversion_map = {
					{(UstCtlEncodingType) lttng_ust_ctl_encode_ASCII,
							lst::null_terminated_string_type::encoding::ASCII},
					{(UstCtlEncodingType) lttng_ust_ctl_encode_UTF8,
							lst::null_terminated_string_type::encoding::UTF8},
			};

	const auto encoding_it = encoding_conversion_map.find(encoding);
	if (encoding_it == encoding_conversion_map.end()) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unknown lttng_ust_ctl_string_encodings value `{}` encountered when decoding integer field",
				encoding));
	}

	return encoding_it->second;
}

template <class UstCtlBaseType>
enum lst::integer_type::base ust_ctl_base_to_integer_field_base(UstCtlBaseType base)
{
	static const std::unordered_map<UstCtlBaseType, enum lst::integer_type::base>
			base_conversion_map = {{2, lst::integer_type::base::BINARY},
					{8, lst::integer_type::base::OCTAL},
					{10, lst::integer_type::base::DECIMAL},
					{16, lst::integer_type::base::HEXADECIMAL}};

	const auto base_it = base_conversion_map.find(base);
	if (base_it == base_conversion_map.end()) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unknown integer base value `{}` encountered when decoding integer field",
				base));
	}

	return base_it->second;
}

lst::type::cuptr create_integer_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto base = ust_ctl_base_to_integer_field_base(current->type.u.integer.base);
	const auto signedness = current->type.u.integer.signedness ?
			lst::integer_type::signedness::SIGNED :
			      lst::integer_type::signedness::UNSIGNED;
	const auto byte_order = current->type.u.integer.reverse_byte_order ?
			lst::type::reverse_byte_order(session_attributes._native_trace_byte_order) :
			      session_attributes._native_trace_byte_order;

	*next_ust_ctl_field = current + 1;

	return lttng::make_unique<const lst::integer_type>(current->type.u.integer.alignment,
			byte_order, current->type.u.integer.size, signedness, base);
}

lst::type::cuptr create_floating_point_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	*next_ust_ctl_field = current + 1;

	const auto byte_order = current->type.u._float.reverse_byte_order ?
			lst::type::reverse_byte_order(session_attributes._native_trace_byte_order) :
			      session_attributes._native_trace_byte_order;

	try {
		return lttng::make_unique<const lst::floating_point_type>(
				current->type.u._float.alignment, byte_order,
				current->type.u._float.exp_dig, current->type.u._float.mant_dig);
	} catch (lttng::invalid_argument_error& ex) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format("Invalid floating point attribute in {}: {}",
				typeid(*current), ex.what()));
	}
}

lst::type::cuptr create_enumeration_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	uint64_t enumeration_id;
	const auto& enum_uctl_field = *current;
	const char *enumeration_name;
	const auto *enum_container_uctl_type =
			&current->type.u.legacy.basic.enumeration.container_type;

	if (enum_uctl_field.type.atype == lttng_ust_ctl_atype_enum_nestable) {
		/* Nestable enumeration fields are followed by their container type. */
		++current;
		if (current >= end) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Array of {} is too short to contain nestable enumeration's container",
					typeid(*current)));
		}

		if (current->type.atype != lttng_ust_ctl_atype_integer) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Invalid type of nestable enum container: type id = {}",
					current->type.atype));
		}

		enum_container_uctl_type = &current->type.u.integer;
		enumeration_id = enum_uctl_field.type.u.enum_nestable.id;
		enumeration_name = enum_uctl_field.type.u.enum_nestable.name;
	} else {
		enumeration_id = enum_uctl_field.type.u.legacy.basic.enumeration.id;
		enumeration_name = enum_uctl_field.type.u.legacy.basic.enumeration.name;
	}

	*next_ust_ctl_field = current + 1;

	const auto base = ust_ctl_base_to_integer_field_base(enum_container_uctl_type->base);
	const auto byte_order = enum_container_uctl_type->reverse_byte_order ?
			lst::integer_type::reverse_byte_order(
					session_attributes._native_trace_byte_order) :
			      session_attributes._native_trace_byte_order;
	const auto signedness = enum_container_uctl_type->signedness ?
			lst::integer_type::signedness::SIGNED :
			      lst::integer_type::signedness::UNSIGNED;

	if (enum_container_uctl_type->signedness) {
		const auto& enum_registry = static_cast<const lsu::registry_signed_enum&>(
				*session_attributes.get_registry_enum(
						enumeration_name, enumeration_id));

		return lttng::make_unique<const lst::signed_enumeration_type>(
				enum_container_uctl_type->alignment, byte_order,
				enum_container_uctl_type->size, signedness, base,
				enum_registry._mappings);
	} else {
		const auto& enum_registry = static_cast<const lsu::registry_unsigned_enum&>(
				*session_attributes.get_registry_enum(
						enumeration_name, enumeration_id));

		return lttng::make_unique<const lst::unsigned_enumeration_type>(
				enum_container_uctl_type->alignment, byte_order,
				enum_container_uctl_type->size, signedness, base,
				enum_registry._mappings);
	}
}

lst::type::cuptr create_string_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes __attribute__((unused)),
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& string_uctl_field = *current;
	*next_ust_ctl_field = current + 1;

	const auto encoding = ust_ctl_encoding_to_string_field_encoding(
			string_uctl_field.type.u.string.encoding);

	return lttng::make_unique<const lst::null_terminated_string_type>(1, encoding);
}

lst::type::cuptr create_integer_type_from_ust_ctl_basic_type(
		const lttng_ust_ctl_basic_type& type, const session_attributes& session_attributes)
{
	/* Checked by caller. */
	LTTNG_ASSERT(type.atype == lttng_ust_ctl_atype_integer);

	const auto byte_order = type.u.basic.integer.reverse_byte_order ?
			lst::integer_type::reverse_byte_order(
					session_attributes._native_trace_byte_order) :
			      session_attributes._native_trace_byte_order;
	const auto signedness = type.u.basic.integer.signedness ?
			lst::integer_type::signedness::SIGNED :
			      lst::integer_type::signedness::UNSIGNED;
	const auto base = ust_ctl_base_to_integer_field_base(type.u.basic.integer.base);
	const auto size = type.u.basic.integer.size;
	const auto alignment = type.u.basic.integer.alignment;

	return lttng::make_unique<const lst::integer_type>(
			alignment, byte_order, size, signedness, base);
}

lst::type::cuptr create_array_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& array_uctl_field = *current;
	uint32_t array_alignment, array_length;
	lst::type::cuptr element_type;
	nonstd::optional<enum lst::string_type::encoding> element_encoding;

	array_length = array_uctl_field.type.u.legacy.array.length;
	array_alignment = 0;

	const auto& element_uctl_type = array_uctl_field.type.u.legacy.array.elem_type;
	if (element_uctl_type.atype != lttng_ust_ctl_atype_integer) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unexpected legacy array element type: atype = {}, expected atype = lttng_ust_ctl_atype_integer ({})",
				element_uctl_type.atype, lttng_ust_ctl_atype_integer));
	}

	element_type = create_integer_type_from_ust_ctl_basic_type(
			element_uctl_type, session_attributes);
	if (element_uctl_type.atype == lttng_ust_ctl_atype_integer &&
			element_uctl_type.u.basic.integer.encoding != lttng_ust_ctl_encode_none) {
		/* Element represents a text character. */
		element_encoding = ust_ctl_encoding_to_string_field_encoding(
				element_uctl_type.u.basic.integer.encoding);
	}

	*next_ust_ctl_field = current + 1;

	if (element_encoding) {
		const auto integer_element_size =
				static_cast<const lst::integer_type&>(*element_type).size;

		if (integer_element_size != 8) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Unexpected legacy array element type: integer has encoding but size is not 8: size = {}",
					integer_element_size));
		}

		/* Array is a static-length string. */
		return lttng::make_unique<lst::static_length_string_type>(
				array_alignment, *element_encoding, array_length);
	}

	return lttng::make_unique<lst::static_length_array_type>(
			array_alignment, std::move(element_type), array_length);
}

lst::type::cuptr create_array_nestable_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& array_uctl_field = *current;
	uint32_t array_alignment, array_length;
	lst::type::cuptr element_type;
	nonstd::optional<enum lst::string_type::encoding> element_encoding;

	array_length = array_uctl_field.type.u.array_nestable.length;
	array_alignment = array_uctl_field.type.u.array_nestable.alignment;

	/* Nestable array fields are followed by their element type. */
	const auto& element_uctl_field = *(current + 1);

	/* next_ust_ctl_field is updated as needed. */
	element_type = create_type_from_ust_ctl_fields(&element_uctl_field, end, session_attributes,
			next_ust_ctl_field, publish_field);
	if (element_uctl_field.type.atype == lttng_ust_ctl_atype_integer &&
			element_uctl_field.type.u.integer.encoding != lttng_ust_ctl_encode_none) {
		/* Element represents a text character. */
		element_encoding = ust_ctl_encoding_to_string_field_encoding(
				element_uctl_field.type.u.integer.encoding);
	}

	if (element_encoding) {
		const auto integer_element_size =
				static_cast<const lst::integer_type&>(*element_type).size;

		if (integer_element_size != 8) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Unexpected array element type: integer has encoding but size is not 8: size = {}",
					integer_element_size));
		}

		/* Array is a static-length string. */
		return lttng::make_unique<lst::static_length_string_type>(
				array_alignment, *element_encoding, array_length);
	}

	return lttng::make_unique<lst::static_length_array_type>(
			array_alignment, std::move(element_type), array_length);
}

/*
 * For legacy sequence types, LTTng-UST expresses both the sequence and sequence
 * length as part of the same lttng_ust_ctl_field entry.
 */
lst::type::cuptr create_sequence_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& sequence_uctl_field = *current;
	const auto& element_uctl_type = sequence_uctl_field.type.u.legacy.sequence.elem_type;
	const auto& length_uctl_type = sequence_uctl_field.type.u.legacy.sequence.length_type;
	const auto sequence_alignment = 0U;

	if (element_uctl_type.atype != lttng_ust_ctl_atype_integer) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unexpected legacy sequence element type: atype = {}, expected atype = lttng_ust_ctl_atype_integer ({})",
				element_uctl_type.atype, lttng_ust_ctl_atype_integer));
	}

	if (length_uctl_type.atype != lttng_ust_ctl_atype_integer) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unexpected legacy sequence length field type: atype = {}, expected atype = lttng_ust_ctl_atype_integer ({})",
				length_uctl_type.atype, lttng_ust_ctl_atype_integer));
	}

	nonstd::optional<enum lst::string_type::encoding> element_encoding;
	if (element_uctl_type.atype == lttng_ust_ctl_atype_integer &&
			element_uctl_type.u.basic.integer.encoding != lttng_ust_ctl_encode_none) {
		/* Element represents a text character. */
		element_encoding = ust_ctl_encoding_to_string_field_encoding(
				element_uctl_type.u.basic.integer.encoding);
	}

	const auto length_field_name = fmt::format("_{}_length", sequence_uctl_field.name);
	auto element_type = create_integer_type_from_ust_ctl_basic_type(
			element_uctl_type, session_attributes);
	auto length_type = create_integer_type_from_ust_ctl_basic_type(
			length_uctl_type, session_attributes);

	/* Publish an implicit length field _before_ the sequence field. */
	publish_field(lttng::make_unique<lst::field>(length_field_name, std::move(length_type)));

	*next_ust_ctl_field = current + 1;

	if (element_encoding) {
		const auto integer_element_size =
				static_cast<const lst::integer_type&>(*element_type).size;

		if (integer_element_size != 8) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Unexpected legacy array element type: integer has encoding but size is not 8: size = {}",
					integer_element_size));
		}

		/* Sequence is a dynamic-length string. */
		return lttng::make_unique<lst::dynamic_length_string_type>(sequence_alignment,
				*element_encoding, std::move(length_field_name));
	}

	return lttng::make_unique<lst::dynamic_length_array_type>(
			sequence_alignment, std::move(element_type), std::move(length_field_name));
}

lst::type::cuptr create_sequence_nestable_type_from_ust_ctl_fields(
		const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& sequence_uctl_field = *current;
	const auto sequence_alignment = sequence_uctl_field.type.u.sequence_nestable.alignment;
	const auto *length_field_name = sequence_uctl_field.type.u.sequence_nestable.length_name;

	/* Nestable sequence fields are followed by their element type. */
	const auto& element_uctl_field = *(current + 1);

	nonstd::optional<enum lst::string_type::encoding> element_encoding;
	if (element_uctl_field.type.atype == lttng_ust_ctl_atype_integer &&
			element_uctl_field.type.u.integer.encoding != lttng_ust_ctl_encode_none) {
		/* Element represents a text character. */
		element_encoding = ust_ctl_encoding_to_string_field_encoding(
				element_uctl_field.type.u.integer.encoding);
	}

	/* next_ust_ctl_field is updated as needed. */
	auto element_type = create_type_from_ust_ctl_fields(&element_uctl_field, end,
			session_attributes, next_ust_ctl_field, publish_field);

	if (lttng_strnlen(sequence_uctl_field.type.u.sequence_nestable.length_name,
			    sizeof(sequence_uctl_field.type.u.sequence_nestable.length_name)) ==
			sizeof(sequence_uctl_field.type.u.sequence_nestable.length_name)) {
		LTTNG_THROW_PROTOCOL_ERROR("Sequence length field name is not null terminated");
	}

	if (element_encoding) {
		const auto integer_element_size =
				static_cast<const lst::integer_type&>(*element_type).size;

		if (integer_element_size != 8) {
			LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
					"Unexpected array element type: integer has encoding but size is not 8: size = {}",
					integer_element_size));
		}

		/* Sqeuence is a dynamic-length string. */
		return lttng::make_unique<lst::dynamic_length_string_type>(sequence_alignment,
				*element_encoding, std::move(length_field_name));
	}

	return lttng::make_unique<lst::dynamic_length_array_type>(
			sequence_alignment, std::move(element_type), std::move(length_field_name));
}

lst::type::cuptr create_structure_field_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes __attribute__((unused)),
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	uint32_t field_count;
	uint32_t alignment;
	const auto& structure_uctl_field = *current;

	if (structure_uctl_field.type.atype == lttng_ust_ctl_atype_struct) {
		field_count = structure_uctl_field.type.u.legacy._struct.nr_fields;
		alignment = 0;
	} else {
		field_count = structure_uctl_field.type.u.struct_nestable.nr_fields;
		alignment = structure_uctl_field.type.u.struct_nestable.alignment;
	}

	if (field_count != 0) {
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Only empty structures are supported by LTTng-UST: nr_fields = {}",
				field_count));
	}

	*next_ust_ctl_field = current + 1;
	return lttng::make_unique<lst::structure_type>(alignment, lst::structure_type::fields());
}

lst::type::cuptr create_variant_field_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field)
{
	if (current >= end) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("End of {} array reached unexpectedly during decoding",
						typeid(*current)));
	}

	const auto& variant_uctl_field = *current;
	current++;

	uint32_t alignment;
	uint32_t choice_count;
	const char *tag_name;

	if (variant_uctl_field.type.atype == lttng_ust_ctl_atype_variant) {
		alignment = 0;
		choice_count = variant_uctl_field.type.u.legacy.variant.nr_choices;
		tag_name = variant_uctl_field.type.u.legacy.variant.tag_name;
	} else {
		alignment = variant_uctl_field.type.u.variant_nestable.alignment;
		choice_count = variant_uctl_field.type.u.variant_nestable.nr_choices;
		tag_name = variant_uctl_field.type.u.variant_nestable.tag_name;
	}

	/* Choices follow. next_ust_ctl_field is updated as needed. */
	lst::variant_type::choices choices;
	for (unsigned int i = 0; i < choice_count; i++) {
		create_field_from_ust_ctl_fields(current, end, session_attributes,
				next_ust_ctl_field, [&choices](lst::field::cuptr field) {
					choices.emplace_back(std::move(field));
				});

		current = *next_ust_ctl_field;
	}

	return lttng::make_unique<lst::variant_type>(alignment, tag_name, std::move(choices));
}

lst::type::cuptr create_type_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field)
{
	switch (current->type.atype) {
	case lttng_ust_ctl_atype_integer:
		return create_integer_type_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	case lttng_ust_ctl_atype_enum:
	case lttng_ust_ctl_atype_enum_nestable:
		return create_enumeration_type_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	case lttng_ust_ctl_atype_float:
		return create_floating_point_type_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	case lttng_ust_ctl_atype_string:
		return create_string_type_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	case lttng_ust_ctl_atype_array:
		return create_array_type_from_ust_ctl_fields(current, end, session_attributes,
				next_ust_ctl_field);
	case lttng_ust_ctl_atype_array_nestable:
		return create_array_nestable_type_from_ust_ctl_fields(current, end,
				session_attributes, next_ust_ctl_field, publish_field);
	case lttng_ust_ctl_atype_sequence:
		return create_sequence_type_from_ust_ctl_fields(current, end, session_attributes,
				next_ust_ctl_field, publish_field);
	case lttng_ust_ctl_atype_sequence_nestable:
		return create_sequence_nestable_type_from_ust_ctl_fields(current, end,
				session_attributes, next_ust_ctl_field, publish_field);
	case lttng_ust_ctl_atype_struct:
	case lttng_ust_ctl_atype_struct_nestable:
		return create_structure_field_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	case lttng_ust_ctl_atype_variant:
	case lttng_ust_ctl_atype_variant_nestable:
		return create_variant_field_from_ust_ctl_fields(
				current, end, session_attributes, next_ust_ctl_field);
	default:
		LTTNG_THROW_PROTOCOL_ERROR(fmt::format(
				"Unknown {} value `{}` encountered while converting {} to {}",
				typeid(current->type.atype), current->type.atype, typeid(*current),
				typeid(lst::type::cuptr::element_type)));
	}
}

void create_field_from_ust_ctl_fields(const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end,
		const session_attributes& session_attributes,
		const lttng_ust_ctl_field **next_ust_ctl_field,
		publish_field_fn publish_field)
{
	LTTNG_ASSERT(current < end);

	if (lttng_strnlen(current->name, sizeof(current->name)) == sizeof(current->name)) {
		LTTNG_THROW_PROTOCOL_ERROR(
				fmt::format("Name of {} is not null-terminated", typeid(*current)));
	}

	publish_field(lttng::make_unique<lst::field>(current->name,
			create_type_from_ust_ctl_fields(current, end, session_attributes,
					next_ust_ctl_field, publish_field)));
}

/*
 * `lttng_ust_ctl_field`s can be nested, in which case creating a field will consume
 * more than one lttng_ust_ctl_field. create_field_from_ust_ctl_fields returns the
 * position of the next lttng_ust_ctl_field to consume or `end` when the last field
 * is consumed.
 *
 * Always returns a new field, throws on error.
 */
std::vector<lst::field::cuptr> create_fields_from_ust_ctl_fields(
		const lsu::registry_session& session,
		const lttng_ust_ctl_field *current,
		const lttng_ust_ctl_field *end)
{
	std::vector<lst::field::cuptr> fields;
	const auto trace_native_byte_order = session.abi.byte_order;
	const session_attributes session_attributes{
			[&session](const char *enum_name, uint64_t enum_id) {
				return session.get_enumeration(enum_name, enum_id);
			},
			trace_native_byte_order};

	while (current < end) {
		auto *next_field = current;

		/*
		 * create_field_from_ust_ctl_fields will consume one field at a time.
		 * However, some fields expressed by LTTng-UST's protocol are expended
		 * to multiple event fields (legacy sequence fields implicitly define
		 * their length field).
		 *
		 * The lambda allows the factory functions to push as many fields as
		 * needed depending on the decoded field's type.
		 */
		create_field_from_ust_ctl_fields(current, end, session_attributes, &next_field,
				[&fields](lst::field::cuptr field) {
					fields.emplace_back(std::move(field));
				});

		current = next_field;
	}

	return fields;
}
} /* namespace */

std::vector<lst::field::cuptr> lsu::create_trace_fields_from_ust_ctl_fields(
		const lsu::registry_session& session,
		const lttng_ust_ctl_field *fields,
		std::size_t field_count)
{
	return create_fields_from_ust_ctl_fields(session, fields, fields + field_count);
}
