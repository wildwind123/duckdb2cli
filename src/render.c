#include "render.h"

#include "format.h"
#include "platform.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static const struct {
	const char *name;
	output_mode mode;
} MODES[] = {
    {"box", MODE_BOX},   {"csv", MODE_CSV},         {"tsv", MODE_TSV},           {"json", MODE_JSON},
    {"jsonlines", MODE_JSONLINES}, {"line", MODE_LINE}, {"markdown", MODE_MARKDOWN},
};

#define MODE_COUNT (sizeof(MODES) / sizeof(MODES[0]))

bool render_parse_mode(const char *name, output_mode *out) {
	for (size_t i = 0; i < MODE_COUNT; i++) {
		if (str_ieq(name, MODES[i].name)) {
			*out = MODES[i].mode;
			return true;
		}
	}
	if (str_ieq(name, "duckbox") || str_ieq(name, "table")) {
		*out = MODE_BOX;
		return true;
	}
	if (str_ieq(name, "ndjson")) {
		*out = MODE_JSONLINES;
		return true;
	}
	return false;
}

const char *render_mode_name(output_mode mode) {
	for (size_t i = 0; i < MODE_COUNT; i++) {
		if (MODES[i].mode == mode) {
			return MODES[i].name;
		}
	}
	return "?";
}

const char *render_mode_list(void) {
	return "box, csv, tsv, json, jsonlines, line, markdown";
}

/* ---- Result columns ---- */

typedef struct {
	idx_t count;
	const char **names;
	duckdb_logical_type *types;
	duckdb_type *ids;
	char **type_names;
	bool *numeric;
} columns;

static void columns_load(columns *cols, duckdb_result *result) {
	idx_t n = duckdb_column_count(result);
	cols->count = n;
	cols->names = xcalloc((size_t)n, sizeof(char *));
	cols->types = xcalloc((size_t)n, sizeof(duckdb_logical_type));
	cols->ids = xcalloc((size_t)n, sizeof(duckdb_type));
	cols->type_names = xcalloc((size_t)n, sizeof(char *));
	cols->numeric = xcalloc((size_t)n, sizeof(bool));
	for (idx_t c = 0; c < n; c++) {
		const char *name = duckdb_column_name(result, c);
		cols->names[c] = name ? name : "";
		cols->types[c] = duckdb_column_logical_type(result, c);
		cols->ids[c] = duckdb_get_type_id(cols->types[c]);
		cols->type_names[c] = format_type_name(cols->types[c]);
		cols->numeric[c] = format_type_is_numeric(cols->ids[c]);
	}
}

static void columns_free(columns *cols) {
	for (idx_t c = 0; c < cols->count; c++) {
		duckdb_destroy_logical_type(&cols->types[c]);
		free(cols->type_names[c]);
	}
	free(cols->names);
	free(cols->types);
	free(cols->ids);
	free(cols->type_names);
	free(cols->numeric);
}

static const char *null_text(const render_options *opts) {
	if (opts->null_text) {
		return opts->null_text;
	}
	return (opts->mode == MODE_CSV || opts->mode == MODE_TSV) ? "" : "NULL";
}

/* ---- Escaping helpers ---- */

static void put_csv_field(FILE *out, const char *s, size_t len) {
	bool quote = false;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') {
			quote = true;
			break;
		}
	}
	if (!quote) {
		fwrite(s, 1, len, out);
		return;
	}
	fputc('"', out);
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '"') {
			fputc('"', out);
		}
		fputc(s[i], out);
	}
	fputc('"', out);
}

static void put_tsv_field(FILE *out, const char *s, size_t len) {
	for (size_t i = 0; i < len; i++) {
		switch (s[i]) {
		case '\t': fputs("\\t", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\\': fputs("\\\\", out); break;
		default: fputc(s[i], out); break;
		}
	}
}

static void put_json_string(FILE *out, const char *s, size_t len) {
	fputc('"', out);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\t': fputs("\\t", out); break;
		default:
			if (c < 0x20) {
				fprintf(out, "\\u%04x", c);
			} else {
				fputc(c, out);
			}
		}
	}
	fputc('"', out);
}

