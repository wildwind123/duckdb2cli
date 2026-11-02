#ifndef DUCKB2CLI_FORMAT_H
#define DUCKB2CLI_FORMAT_H

#include "util.h"

#include <duckdb.h>
#include <stdbool.h>

/* SQL-style type name such as "integer[]" or "struct(a bigint)" (malloc'ed). */
char *format_type_name(duckdb_logical_type type);

/* Numeric types are right-aligned in tables and unquoted in JSON. */
bool format_type_is_numeric(duckdb_type id);

/*
 * Appends the text of row `row` of `vec` (of logical type `type`) to `out`.
 * Returns false (appending nothing) if the value is NULL.
 */
bool format_cell(duckdb_vector vec, duckdb_logical_type type, idx_t row, strbuf *out);

#endif
