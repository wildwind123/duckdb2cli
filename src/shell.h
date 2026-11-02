#ifndef DUCKB2CLI_SHELL_H
#define DUCKB2CLI_SHELL_H

#include "config.h"
#include "render.h"

#include <duckdb.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
	duckdb_database db;
	duckdb_connection conn;
	/* Second connection used to cancel server-side work on Ctrl+C (quack mode only). */
	duckdb_connection cancel_conn;
	render_options render;
	bool timer;
	bool bail;
	bool echo;
	bool interactive;
	bool quit_requested;
	int exit_code;
	unsigned errors;
	FILE *out;
	/* Connection description for .status and the banner. */
	char *uri;
	char *alias;
	char *local_path;
	/* Secret to scrub from any error text shown to the user. */
	char *token;
} session;

/* Opens the local engine and, unless cfg->local_path is set, attaches the quack server. */
bool session_open(session *s, const client_config *cfg, char **err);
void session_close(session *s);

/* Runs one or more SQL statements. Returns false if any failed. */
bool session_run_sql(session *s, const char *sql);
/* Runs a dot command such as ".mode csv". Returns false on error. */
bool session_run_dot(session *s, const char *line);
/* Reads statements and dot commands from `in` (or the terminal when interactive) until EOF or .quit. */
bool session_run_input(session *s, FILE *in, bool interactive);
/* Runs the statements in a file. */
bool session_run_file(session *s, const char *path);

void session_print_banner(session *s);

#endif