static void put_json_value(FILE *out, duckdb_type id, bool is_null, const char *s, size_t len) {
	if (is_null) {
		fputs("null", out);
	} else if (id == DUCKDB_TYPE_BOOLEAN) {
		fwrite(s, 1, len, out);
	} else if (format_type_is_numeric(id)) {
		/* inf and nan have no JSON representation. */
		bool finite = len > 0 && (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) && !strstr(s, "inf") &&
		              !strstr(s, "nan");
		if (finite) {
			fwrite(s, 1, len, out);
		} else {
			fputs("null", out);
		}
	} else {
		put_json_string(out, s, len);
	}
}

static void put_markdown_field(FILE *out, const char *s, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '|') {
			fputs("\\|", out);
		} else if (s[i] == '\n' || s[i] == '\r') {
			fputc(' ', out);
		} else {
			fputc(s[i], out);
		}
	}
}

/* Box cells: control characters are shown escaped so rows stay on one line. */
static char *box_cell_text(const char *s, size_t len) {
	strbuf sb;
	sb_init(&sb);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n') {
			sb_append(&sb, "\\n");
		} else if (c == '\r') {
			sb_append(&sb, "\\r");
		} else if (c == '\t') {
			sb_append(&sb, "\\t");
		} else if (c < 0x20 || c == 0x7F) {
			sb_appendf(&sb, "\\x%02X", c);
		} else {
			sb_append_char(&sb, (char)c);
		}
	}
	return sb_detach(&sb);
}

static void put_repeat(FILE *out, const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		fputs(s, out);
	}
}

/* Writes s padded (or truncated with an ellipsis) to exactly `width` columns. */
static void put_aligned(FILE *out, const char *s, size_t width, int align /* -1 left, 0 center, 1 right */) {
	size_t len = strlen(s);
	size_t w = utf8_width(s, len);
	if (w > width) {
		size_t fit_w;
		size_t fit = utf8_prefix_fitting(s, len, width > 0 ? width - 1 : 0, &fit_w);
		fwrite(s, 1, fit, out);
		fputs("…", out);
		put_repeat(out, " ", width - fit_w - (width > 0 ? 1 : 0));
		return;
	}
	size_t pad = width - w;
	size_t left = align < 0 ? 0 : align > 0 ? pad : pad / 2;
	put_repeat(out, " ", left);
	fwrite(s, 1, len, out);
	put_repeat(out, " ", pad - left);
}

static void put_border(FILE *out, const size_t *widths, idx_t n, const char *l, const char *m, const char *r) {
	fputs(l, out);
	for (idx_t c = 0; c < n; c++) {
		put_repeat(out, "─", widths[c] + 2);
		fputs(c + 1 < n ? m : r, out);
	}
	fputc('\n', out);
}

/* ---- Box mode ---- */

