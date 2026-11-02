#include "shell.h"

#include "platform.h"
#include "util.h"
#include "version.h"

#include <ctype.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* Interrupt state shared with the Ctrl+C handler thread. */
static duckdb_connection g_conn = NULL;
static duckdb_connection g_cancel_conn = NULL;
static char *g_cancel_sql = NULL;
static volatile sig_atomic_t g_query_running = 0;
static volatile sig_atomic_t g_query_interrupted = 0;
static volatile sig_atomic_t g_prompt_interrupted = 0;

static void on_interrupt(void) {
	if (g_query_running) {
		g_query_interrupted = 1;
		if (g_conn) {
			duckdb_interrupt(g_conn);
		}
		/* duckdb_interrupt only stops local work; the server must be told separately. */
		if (g_cancel_conn && g_cancel_sql) {
			duckdb_result res;
			duckdb_query(g_cancel_conn, g_cancel_sql, &res);
			duckdb_destroy_result(&res);
		}
	} else {
		g_prompt_interrupted = 1;
	}
}

static void print_error(session *s, const char *prefix, const char *msg) {
	char *clean = NULL;
	if (msg && s->token && *s->token) {
		clean = str_replace_all(msg, s->token, "***");
		msg = clean;
	}
	fflush(s->out);
	fprintf(stderr, "%s%s\n", prefix, msg ? msg : "unknown error");
	fflush(stderr);
	free(clean);
	s->errors++;
}

/* Runs SQL internal to the client, reporting errors but not rendering results. */
static bool exec_quiet(session *s, const char *sql, char **err) {
	duckdb_result res;
	bool ok = duckdb_query(s->conn, sql, &res) == DuckDBSuccess;
	if (!ok && err) {
		const char *msg = duckdb_result_error(&res);
		char *clean = (msg && s->token && *s->token) ? str_replace_all(msg, s->token, "***") : NULL;
		*err = clean ? clean : xstrdup(msg ? msg : "unknown error");
	}
	duckdb_destroy_result(&res);
	return ok;
}

static bool load_quack(session *s, char **err) {
	if (exec_quiet(s, "LOAD quack", NULL)) {
		return true;
	}
	if (s->interactive) {
		fputs("Installing the quack extension...\n", stderr);
	}
	return exec_quiet(s, "INSTALL quack", err) && exec_quiet(s, "LOAD quack", err);
}

