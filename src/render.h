#ifndef DUCKB2CLI_RENDER_H
#define DUCKB2CLI_RENDER_H

#include <duckdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
	MODE_BOX,
	MODE_CSV,
	MODE_TSV,
	MODE_JSON,
	MODE_JSONLINES,
	MODE_LINE,
	MODE_MARKDOWN,
} output_mode;

typedef struct {
	output_mode mode;
	bool headers;
	/* Text for NULL; NULL means the mode's default ("NULL", or empty for csv/tsv). */
	char *null_text;
	/* Box mode: rows shown before the rest are only counted (0 = all). */
	unsigned long long max_rows;
	/* Box mode: widest a column may get, in terminal columns (0 = fit to terminal). */
	unsigned max_width;
} render_options;

bool render_parse_mode(const char *name, output_mode *out);
const char *render_mode_name(output_mode mode);
/* Comma-separated list of every mode name. */
const char *render_mode_list(void);

/*
 * Writes every row of a query result. Stops early once *interrupted becomes non-zero.
 * The number of rows read goes to *row_count.
 */
void render_result(FILE *out, duckdb_result *result, const render_options *opts,
                   volatile sig_atomic_t *interrupted, unsigned long long *row_count);

#endif