static void render_box(FILE *out, duckdb_result *result, columns *cols, const render_options *opts,
                       volatile sig_atomic_t *interrupted, unsigned long long *row_count) {
	idx_t n = cols->count;
	size_t cap = 64, kept = 0;
	char **cells = xmalloc(cap * (size_t)n * sizeof(char *));
	unsigned long long total = 0;
	strbuf sb;
	sb_init(&sb);

	duckdb_data_chunk chunk;
	while (!*interrupted && (chunk = duckdb_fetch_chunk(*result)) != NULL) {
		idx_t size = duckdb_data_chunk_get_size(chunk);
		for (idx_t r = 0; r < size; r++, total++) {
			if (opts->max_rows && total >= opts->max_rows) {
				continue;
			}
			if (kept == cap) {
				cap *= 2;
				cells = xrealloc(cells, cap * (size_t)n * sizeof(char *));
			}
			for (idx_t c = 0; c < n; c++) {
				sb_clear(&sb);
				bool valid = format_cell(duckdb_data_chunk_get_vector(chunk, c), cols->types[c], r, &sb);
				cells[kept * n + c] = valid ? box_cell_text(sb.data, sb.len) : NULL;
			}
			kept++;
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	sb_free(&sb);
	*row_count = total;

	const char *null_str = null_text(opts);
	size_t *widths = xcalloc((size_t)n, sizeof(size_t));
	for (idx_t c = 0; c < n; c++) {
		size_t w = utf8_width(cols->names[c], strlen(cols->names[c]));
		size_t tw = utf8_width(cols->type_names[c], strlen(cols->type_names[c]));
		widths[c] = w > tw ? w : tw;
		for (size_t r = 0; r < kept; r++) {
			const char *s = cells[r * n + c] ? cells[r * n + c] : null_str;
			size_t cw = utf8_width(s, strlen(s));
			if (cw > widths[c]) {
				widths[c] = cw;
			}
		}
		if (opts->max_width && widths[c] > opts->max_width) {
			widths[c] = opts->max_width;
		}
	}
	/* Shrink the widest columns until the table fits the terminal. */
	int term = (out == stdout && plat_stdout_is_tty()) ? plat_terminal_width() : 0;
	if (!opts->max_width && term > 0) {
		const size_t min_width = 6;
		for (;;) {
			size_t total_w = 1, widest = 0;
			for (idx_t c = 0; c < n; c++) {
				total_w += widths[c] + 3;
				if (widths[c] > widths[widest]) {
					widest = (size_t)c;
				}
			}
			if (total_w <= (size_t)term || widths[widest] <= min_width) {
				break;
			}
			widths[widest]--;
		}
	}

	if (n > 0) {
		put_border(out, widths, n, "┌", "┬", "┐");
		if (opts->headers) {
			fputs("│", out);
			for (idx_t c = 0; c < n; c++) {
				fputc(' ', out);
				put_aligned(out, cols->names[c], widths[c], 0);
				fputs(" │", out);
			}
			fputc('\n', out);
			fputs("│", out);
			for (idx_t c = 0; c < n; c++) {
				fputc(' ', out);
				put_aligned(out, cols->type_names[c], widths[c], 0);
				fputs(" │", out);
			}
			fputc('\n', out);
			if (kept > 0) {
				put_border(out, widths, n, "├", "┼", "┤");
			}
		}
		for (size_t r = 0; r < kept; r++) {
			fputs("│", out);
			for (idx_t c = 0; c < n; c++) {
				const char *s = cells[r * n + c];
				fputc(' ', out);
				put_aligned(out, s ? s : null_str, widths[c], cols->numeric[c] ? 1 : -1);
				fputs(" │", out);
			}
			fputc('\n', out);
		}
		put_border(out, widths, n, "└", "┴", "┘");
	}
	if (*interrupted) {
		fprintf(out, "%llu rows read before interrupt\n", total);
	} else if (kept < total) {
		fprintf(out, "%llu rows (%llu shown, see .maxrows)\n", total, (unsigned long long)kept);
	} else {
		fprintf(out, "%llu %s\n", total, total == 1 ? "row" : "rows");
	}

	for (size_t i = 0; i < kept * (size_t)n; i++) {
		free(cells[i]);
	}
	free(cells);
	free(widths);
}

/* ---- Row-at-a-time modes ---- */

static void render_rows(FILE *out, duckdb_result *result, columns *cols, const render_options *opts,
                        volatile sig_atomic_t *interrupted, unsigned long long *row_count) {
	idx_t n = cols->count;
	const char *null_str = null_text(opts);
	size_t name_width = 0;
	for (idx_t c = 0; c < n; c++) {
		size_t w = utf8_width(cols->names[c], strlen(cols->names[c]));
		if (w > name_width) {
			name_width = w;
		}
	}

	if (opts->headers && n > 0) {
		if (opts->mode == MODE_CSV || opts->mode == MODE_TSV) {
			for (idx_t c = 0; c < n; c++) {
				if (c) {
					fputc(opts->mode == MODE_CSV ? ',' : '\t', out);
				}
				const char *name = cols->names[c];
				if (opts->mode == MODE_CSV) {
					put_csv_field(out, name, strlen(name));
				} else {
					put_tsv_field(out, name, strlen(name));
				}
			}
			fputc('\n', out);
		} else if (opts->mode == MODE_MARKDOWN) {
			fputc('|', out);
			for (idx_t c = 0; c < n; c++) {
				fputc(' ', out);
				put_markdown_field(out, cols->names[c], strlen(cols->names[c]));
				fputs(" |", out);
			}
			fputs("\n|", out);
			for (idx_t c = 0; c < n; c++) {
				fputs(cols->numeric[c] ? "---:|" : "---|", out);
			}
			fputc('\n', out);
		}
	}
	if (opts->mode == MODE_JSON) {
		fputc('[', out);
	}

	unsigned long long total = 0;
	strbuf sb;
	sb_init(&sb);
	duckdb_data_chunk chunk;
	while (!*interrupted && (chunk = duckdb_fetch_chunk(*result)) != NULL) {
		idx_t size = duckdb_data_chunk_get_size(chunk);
		for (idx_t r = 0; r < size; r++, total++) {
			if (opts->mode == MODE_JSON) {
				fputs(total ? ",\n" : "\n", out);
			}
			if (opts->mode == MODE_JSON || opts->mode == MODE_JSONLINES) {
				fputc('{', out);
			} else if (opts->mode == MODE_MARKDOWN) {
				fputc('|', out);
			} else if (opts->mode == MODE_LINE && total) {
				fputc('\n', out);
			}
			for (idx_t c = 0; c < n; c++) {
				sb_clear(&sb);
				bool valid = format_cell(duckdb_data_chunk_get_vector(chunk, c), cols->types[c], r, &sb);
				const char *s = valid ? sb.data : null_str;
				size_t len = valid ? sb.len : strlen(null_str);
				switch (opts->mode) {
				case MODE_CSV:
					if (c) {
						fputc(',', out);
					}
					put_csv_field(out, s, len);
					break;
				case MODE_TSV:
					if (c) {
						fputc('\t', out);
					}
					put_tsv_field(out, s, len);
					break;
				case MODE_JSON:
				case MODE_JSONLINES:
					if (c) {
						fputc(',', out);
					}
					put_json_string(out, cols->names[c], strlen(cols->names[c]));
					fputc(':', out);
					put_json_value(out, cols->ids[c], !valid, s, len);
					break;
				case MODE_MARKDOWN:
					fputc(' ', out);
					put_markdown_field(out, s, len);
					fputs(" |", out);
					break;
				case MODE_LINE:
					put_repeat(out, " ", name_width - utf8_width(cols->names[c], strlen(cols->names[c])));
					fprintf(out, "%s = ", cols->names[c]);
					fwrite(s, 1, len, out);
					fputc('\n', out);
					break;
				default:
					break;
				}
			}
			if (opts->mode == MODE_JSON) {
				fputc('}', out);
			} else if (opts->mode != MODE_LINE) {
				fputs(opts->mode == MODE_JSONLINES ? "}\n" : "\n", out);
			}
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	sb_free(&sb);
	if (opts->mode == MODE_JSON) {
		fputs(total ? "\n]\n" : "]\n", out);
	}
	*row_count = total;
}

void render_result(FILE *out, duckdb_result *result, const render_options *opts,
                   volatile sig_atomic_t *interrupted, unsigned long long *row_count) {
	columns cols;
	columns_load(&cols, result);
	*row_count = 0;
	if (opts->mode == MODE_BOX) {
		render_box(out, result, &cols, opts, interrupted, row_count);
	} else {
		render_rows(out, result, &cols, opts, interrupted, row_count);
	}
	columns_free(&cols);
	fflush(out);
}