bool session_open(session *s, const client_config *cfg, char **err) {
	duckdb_config dbcfg;
	if (duckdb_create_config(&dbcfg) == DuckDBError) {
		*err = xstrdup("failed to create DuckDB configuration");
		return false;
	}
	duckdb_set_config(dbcfg, "custom_user_agent", "duckb2cli/" DUCKB2CLI_VERSION);
	char *open_err = NULL;
	const char *path = cfg->local_path ? cfg->local_path : ":memory:";
	duckdb_state st = duckdb_open_ext(path, &s->db, dbcfg, &open_err);
	duckdb_destroy_config(&dbcfg);
	if (st == DuckDBError) {
		strbuf sb;
		sb_init(&sb);
		sb_appendf(&sb, "cannot open database '%s': %s", path, open_err ? open_err : "unknown error");
		duckdb_free(open_err);
		*err = sb_detach(&sb);
		return false;
	}
	if (duckdb_connect(s->db, &s->conn) == DuckDBError) {
		*err = xstrdup("cannot create a connection");
		return false;
	}
	g_conn = s->conn;
	plat_set_interrupt_handler(on_interrupt);

	if (cfg->local_path) {
		s->local_path = xstrdup(cfg->local_path);
		return true;
	}

	char *uri = config_server_uri(cfg);
	if (!uri) {
		*err = xstrdup("no server configured: set DUCKDB_HOST_BIND (or use --host, --uri or --local)");
		return false;
	}
	s->uri = uri;
	s->alias = xstrdup(cfg->alias ? cfg->alias : DEFAULT_ALIAS);
	s->token = cfg->token ? xstrdup(cfg->token) : NULL;

	if (!load_quack(s, err)) {
		return false;
	}

	strbuf sql;
	sb_init(&sql);
	sb_append(&sql, "ATTACH ");
	sql_quote_literal(&sql, uri);
	sb_append(&sql, " AS ");
	sql_quote_ident(&sql, s->alias);
	sb_append(&sql, " (TYPE quack");
	if (cfg->token) {
		sb_append(&sql, ", TOKEN ");
		sql_quote_literal(&sql, cfg->token);
	}
	if (cfg->ssl != SSL_DEFAULT) {
		sb_append(&sql, cfg->ssl == SSL_ON ? ", DISABLE_SSL false" : ", DISABLE_SSL true");
	}
	if (cfg->ssl_fingerprint) {
		sb_append(&sql, ", SSL_FINGERPRINT ");
		sql_quote_literal(&sql, cfg->ssl_fingerprint);
	}
	sb_append(&sql, ")");
	char *attach_err = NULL;
	bool ok = exec_quiet(s, sql.data, &attach_err);
	/* The statement holds the token; wipe it before freeing. */
	memset(sql.data, 0, sql.len);
	sb_free(&sql);
	if (!ok) {
		strbuf sb;
		sb_init(&sb);
		sb_appendf(&sb, "cannot connect to %s: %s", uri, attach_err);
		/* The raw TLS errors do not say what to change. */
		if (strstr(attach_err, "Unsupported protocol") || strstr(attach_err, "wrong version number")) {
			sb_append(&sb, "\nHint: the server may require TLS; try --ssl (DUCKDB_QUACK_SSL=true).");
		} else if (strstr(attach_err, "certificate") && !cfg->ssl_fingerprint) {
			sb_append(&sb, "\nHint: for a self-signed server certificate, pin it with --ssl-fingerprint "
			               "(DUCKDB_QUACK_SSL_FINGERPRINT); the server prints it in quack_generate_keys().");
		} else if (strstr(attach_err, "SSL connect error") && cfg->ssl == SSL_ON) {
			sb_append(&sb, "\nHint: the server may not use TLS; try without --ssl.");
		}
		free(attach_err);
		*err = sb_detach(&sb);
		return false;
	}

	sb_init(&sql);
	sb_append(&sql, "USE ");
	sql_quote_ident(&sql, s->alias);
	ok = exec_quiet(s, sql.data, err);
	sb_free(&sql);

	if (ok && duckdb_connect(s->db, &s->cancel_conn) == DuckDBSuccess) {
		sb_init(&sql);
		sb_append(&sql, "FROM quack_cancel(");
		sql_quote_literal(&sql, s->alias);
		sb_append(&sql, ", quack_connection_id(");
		sql_quote_literal(&sql, s->alias);
		sb_append(&sql, "))");
		g_cancel_sql = sb_detach(&sql);
		g_cancel_conn = s->cancel_conn;
	}
	return ok;
}

void session_close(session *s) {
	if (s->out && s->out != stdout) {
		fclose(s->out);
	}
	s->out = stdout;
	g_conn = NULL;
	g_cancel_conn = NULL;
	if (s->cancel_conn) {
		duckdb_disconnect(&s->cancel_conn);
	}
	duckdb_disconnect(&s->conn);
	duckdb_close(&s->db);
	free(s->uri);
	free(s->alias);
	free(s->local_path);
	if (s->token) {
		memset(s->token, 0, strlen(s->token));
		free(s->token);
	}
	free(s->render.null_text);
	s->uri = s->alias = s->local_path = s->token = s->render.null_text = NULL;
}

void session_print_banner(session *s) {
	printf("duckb2cli %s (DuckDB %s)\n", DUCKB2CLI_VERSION, duckdb_library_version());
	if (s->uri) {
		printf("Connected to %s as database \"%s\".\n", s->uri, s->alias);
	} else {
		printf("Using local database %s.\n", s->local_path);
	}
	printf("Enter \".help\" for usage hints.\n");
}

/* ---- Statement execution ---- */

typedef enum { RESULT_ROWS, RESULT_CHANGED_COUNT, RESULT_STATUS } result_shape;

