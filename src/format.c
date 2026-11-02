#include "format.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void append_owned(strbuf *sb, char *s) {
	if (s) {
		sb_append(sb, s);
		duckdb_free(s);
	}
}

char *format_type_name(duckdb_logical_type type) {
	strbuf sb;
	sb_init(&sb);
	char *alias = duckdb_logical_type_get_alias(type);
	if (alias) {
		append_owned(&sb, alias);
		return sb_detach(&sb);
	}
	duckdb_type id = duckdb_get_type_id(type);
	switch (id) {
	case DUCKDB_TYPE_BOOLEAN: sb_append(&sb, "boolean"); break;
	case DUCKDB_TYPE_TINYINT: sb_append(&sb, "tinyint"); break;
	case DUCKDB_TYPE_SMALLINT: sb_append(&sb, "smallint"); break;
	case DUCKDB_TYPE_INTEGER: sb_append(&sb, "integer"); break;
	case DUCKDB_TYPE_BIGINT: sb_append(&sb, "bigint"); break;
	case DUCKDB_TYPE_UTINYINT: sb_append(&sb, "utinyint"); break;
	case DUCKDB_TYPE_USMALLINT: sb_append(&sb, "usmallint"); break;
	case DUCKDB_TYPE_UINTEGER: sb_append(&sb, "uinteger"); break;
	case DUCKDB_TYPE_UBIGINT: sb_append(&sb, "ubigint"); break;
	case DUCKDB_TYPE_HUGEINT: sb_append(&sb, "hugeint"); break;
	case DUCKDB_TYPE_UHUGEINT: sb_append(&sb, "uhugeint"); break;
	case DUCKDB_TYPE_FLOAT: sb_append(&sb, "float"); break;
	case DUCKDB_TYPE_DOUBLE: sb_append(&sb, "double"); break;
	case DUCKDB_TYPE_DECIMAL:
		sb_appendf(&sb, "decimal(%u,%u)", (unsigned)duckdb_decimal_width(type), (unsigned)duckdb_decimal_scale(type));
		break;
	case DUCKDB_TYPE_BIGNUM: sb_append(&sb, "bignum"); break;
	case DUCKDB_TYPE_VARCHAR: sb_append(&sb, "varchar"); break;
	case DUCKDB_TYPE_BLOB: sb_append(&sb, "blob"); break;
	case DUCKDB_TYPE_BIT: sb_append(&sb, "bit"); break;
	case DUCKDB_TYPE_UUID: sb_append(&sb, "uuid"); break;
	case DUCKDB_TYPE_DATE: sb_append(&sb, "date"); break;
	case DUCKDB_TYPE_TIME: sb_append(&sb, "time"); break;
	case DUCKDB_TYPE_TIME_NS: sb_append(&sb, "time_ns"); break;
	case DUCKDB_TYPE_TIME_TZ: sb_append(&sb, "time with time zone"); break;
	case DUCKDB_TYPE_TIMESTAMP: sb_append(&sb, "timestamp"); break;
	case DUCKDB_TYPE_TIMESTAMP_S: sb_append(&sb, "timestamp_s"); break;
	case DUCKDB_TYPE_TIMESTAMP_MS: sb_append(&sb, "timestamp_ms"); break;
	case DUCKDB_TYPE_TIMESTAMP_NS: sb_append(&sb, "timestamp_ns"); break;
	case DUCKDB_TYPE_TIMESTAMP_TZ: sb_append(&sb, "timestamp with time zone"); break;
	case DUCKDB_TYPE_TIMESTAMP_TZ_NS: sb_append(&sb, "timestamp_ns with time zone"); break;
	case DUCKDB_TYPE_INTERVAL: sb_append(&sb, "interval"); break;
	case DUCKDB_TYPE_ENUM: sb_append(&sb, "enum"); break;
	case DUCKDB_TYPE_GEOMETRY: sb_append(&sb, "geometry"); break;
	case DUCKDB_TYPE_VARIANT: sb_append(&sb, "variant"); break;
	case DUCKDB_TYPE_SQLNULL: sb_append(&sb, "null"); break;
	case DUCKDB_TYPE_LIST:
	case DUCKDB_TYPE_ARRAY: {
		duckdb_logical_type child =
		    id == DUCKDB_TYPE_LIST ? duckdb_list_type_child_type(type) : duckdb_array_type_child_type(type);
		char *name = format_type_name(child);
		duckdb_destroy_logical_type(&child);
		if (id == DUCKDB_TYPE_LIST) {
			sb_appendf(&sb, "%s[]", name);
		} else {
			sb_appendf(&sb, "%s[%" PRIu64 "]", name, (uint64_t)duckdb_array_type_array_size(type));
		}
		free(name);
		break;
	}
	case DUCKDB_TYPE_MAP: {
		duckdb_logical_type k = duckdb_map_type_key_type(type);
		duckdb_logical_type v = duckdb_map_type_value_type(type);
		char *kn = format_type_name(k);
		char *vn = format_type_name(v);
		sb_appendf(&sb, "map(%s, %s)", kn, vn);
		free(kn);
		free(vn);
		duckdb_destroy_logical_type(&k);
		duckdb_destroy_logical_type(&v);
		break;
	}
	case DUCKDB_TYPE_STRUCT:
	case DUCKDB_TYPE_UNION: {
		bool is_struct = id == DUCKDB_TYPE_STRUCT;
		idx_t n = is_struct ? duckdb_struct_type_child_count(type) : duckdb_union_type_member_count(type);
		sb_append(&sb, is_struct ? "struct(" : "union(");
		for (idx_t i = 0; i < n; i++) {
			char *member = is_struct ? duckdb_struct_type_child_name(type, i) : duckdb_union_type_member_name(type, i);
			duckdb_logical_type t =
			    is_struct ? duckdb_struct_type_child_type(type, i) : duckdb_union_type_member_type(type, i);
			char *tn = format_type_name(t);
			sb_appendf(&sb, "%s%s %s", i ? ", " : "", member ? member : "", tn);
			free(tn);
			duckdb_free(member);
			duckdb_destroy_logical_type(&t);
		}
		sb_append_char(&sb, ')');
		break;
	}
	default: sb_appendf(&sb, "type%d", (int)id); break;
	}
	return sb_detach(&sb);
}

