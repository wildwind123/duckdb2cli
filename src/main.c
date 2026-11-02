#include "config.h"
#include "platform.h"
#include "render.h"
#include "shell.h"
#include "util.h"
#include "version.h"

#include <duckdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out) {
	fputs("Usage: duckb2cli [OPTIONS] [SQL...]\n"
	      "\n"
	      "Interactive SQL client for DuckDB servers speaking the Quack protocol.\n"
	      "With no SQL, -c or -f it starts an interactive shell (or reads SQL from piped stdin).\n"
	      "\n"
	      "Connection:\n"
	      "  -H, --host HOST           Server host           (env DUCKDB_HOST_BIND)\n"
	      "  -p, --port PORT           Server port, default 9494 (env DUCKDB_HOST_PORT)\n"
	      "  -t, --token TOKEN         Auth token            (env DUCKDB_QUACK_TOKEN)\n"
	      "  -u, --uri URI             Full quack:// URI, overrides host/port (env DUCKDB_QUACK_URI)\n"
	      "      --ssl, --no-ssl       Force TLS on or off   (env DUCKDB_QUACK_SSL)\n"
	      "      --ssl-fingerprint HEX Pin the server certificate's SHA-256 (env DUCKDB_QUACK_SSL_FINGERPRINT)\n"
	      "  -a, --alias NAME          Name of the attached database, default \"remote\"\n"
	      "  -l, --local PATH          Open a local database file instead (\":memory:\" allowed)\n"
	      "      --config FILE         Settings file (KEY=VALUE lines, same keys as the env vars)\n"
	      "\n"
	      "Execution:\n"
	      "  -c, --command SQL         Run SQL and exit (repeatable, dot commands allowed)\n"
	      "  -f, --file FILE           Run a SQL file and exit (repeatable)\n"
	      "  -i, --interactive         Start the shell after -c/-f instead of exiting\n"
	      "  -b, --bail                Stop at the first error\n"
	      "  -e, --echo                Print each statement before running it\n"
	      "\n"
	      "Output:\n"
	      "  -m, --mode MODE           box, csv, tsv, json, jsonlines, line, markdown (default box)\n"
	      "      --csv, --json, --line, --markdown, --tsv, --jsonlines   Shortcuts for --mode\n"
	      "      --no-header           Omit column headers\n"
	      "      --null TEXT           Text shown for NULL\n"
	      "      --max-rows N          Rows shown in box mode, 0 for all (default 1000)\n"
	      "  -o, --output FILE         Write results to FILE\n"
	      "\n"
	      "  -h, --help                Show this help\n"
	      "  -v, --version             Show version information\n"
	      "\n"
	      "Settings are read from the config file, then the environment, then the command line;\n"
	      "later sources win. Without --config the first existing file of ./duckb2cli.env,\n"
	      "$DUCKB2CLI_CONFIG and the per-user config.env (~/.config/duckb2cli or %APPDATA%\\duckb2cli)\n"
	      "is used.\n",
	      out);
}

typedef enum { ACTION_SQL, ACTION_FILE } action_kind;

typedef struct {
	action_kind kind;
	const char *text;
} action;

typedef struct {
	int argc;
	char **argv;
	int i;
} arg_reader;

/* Matches "-x", "--long" or "--long=value"; *value receives the option's argument if it takes one. */
static bool match_opt(arg_reader *r, const char *short_name, const char *long_name, bool takes_value,
                      const char **value, bool *missing) {
	const char *arg = r->argv[r->i];
	const char *inline_value = NULL;
	bool matched = false;
	if (short_name && strcmp(arg, short_name) == 0) {
		matched = true;
	} else if (long_name) {
		size_t n = strlen(long_name);
		if (strncmp(arg, long_name, n) == 0) {
			if (arg[n] == '\0') {
				matched = true;
			} else if (arg[n] == '=' && takes_value) {
				matched = true;
				inline_value = arg + n + 1;
			}
		}
	}
	if (!matched) {
		return false;
	}
	*missing = false;
	if (takes_value) {
		if (inline_value) {
			*value = inline_value;
		} else if (r->i + 1 < r->argc) {
			*value = r->argv[++r->i];
		} else {
			*missing = true;
		}
	}
	return true;
}