/* True if the result is a single column with this name and type. */
static bool single_column(duckdb_result *res, const char *name, duckdb_type type) {
	if (duckdb_column_count(res) != 1) {
		return false;
	}
	const char *col = duckdb_column_name(res, 0);
	return col && strcmp(col, name) == 0 && duckdb_column_type(res, 0) == type;
}

/*
 * DML and DDL answer with a one-column "Count" or "Success" table; those are reported
 * as a short message rather than rendered, so scripted csv/json output stays clean.
 */
static result_shape classify_result(duckdb_result *res, duckdb_statement_type type) {
	switch (type) {
	case DUCKDB_STATEMENT_TYPE_SELECT:
	case DUCKDB_STATEMENT_TYPE_EXPLAIN:
	case DUCKDB_STATEMENT_TYPE_PRAGMA:
	case DUCKDB_STATEMENT_TYPE_CALL:
	case DUCKDB_STATEMENT_TYPE_EXECUTE:
	case DUCKDB_STATEMENT_TYPE_RELATION:
		return RESULT_ROWS;
	case DUCKDB_STATEMENT_TYPE_INSERT:
	case DUCKDB_STATEMENT_TYPE_UPDATE:
	case DUCKDB_STATEMENT_TYPE_DELETE:
	case DUCKDB_STATEMENT_TYPE_COPY:
	case DUCKDB_STATEMENT_TYPE_MERGE_INTO:
	case DUCKDB_STATEMENT_TYPE_CREATE:
		if (single_column(res, "Count", DUCKDB_TYPE_BIGINT)) {
			return RESULT_CHANGED_COUNT;
		}
		break;
	default:
		break;
	}
	return single_column(res, "Success", DUCKDB_TYPE_BOOLEAN) ? RESULT_STATUS : RESULT_ROWS;
}