bool format_type_is_numeric(duckdb_type id) {
	switch (id) {
	case DUCKDB_TYPE_TINYINT:
	case DUCKDB_TYPE_SMALLINT:
	case DUCKDB_TYPE_INTEGER:
	case DUCKDB_TYPE_BIGINT:
	case DUCKDB_TYPE_UTINYINT:
	case DUCKDB_TYPE_USMALLINT:
	case DUCKDB_TYPE_UINTEGER:
	case DUCKDB_TYPE_UBIGINT:
	case DUCKDB_TYPE_HUGEINT:
	case DUCKDB_TYPE_UHUGEINT:
	case DUCKDB_TYPE_FLOAT:
	case DUCKDB_TYPE_DOUBLE:
	case DUCKDB_TYPE_DECIMAL:
	case DUCKDB_TYPE_BIGNUM:
		return true;
	default:
		return false;
	}
}

static bool row_is_valid(duckdb_vector vec, idx_t row) {
	uint64_t *validity = duckdb_vector_get_validity(vec);
	return !validity || duckdb_validity_row_is_valid(validity, row);
}

static duckdb_hugeint hugeint_from_int64(int64_t v) {
	duckdb_hugeint h;
	h.lower = (uint64_t)v;
	h.upper = v < 0 ? -1 : 0;
	return h;
}

/*
 * BIGNUM is stored as a 3-byte header (sign bit + byte count) followed by the big-endian magnitude.
 * Negative numbers have every byte, header included, bit-inverted.
 */
static duckdb_value bignum_value(const uint8_t *data, idx_t size) {
	if (size < 3) {
		return duckdb_create_null_value();
	}
	bool negative = (data[0] & 0x80) == 0;
	idx_t n = size - 3;
	uint8_t *mag = duckdb_malloc(n ? n : 1);
	for (idx_t i = 0; i < n; i++) {
		mag[i] = negative ? (uint8_t)~data[3 + i] : data[3 + i];
	}
	duckdb_bignum b;
	b.data = mag;
	b.size = n;
	b.is_negative = negative;
	duckdb_value v = duckdb_create_bignum(b);
	duckdb_free(mag);
	return v;
}