static int die(const char *msg) {
	fprintf(stderr, "duckb2cli: %s\n", msg);
	return 2;
}

int main(int argc, char **argv) {
	plat_init();
	plat_utf8_args(&argc, &argv);

	client_config cfg;
	config_init(&cfg);
	/* Command-line settings are applied after the file and environment, so collect them first. */
	client_config cli;
	config_init(&cli);
	bool cli_port = false, cli_ssl = false;

	const char *config_path = NULL;
	action *actions = xcalloc((size_t)argc + 1, sizeof(action));
	size_t action_count = 0;
	bool force_interactive = false, bail = false, echo = false, headers = true;
	output_mode mode = MODE_BOX;
	const char *null_text = NULL, *output_path = NULL;
	unsigned long long max_rows = 1000;

	arg_reader r = {argc, argv, 1};
	for (; r.i < argc; r.i++) {
		const char *v = NULL;
		bool missing = false;
		const char *arg = argv[r.i];
		if (arg[0] != '-' || strcmp(arg, "-") == 0) {
			actions[action_count].kind = ACTION_SQL;
			actions[action_count++].text = arg;
			continue;
		}
		if (strcmp(arg, "--") == 0) {
			for (r.i++; r.i < argc; r.i++) {
				actions[action_count].kind = ACTION_SQL;
				actions[action_count++].text = argv[r.i];
			}
			break;
		}
#define OPT(s, l, val) match_opt(&r, s, l, val, &v, &missing)
		if (OPT("-h", "--help", false)) {
			print_usage(stdout);
			return 0;
		} else if (OPT("-v", "--version", false)) {
			printf("duckb2cli %s (DuckDB %s)\n", DUCKB2CLI_VERSION, duckdb_library_version());
			return 0;
		} else if (OPT("-H", "--host", true)) {
			if (!missing) {
				free(cli.host);
				cli.host = xstrdup(v);
			}
		} else if (OPT("-p", "--port", true)) {
			if (!missing) {
				char *err = NULL;
				if (!config_set(&cli, "DUCKDB_HOST_PORT", v, &err)) {
					return die(err);
				}
				cli_port = true;
			}
		} else if (OPT("-t", "--token", true)) {
			if (!missing) {
				free(cli.token);
				cli.token = xstrdup(v);
			}
		} else if (OPT("-u", "--uri", true)) {
			if (!missing) {
				char *err = NULL;
				if (!config_set(&cli, "DUCKDB_QUACK_URI", v, &err)) {
					return die(err);
				}
			}
		} else if (OPT(NULL, "--ssl", false)) {
			cli.ssl = SSL_ON;
			cli_ssl = true;
		} else if (OPT(NULL, "--no-ssl", false)) {
			cli.ssl = SSL_OFF;
			cli_ssl = true;
		} else if (OPT(NULL, "--ssl-fingerprint", true)) {
			if (!missing) {
				free(cli.ssl_fingerprint);
				cli.ssl_fingerprint = xstrdup(v);
			}
		} else if (OPT("-a", "--alias", true)) {
			if (!missing) {
				free(cli.alias);
				cli.alias = xstrdup(v);
			}
		} else if (OPT("-l", "--local", true)) {
			if (!missing) {
				free(cli.local_path);
				cli.local_path = xstrdup(v);
			}
		} else if (OPT(NULL, "--config", true)) {
			config_path = v;
		} else if (OPT("-c", "--command", true)) {
			if (!missing) {
				actions[action_count].kind = ACTION_SQL;
				actions[action_count++].text = v;
			}
		} else if (OPT("-f", "--file", true)) {
			if (!missing) {
				actions[action_count].kind = ACTION_FILE;
				actions[action_count++].text = v;
			}
		} else if (OPT("-i", "--interactive", false)) {
			force_interactive = true;
		} else if (OPT("-b", "--bail", false)) {
			bail = true;
		} else if (OPT("-e", "--echo", false)) {
			echo = true;
		} else if (OPT("-m", "--mode", true)) {
			if (!missing && !render_parse_mode(v, &mode)) {
				fprintf(stderr, "duckb2cli: unknown mode '%s'; choose one of: %s\n", v, render_mode_list());
				return 2;
			}
		} else if (strncmp(arg, "--", 2) == 0 && render_parse_mode(arg + 2, &mode)) {
			/* --csv, --json, ... */
		} else if (OPT(NULL, "--no-header", false) || OPT(NULL, "--no-headers", false)) {
			headers = false;
		} else if (OPT(NULL, "--null", true)) {
			null_text = v;
		} else if (OPT(NULL, "--max-rows", true)) {
			if (!missing && !parse_uint(v, ~0ULL, &max_rows)) {
				return die("--max-rows expects a number");
			}
		} else if (OPT("-o", "--output", true)) {
			output_path = v;
		} else {
			fprintf(stderr, "duckb2cli: unknown option '%s' (see --help)\n", arg);
			return 2;
		}
		if (missing) {
			fprintf(stderr, "duckb2cli: option '%s' needs a value\n", arg);
			return 2;
		}
#undef OPT
	}

	/* Config file, then environment, then command line. */
	char *err = NULL;
	char *default_path = config_path ? NULL : config_default_path();
	const char *file = config_path ? config_path : default_path;
	if (file && !config_load_file(&cfg, file, &err)) {
		return die(err);
	}
	free(default_path);
	if (!config_apply_env(&cfg, &err)) {
		return die(err);
	}
	if (cli.host) {
		free(cfg.host);
		cfg.host = xstrdup(cli.host);
		/* An explicit host replaces any configured URI. */
		free(cfg.uri);
		cfg.uri = NULL;
	}
	if (cli_port) {
		cfg.port = cli.port;
	}
	if (cli.token) {
		free(cfg.token);
		cfg.token = xstrdup(cli.token);
	}
	if (cli.uri) {
		free(cfg.uri);
		cfg.uri = xstrdup(cli.uri);
	}
	if (cli_ssl) {
		cfg.ssl = cli.ssl;
	}
	if (cli.ssl_fingerprint) {
		free(cfg.ssl_fingerprint);
		cfg.ssl_fingerprint = xstrdup(cli.ssl_fingerprint);
	}
	if (cli.alias) {
		free(cfg.alias);
		cfg.alias = xstrdup(cli.alias);
	}
	if (cli.local_path) {
		free(cfg.local_path);
		cfg.local_path = xstrdup(cli.local_path);
	}
	config_free(&cli);

	bool stdin_tty = plat_stdin_is_tty();
	bool interactive = force_interactive || (action_count == 0 && stdin_tty);

	session s;
	memset(&s, 0, sizeof(s));
	s.out = stdout;
	s.render.mode = mode;
	s.render.headers = headers;
	s.render.null_text = null_text ? xstrdup(null_text) : NULL;
	s.render.max_rows = max_rows;
	s.bail = bail;
	s.echo = echo;
	s.interactive = interactive;

	if (output_path) {
		s.out = plat_fopen(output_path, "wb");
		if (!s.out) {
			fprintf(stderr, "duckb2cli: cannot open '%s' for writing\n", output_path);
			return 2;
		}
	}

	if (!session_open(&s, &cfg, &err)) {
		fprintf(stderr, "Error: %s\n", err);
		free(err);
		config_free(&cfg);
		session_close(&s);
		return 1;
	}
	config_free(&cfg);

	bool ok = true;
	for (size_t i = 0; i < action_count && !s.quit_requested; i++) {
		bool step_ok;
		if (actions[i].kind == ACTION_FILE) {
			step_ok = session_run_file(&s, actions[i].text);
		} else if (strcmp(actions[i].text, "-") == 0) {
			step_ok = session_run_input(&s, stdin, false);
		} else {
			const char *text = actions[i].text;
			while (*text == ' ' || *text == '\t') {
				text++;
			}
			step_ok = text[0] == '.' ? session_run_dot(&s, text) : session_run_sql(&s, text);
		}
		ok = step_ok && ok;
		if (!step_ok && bail) {
			break;
		}
	}
	free(actions);

	if (!s.quit_requested && (ok || !bail)) {
		if (interactive) {
			session_print_banner(&s);
			session_run_input(&s, stdin, true);
		} else if (action_count == 0) {
			ok = session_run_input(&s, stdin, false);
		}
	}

	int code = s.quit_requested ? s.exit_code : (interactive ? 0 : (ok ? 0 : 1));
	session_close(&s);
	return code;
}