static int64_t read_count(duckdb_result *res) {
	int64_t total = 0;
	duckdb_data_chunk chunk;
	while ((chunk = duckdb_fetch_chunk(*res)) != NULL) {
		duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, 0);
		const int64_t *data = duckdb_vector_get_data(vec);
		uint64_t *validity = duckdb_vector_get_validity(vec);
		for (idx_t r = 0; r < duckdb_data_chunk_get_size(chunk); r++) {
			if (!validity || duckdb_validity_row_is_valid(validity, r)) {
				total += data[r];
			}
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	return total;
}

static bool run_prepared(session *s, duckdb_prepared_statement ps, double start) {
	duckdb_result res;
	duckdb_state st = duckdb_execute_prepared(ps, &res);
	if (st == DuckDBError) {
		print_error(s, "Error: ", g_query_interrupted ? "Interrupted" : duckdb_result_error(&res));
		duckdb_destroy_result(&res);
		return false;
	}
	unsigned long long rows = 0;
	duckdb_result_type kind = duckdb_result_return_type(res);
	result_shape shape = RESULT_STATUS;
	if (kind == DUCKDB_RESULT_TYPE_QUERY_RESULT) {
		shape = classify_result(&res, duckdb_prepared_statement_type(ps));
	}
	if (kind == DUCKDB_RESULT_TYPE_CHANGED_ROWS) {
		shape = RESULT_CHANGED_COUNT;
	}
	if (shape == RESULT_ROWS) {
		render_result(s->out, &res, &s->render, &g_query_interrupted, &rows);
	} else if (shape == RESULT_CHANGED_COUNT) {
		int64_t changed = kind == DUCKDB_RESULT_TYPE_CHANGED_ROWS ? (int64_t)duckdb_rows_changed(&res)
		                                                          : read_count(&res);
		if (s->interactive) {
			fprintf(s->out, "%lld %s changed\n", (long long)changed, changed == 1 ? "row" : "rows");
		}
	}
	duckdb_destroy_result(&res);
	if (s->timer) {
		fprintf(s->out, "Run Time: %.3fs\n", plat_now_seconds() - start);
	}
	fflush(s->out);
	if (g_query_interrupted) {
		print_error(s, "Error: ", "Interrupted");
		return false;
	}
	return true;
}

bool session_run_sql(session *s, const char *sql) {
	duckdb_extracted_statements stmts = NULL;
	idx_t count = duckdb_extract_statements(s->conn, sql, &stmts);
	if (count == 0) {
		const char *msg = duckdb_extract_statements_error(stmts);
		bool ok = msg == NULL;
		if (!ok) {
			print_error(s, "Error: ", msg);
		}
		duckdb_destroy_extracted(&stmts);
		return ok;
	}
	bool ok = true;
	for (idx_t i = 0; i < count; i++) {
		double start = plat_now_seconds();
		/* Binding can already run remote work (e.g. quack_query_by_name), so Ctrl+C covers prepare too. */
		g_query_interrupted = 0;
		g_query_running = 1;
		duckdb_prepared_statement ps = NULL;
		bool stmt_ok;
		if (duckdb_prepare_extracted_statement(s->conn, stmts, i, &ps) == DuckDBError) {
			print_error(s, "Error: ", g_query_interrupted ? "Interrupted" : duckdb_prepare_error(ps));
			stmt_ok = false;
		} else {
			stmt_ok = run_prepared(s, ps, start);
		}
		g_query_running = 0;
		duckdb_destroy_prepare(&ps);
		if (!stmt_ok) {
			ok = false;
			break;
		}
	}
	duckdb_destroy_extracted(&stmts);
	if (!s->interactive && (g_query_interrupted || g_prompt_interrupted)) {
		/* Ctrl+C ends a script instead of moving on to the next statement. */
		s->quit_requested = true;
		s->exit_code = 130;
	}
	return ok;
}

/*
 * True when the buffer ends in a ';' outside of quotes and comments,
 * i.e. the user has finished typing at least one statement.
 */
static bool sql_is_complete(const char *s) {
	enum { NORMAL, SINGLE, DOUBLE, LINE_COMMENT, BLOCK_COMMENT, DOLLAR } state = NORMAL;
	int block_depth = 0;
	const char *dollar_tag = NULL;
	size_t dollar_len = 0;
	char last = 0;
	for (const char *p = s; *p; p++) {
		switch (state) {
		case NORMAL:
			if (*p == '\'') {
				state = SINGLE;
			} else if (*p == '"') {
				state = DOUBLE;
			} else if (p[0] == '-' && p[1] == '-') {
				state = LINE_COMMENT;
				p++;
				continue;
			} else if (p[0] == '/' && p[1] == '*') {
				state = BLOCK_COMMENT;
				block_depth = 1;
				p++;
				continue;
			} else if (*p == '$') {
				/* $tag$ ... $tag$ quoting; $1 parameters are not tags. */
				const char *q = p + 1;
				while (isalnum((unsigned char)*q) || *q == '_') {
					q++;
				}
				if (*q == '$' && !isdigit((unsigned char)p[1])) {
					state = DOLLAR;
					dollar_tag = p;
					dollar_len = (size_t)(q - p) + 1;
					p = q;
				}
			}
			if (!isspace((unsigned char)*p)) {
				last = *p;
			}
			break;
		case SINGLE:
			if (*p == '\'') {
				state = NORMAL;
			}
			break;
		case DOUBLE:
			if (*p == '"') {
				state = NORMAL;
			}
			break;
		case LINE_COMMENT:
			if (*p == '\n') {
				state = NORMAL;
			}
			break;
		case BLOCK_COMMENT:
			if (p[0] == '*' && p[1] == '/') {
				p++;
				if (--block_depth == 0) {
					state = NORMAL;
				}
			} else if (p[0] == '/' && p[1] == '*') {
				p++;
				block_depth++;
			}
			break;
		case DOLLAR:
			if (strncmp(p, dollar_tag, dollar_len) == 0) {
				p += dollar_len - 1;
				state = NORMAL;
				last = '$';
			}
			break;
		}
	}
	return (state == NORMAL || state == LINE_COMMENT) && last == ';';
}

static bool is_blank_sql(const char *s) {
	for (; *s; s++) {
		if (!isspace((unsigned char)*s)) {
			return false;
		}
	}
	return true;
}

/* ---- Dot commands ---- */

static const char *HELP_TEXT =
    ".bail on|off          Stop a script at the first error (default off)\n"
    ".databases            List attached databases\n"
    ".echo on|off          Print each statement before running it\n"
    ".exit [CODE]          Exit (also .quit, Ctrl+D; Ctrl+Z Enter on Windows)\n"
    ".headers on|off       Show column headers\n"
    ".help                 Show this message\n"
    ".maxrows N            Rows shown in box mode, 0 for all\n"
    ".maxwidth N           Widest column in box mode, 0 to fit the terminal\n"
    ".mode [MODE]          Output mode: box, csv, tsv, json, jsonlines, line, markdown\n"
    ".nullvalue TEXT       Text shown for NULL\n"
    ".output [FILE]        Send results to FILE, or back to the terminal with no argument\n"
    ".read FILE            Run the statements in FILE\n"
    ".remote SQL           Run SQL on the server as-is (e.g. server-side SET or PRAGMA)\n"
    ".schema [TABLE]       Show CREATE statements, or the columns of TABLE\n"
    ".status               Show connection and settings\n"
    ".tables               List tables in the current database\n"
    ".timer on|off         Show how long each statement takes\n"
    "\n"
    "SQL statements end with ';' and may span several lines. Ctrl+C cancels a running query.\n";

/* Splits a dot command into its name and the (trimmed) rest of the line. */
static void split_command(char *line, char **name, char **arg) {
	char *p = line + 1;
	*name = p;
	while (*p && !isspace((unsigned char)*p)) {
		p++;
	}
	if (*p) {
		*p++ = '\0';
	}
	*arg = str_trim(p);
}

static bool set_flag(session *s, const char *name, const char *arg, bool *flag) {
	if (!parse_bool(arg, flag)) {
		strbuf sb;
		sb_init(&sb);
		sb_appendf(&sb, "usage: .%s on|off", name);
		print_error(s, "", sb.data);
		sb_free(&sb);
		return false;
	}
	return true;
}

static bool dot_remote(session *s, const char *arg) {
	if (!s->alias) {
		print_error(s, "", ".remote needs a quack connection (not available with --local)");
		return false;
	}
	if (!*arg) {
		print_error(s, "", "usage: .remote SQL");
		return false;
	}
	strbuf sql;
	sb_init(&sql);
	sb_append(&sql, "FROM quack_query_by_name(");
	sql_quote_literal(&sql, s->alias);
	sb_append(&sql, ", ");
	sql_quote_literal(&sql, arg);
	sb_append(&sql, ")");
	bool ok = session_run_sql(s, sql.data);
	sb_free(&sql);
	return ok;
}

static bool dot_status(session *s) {
	FILE *o = s->out;
	fprintf(o, "duckb2cli:   %s\n", DUCKB2CLI_VERSION);
	fprintf(o, "DuckDB:      %s (local engine)\n", duckdb_library_version());
	if (s->uri) {
		fprintf(o, "Server:      %s\n", s->uri);
		fprintf(o, "Database:    %s\n", s->alias);
		fprintf(o, "Token:       %s\n", s->token ? "set" : "not set");
	} else {
		fprintf(o, "Database:    %s (local)\n", s->local_path);
	}
	fprintf(o, "Mode:        %s\n", render_mode_name(s->render.mode));
	fprintf(o, "Headers:     %s\n", s->render.headers ? "on" : "off");
	fprintf(o, "Max rows:    %llu\n", s->render.max_rows);
	fprintf(o, "Timer:       %s\n", s->timer ? "on" : "off");
	fflush(o);
	if (s->uri) {
		strbuf sql;
		sb_init(&sql);
		sb_append(&sql, "FROM quack_query_by_name(");
		sql_quote_literal(&sql, s->alias);
		sb_append(&sql, ", 'SELECT version() AS server_version, current_database() AS server_database')");
		bool ok = session_run_sql(s, sql.data);
		sb_free(&sql);
		return ok;
	}
	return true;
}

bool session_run_dot(session *s, const char *input) {
	char *line = xstrdup(input);
	char *name, *arg;
	split_command(str_trim(line), &name, &arg);
	bool ok = true;

	if (str_ieq(name, "quit") || str_ieq(name, "exit") || str_ieq(name, "q")) {
		unsigned long long code = 0;
		if (*arg && !parse_uint(arg, 255, &code)) {
			print_error(s, "", "usage: .exit [CODE]");
			ok = false;
		} else {
			s->quit_requested = true;
			s->exit_code = (int)code;
		}
	} else if (str_ieq(name, "help")) {
		fputs(HELP_TEXT, s->out);
	} else if (str_ieq(name, "mode")) {
		if (!*arg) {
			fprintf(s->out, "%s\n", render_mode_name(s->render.mode));
		} else if (!render_parse_mode(arg, &s->render.mode)) {
			strbuf sb;
			sb_init(&sb);
			sb_appendf(&sb, "unknown mode '%s'; choose one of: %s", arg, render_mode_list());
			print_error(s, "", sb.data);
			sb_free(&sb);
			ok = false;
		}
	} else if (str_ieq(name, "headers") || str_ieq(name, "header")) {
		ok = set_flag(s, "headers", arg, &s->render.headers);
	} else if (str_ieq(name, "timer")) {
		ok = set_flag(s, "timer", arg, &s->timer);
	} else if (str_ieq(name, "bail")) {
		ok = set_flag(s, "bail", arg, &s->bail);
	} else if (str_ieq(name, "echo")) {
		ok = set_flag(s, "echo", arg, &s->echo);
	} else if (str_ieq(name, "nullvalue")) {
		free(s->render.null_text);
		s->render.null_text = xstrdup(arg);
	} else if (str_ieq(name, "maxrows") || str_ieq(name, "maxwidth")) {
		unsigned long long v;
		bool rows = str_ieq(name, "maxrows");
		if (!parse_uint(arg, rows ? ~0ULL : 100000, &v)) {
			print_error(s, "", rows ? "usage: .maxrows N" : "usage: .maxwidth N");
			ok = false;
		} else if (rows) {
			s->render.max_rows = v;
		} else {
			s->render.max_width = (unsigned)v;
		}
	} else if (str_ieq(name, "output")) {
		FILE *f = stdout;
		if (*arg && (f = plat_fopen(arg, "wb")) == NULL) {
			strbuf sb;
			sb_init(&sb);
			sb_appendf(&sb, "cannot open '%s' for writing", arg);
			print_error(s, "", sb.data);
			sb_free(&sb);
			ok = false;
		} else {
			if (s->out != stdout) {
				fclose(s->out);
			}
			s->out = f;
		}
	} else if (str_ieq(name, "read")) {
		if (!*arg) {
			print_error(s, "", "usage: .read FILE");
			ok = false;
		} else {
			ok = session_run_file(s, arg);
		}
	} else if (str_ieq(name, "tables")) {
		ok = session_run_sql(s, "SHOW TABLES");
	} else if (str_ieq(name, "databases")) {
		ok = session_run_sql(s, "SHOW DATABASES");
	} else if (str_ieq(name, "schema")) {
		strbuf sql;
		sb_init(&sql);
		if (*arg) {
			sb_append(&sql, "DESCRIBE ");
			sb_append(&sql, arg);
		} else {
			sb_append(&sql, "SELECT sql FROM duckdb_tables() WHERE database_name = current_database() "
			                "UNION ALL SELECT sql FROM duckdb_views() "
			                "WHERE database_name = current_database() AND NOT internal");
		}
		ok = session_run_sql(s, sql.data);
		sb_free(&sql);
	} else if (str_ieq(name, "remote")) {
		ok = dot_remote(s, arg);
	} else if (str_ieq(name, "status")) {
		ok = dot_status(s);
	} else {
		strbuf sb;
		sb_init(&sb);
		sb_appendf(&sb, "unknown command '.%s'; enter \".help\" for a list", name);
		print_error(s, "", sb.data);
		sb_free(&sb);
		ok = false;
	}
	fflush(s->out);
	free(line);
	return ok;
}

/* ---- Input loop ---- */

static char *history_path(void) {
	char *home = plat_home_dir();
	if (!home) {
		return NULL;
	}
	strbuf sb;
	sb_init(&sb);
	sb_appendf(&sb, "%s%s.duckb2cli_history", home, plat_path_sep());
	free(home);
	return sb_detach(&sb);
}

static bool run_buffer(session *s, const char *sql) {
	if (s->echo) {
		fprintf(s->out, "%s\n", sql);
	}
	return session_run_sql(s, sql);
}

bool session_run_input(session *s, FILE *in, bool interactive) {
	bool saved_interactive = s->interactive;
	s->interactive = interactive;
	const char *prompt = s->alias ? s->alias : "local";
	strbuf primary, continuation;
	sb_init(&primary);
	sb_init(&continuation);
	sb_appendf(&primary, "%s> ", prompt);
	for (size_t i = 0; i + 5 < primary.len; i++) {
		sb_append_char(&continuation, ' ');
	}
	sb_append(&continuation, "...> ");

	char *hist = NULL;
	if (interactive) {
		hist = history_path();
		plat_history_load(hist);
		plat_set_reset_prompt(primary.data);
	}

	strbuf buf;
	sb_init(&buf);
	bool ok = true;
	while (!s->quit_requested) {
		char *line = NULL;
		if (interactive) {
			g_prompt_interrupted = 0;
		}
		plat_line_status st = interactive
		                          ? plat_read_line_interactive(buf.len ? continuation.data : primary.data, &line)
		                          : plat_read_line_stream(in, &line);
		if (st == PLAT_LINE_EOF) {
			if (!is_blank_sql(buf.data ? buf.data : "")) {
				/* A final statement without ';' still runs. */
				ok = run_buffer(s, buf.data) && ok;
			}
			break;
		}
		if (!interactive && (st == PLAT_LINE_INTERRUPTED || g_prompt_interrupted)) {
			free(line);
			s->quit_requested = true;
			s->exit_code = 130;
			ok = false;
			break;
		}
		if (st == PLAT_LINE_INTERRUPTED) {
			sb_clear(&buf);
			continue;
		}
		if (g_prompt_interrupted) {
			/* Ctrl+C at the prompt discarded the statement typed so far. */
			sb_clear(&buf);
		}

		char *trimmed = str_trim(line);
		if (buf.len == 0 && trimmed[0] == '.') {
			if (interactive) {
				plat_history_add(trimmed);
			}
			if (s->echo) {
				fprintf(s->out, "%s\n", trimmed);
			}
			bool cmd_ok = session_run_dot(s, trimmed);
			ok = cmd_ok && ok;
			free(line);
			if (!cmd_ok && s->bail && !interactive) {
				break;
			}
			continue;
		}
		if (buf.len == 0 && *trimmed == '\0') {
			free(line);
			continue;
		}
		sb_append(&buf, line);
		sb_append_char(&buf, '\n');
		free(line);

		if (sql_is_complete(buf.data)) {
			while (buf.len > 0 && buf.data[buf.len - 1] == '\n') {
				buf.data[--buf.len] = '\0';
			}
			if (interactive) {
				plat_history_add(buf.data);
			}
			bool stmt_ok = run_buffer(s, buf.data);
			ok = stmt_ok && ok;
			sb_clear(&buf);
			if (!stmt_ok && s->bail && !interactive) {
				break;
			}
		}
	}
	if (interactive) {
		plat_history_save(hist);
		free(hist);
		fputc('\n', stdout);
	}
	sb_free(&buf);
	sb_free(&primary);
	sb_free(&continuation);
	s->interactive = saved_interactive;
	return ok;
}

bool session_run_file(session *s, const char *path) {
	FILE *f = plat_fopen(path, "rb");
	if (!f) {
		strbuf sb;
		sb_init(&sb);
		sb_appendf(&sb, "cannot open '%s'", path);
		print_error(s, "Error: ", sb.data);
		sb_free(&sb);
		return false;
	}
	bool ok = session_run_input(s, f, false);
	fclose(f);
	return ok;
}