/* Builds a duckdb_value for one vector element so DuckDB itself renders it. */
static duckdb_value make_value(duckdb_vector vec, duckdb_logical_type type, idx_t row) {
	if (!row_is_valid(vec, row)) {
		return duckdb_create_null_value();
	}
	void *data = duckdb_vector_get_data(vec);
	duckdb_type id = duckdb_get_type_id(type);
	switch (id) {
	case DUCKDB_TYPE_BOOLEAN: return duckdb_create_bool(((bool *)data)[row]);
	case DUCKDB_TYPE_TINYINT: return duckdb_create_int8(((int8_t *)data)[row]);
	case DUCKDB_TYPE_SMALLINT: return duckdb_create_int16(((int16_t *)data)[row]);
	case DUCKDB_TYPE_INTEGER: return duckdb_create_int32(((int32_t *)data)[row]);
	case DUCKDB_TYPE_BIGINT: return duckdb_create_int64(((int64_t *)data)[row]);
	case DUCKDB_TYPE_UTINYINT: return duckdb_create_uint8(((uint8_t *)data)[row]);
	case DUCKDB_TYPE_USMALLINT: return duckdb_create_uint16(((uint16_t *)data)[row]);
	case DUCKDB_TYPE_UINTEGER: return duckdb_create_uint32(((uint32_t *)data)[row]);
	case DUCKDB_TYPE_UBIGINT: return duckdb_create_uint64(((uint64_t *)data)[row]);
	case DUCKDB_TYPE_HUGEINT: return duckdb_create_hugeint(((duckdb_hugeint *)data)[row]);
	case DUCKDB_TYPE_UHUGEINT: return duckdb_create_uhugeint(((duckdb_uhugeint *)data)[row]);
	case DUCKDB_TYPE_FLOAT: return duckdb_create_float(((float *)data)[row]);
	case DUCKDB_TYPE_DOUBLE: return duckdb_create_double(((double *)data)[row]);
	case DUCKDB_TYPE_DATE: return duckdb_create_date(((duckdb_date *)data)[row]);
	case DUCKDB_TYPE_TIME: return duckdb_create_time(((duckdb_time *)data)[row]);
	case DUCKDB_TYPE_TIME_NS: return duckdb_create_time_ns(((duckdb_time_ns *)data)[row]);
	case DUCKDB_TYPE_TIME_TZ: return duckdb_create_time_tz_value(((duckdb_time_tz *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP: return duckdb_create_timestamp(((duckdb_timestamp *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP_S: return duckdb_create_timestamp_s(((duckdb_timestamp_s *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP_MS: return duckdb_create_timestamp_ms(((duckdb_timestamp_ms *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP_NS: return duckdb_create_timestamp_ns(((duckdb_timestamp_ns *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP_TZ: return duckdb_create_timestamp_tz(((duckdb_timestamp *)data)[row]);
	case DUCKDB_TYPE_TIMESTAMP_TZ_NS: return duckdb_create_timestamp_tz_ns(((duckdb_timestamp_ns *)data)[row]);
	case DUCKDB_TYPE_INTERVAL: return duckdb_create_interval(((duckdb_interval *)data)[row]);
	case DUCKDB_TYPE_UUID: {
		/* Stored as a hugeint with the top bit flipped so that signed order matches UUID order. */
		duckdb_hugeint h = ((duckdb_hugeint *)data)[row];
		duckdb_uhugeint u;
		u.lower = h.lower;
		u.upper = (uint64_t)h.upper ^ ((uint64_t)1 << 63);
		return duckdb_create_uuid(u);
	}
	case DUCKDB_TYPE_DECIMAL: {
		duckdb_decimal d;
		d.width = duckdb_decimal_width(type);
		d.scale = duckdb_decimal_scale(type);
		switch (duckdb_decimal_internal_type(type)) {
		case DUCKDB_TYPE_SMALLINT: d.value = hugeint_from_int64(((int16_t *)data)[row]); break;
		case DUCKDB_TYPE_INTEGER: d.value = hugeint_from_int64(((int32_t *)data)[row]); break;
		case DUCKDB_TYPE_BIGINT: d.value = hugeint_from_int64(((int64_t *)data)[row]); break;
		default: d.value = ((duckdb_hugeint *)data)[row]; break;
		}
		return duckdb_create_decimal(d);
	}
	case DUCKDB_TYPE_VARCHAR:
	case DUCKDB_TYPE_BLOB:
	case DUCKDB_TYPE_BIT:
	case DUCKDB_TYPE_BIGNUM: {
		duckdb_string_t *s = &((duckdb_string_t *)data)[row];
		const char *p = duckdb_string_t_data(s);
		uint32_t len = duckdb_string_t_length(*s);
		if (id == DUCKDB_TYPE_VARCHAR) {
			return duckdb_create_varchar_length(p, len);
		} else if (id == DUCKDB_TYPE_BLOB) {
			return duckdb_create_blob((const uint8_t *)p, len);
		} else if (id == DUCKDB_TYPE_BIT) {
			duckdb_bit b;
			b.data = (uint8_t *)p;
			b.size = len;
			return duckdb_create_bit(b);
		}
		return bignum_value((const uint8_t *)p, len);
	}
	case DUCKDB_TYPE_ENUM: {
		uint64_t idx;
		switch (duckdb_enum_internal_type(type)) {
		case DUCKDB_TYPE_UTINYINT: idx = ((uint8_t *)data)[row]; break;
		case DUCKDB_TYPE_USMALLINT: idx = ((uint16_t *)data)[row]; break;
		default: idx = ((uint32_t *)data)[row]; break;
		}
		return duckdb_create_enum_value(type, idx);
	}
	case DUCKDB_TYPE_LIST:
	case DUCKDB_TYPE_MAP: {
		duckdb_list_entry entry = ((duckdb_list_entry *)data)[row];
		duckdb_vector child = duckdb_list_vector_get_child(vec);
		duckdb_value result;
		if (id == DUCKDB_TYPE_LIST) {
			duckdb_logical_type child_type = duckdb_list_type_child_type(type);
			duckdb_value *items = xcalloc((size_t)entry.length, sizeof(duckdb_value));
			for (idx_t i = 0; i < entry.length; i++) {
				items[i] = make_value(child, child_type, entry.offset + i);
			}
			result = duckdb_create_list_value(child_type, items, entry.length);
			for (idx_t i = 0; i < entry.length; i++) {
				duckdb_destroy_value(&items[i]);
			}
			free(items);
			duckdb_destroy_logical_type(&child_type);
		} else {
			duckdb_logical_type key_type = duckdb_map_type_key_type(type);
			duckdb_logical_type value_type = duckdb_map_type_value_type(type);
			duckdb_vector key_vec = duckdb_struct_vector_get_child(child, 0);
			duckdb_vector value_vec = duckdb_struct_vector_get_child(child, 1);
			duckdb_value *keys = xcalloc((size_t)entry.length, sizeof(duckdb_value));
			duckdb_value *values = xcalloc((size_t)entry.length, sizeof(duckdb_value));
			for (idx_t i = 0; i < entry.length; i++) {
				keys[i] = make_value(key_vec, key_type, entry.offset + i);
				values[i] = make_value(value_vec, value_type, entry.offset + i);
			}
			result = duckdb_create_map_value(type, keys, values, entry.length);
			for (idx_t i = 0; i < entry.length; i++) {
				duckdb_destroy_value(&keys[i]);
				duckdb_destroy_value(&values[i]);
			}
			free(keys);
			free(values);
			duckdb_destroy_logical_type(&key_type);
			duckdb_destroy_logical_type(&value_type);
		}
		return result;
	}
	case DUCKDB_TYPE_ARRAY: {
		idx_t size = duckdb_array_type_array_size(type);
		duckdb_logical_type child_type = duckdb_array_type_child_type(type);
		duckdb_vector child = duckdb_array_vector_get_child(vec);
		duckdb_value *items = xcalloc((size_t)size, sizeof(duckdb_value));
		for (idx_t i = 0; i < size; i++) {
			items[i] = make_value(child, child_type, row * size + i);
		}
		duckdb_value result = duckdb_create_array_value(child_type, items, size);
		for (idx_t i = 0; i < size; i++) {
			duckdb_destroy_value(&items[i]);
		}
		free(items);
		duckdb_destroy_logical_type(&child_type);
		return result;
	}
	case DUCKDB_TYPE_STRUCT: {
		idx_t n = duckdb_struct_type_child_count(type);
		duckdb_value *fields = xcalloc((size_t)n, sizeof(duckdb_value));
		for (idx_t i = 0; i < n; i++) {
			duckdb_logical_type t = duckdb_struct_type_child_type(type, i);
			fields[i] = make_value(duckdb_struct_vector_get_child(vec, i), t, row);
			duckdb_destroy_logical_type(&t);
		}
		duckdb_value result = duckdb_create_struct_value(type, fields);
		for (idx_t i = 0; i < n; i++) {
			duckdb_destroy_value(&fields[i]);
		}
		free(fields);
		return result;
	}
	case DUCKDB_TYPE_UNION: {
		/* A union is a struct whose first child is the member tag. */
		uint8_t tag = ((uint8_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(vec, 0)))[row];
		duckdb_logical_type t = duckdb_union_type_member_type(type, tag);
		duckdb_value member = make_value(duckdb_struct_vector_get_child(vec, (idx_t)tag + 1), t, row);
		duckdb_value result = duckdb_create_union_value(type, tag, member);
		duckdb_destroy_value(&member);
		duckdb_destroy_logical_type(&t);
		return result;
	}
	default:
		return NULL;
	}
}

static void append_int(strbuf *out, duckdb_type id, const void *data, idx_t row) {
	switch (id) {
	case DUCKDB_TYPE_TINYINT: sb_appendf(out, "%d", (int)((const int8_t *)data)[row]); break;
	case DUCKDB_TYPE_SMALLINT: sb_appendf(out, "%d", (int)((const int16_t *)data)[row]); break;
	case DUCKDB_TYPE_INTEGER: sb_appendf(out, "%" PRId32, ((const int32_t *)data)[row]); break;
	case DUCKDB_TYPE_BIGINT: sb_appendf(out, "%" PRId64, ((const int64_t *)data)[row]); break;
	case DUCKDB_TYPE_UTINYINT: sb_appendf(out, "%u", (unsigned)((const uint8_t *)data)[row]); break;
	case DUCKDB_TYPE_USMALLINT: sb_appendf(out, "%u", (unsigned)((const uint16_t *)data)[row]); break;
	case DUCKDB_TYPE_UINTEGER: sb_appendf(out, "%" PRIu32, ((const uint32_t *)data)[row]); break;
	default: sb_appendf(out, "%" PRIu64, ((const uint64_t *)data)[row]); break;
	}
}

bool format_cell(duckdb_vector vec, duckdb_logical_type type, idx_t row, strbuf *out) {
	if (!row_is_valid(vec, row)) {
		return false;
	}
	duckdb_type id = duckdb_get_type_id(type);
	void *data = duckdb_vector_get_data(vec);
	/* Fast paths for the common scalar types; their text is unambiguous. */
	switch (id) {
	case DUCKDB_TYPE_VARCHAR: {
		duckdb_string_t *s = &((duckdb_string_t *)data)[row];
		sb_append_len(out, duckdb_string_t_data(s), duckdb_string_t_length(*s));
		return true;
	}
	case DUCKDB_TYPE_BOOLEAN:
		sb_append(out, ((bool *)data)[row] ? "true" : "false");
		return true;
	case DUCKDB_TYPE_TINYINT:
	case DUCKDB_TYPE_SMALLINT:
	case DUCKDB_TYPE_INTEGER:
	case DUCKDB_TYPE_BIGINT:
	case DUCKDB_TYPE_UTINYINT:
	case DUCKDB_TYPE_USMALLINT:
	case DUCKDB_TYPE_UINTEGER:
	case DUCKDB_TYPE_UBIGINT:
		append_int(out, id, data, row);
		return true;
	default:
		break;
	}
	duckdb_value v = make_value(vec, type, row);
	if (!v) {
		/* No stable C API constructor for this type (e.g. GEOMETRY, VARIANT). */
		char *name = format_type_name(type);
		sb_appendf(out, "<%s>", name);
		free(name);
		return true;
	}
	/* Display text, as the DuckDB shell shows it (duckdb_value_to_string gives SQL literals instead). */
	append_owned(out, duckdb_get_varchar(v));
	duckdb_destroy_value(&v);
	return true;
}
