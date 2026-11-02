#include "config.h"

#include "platform.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Settings keys, shared by the config file and the environment. */
static const char *const CONFIG_KEYS[] = {
    "DUCKDB_HOST_BIND",   "DUCKDB_HOST_PORT", "DUCKDB_QUACK_TOKEN",           "DUCKDB_QUACK_URI",
    "DUCKDB_QUACK_SSL",   "DUCKDB_QUACK_SSL_FINGERPRINT", "DUCKDB_QUACK_ALIAS", "DUCKDB_LOCAL_DATABASE",
};

void config_init(client_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->port = DEFAULT_QUACK_PORT;
	cfg->ssl = SSL_DEFAULT;
}

void config_free(client_config *cfg) {
	free(cfg->host);
	free(cfg->token);
	free(cfg->uri);
	free(cfg->ssl_fingerprint);
	free(cfg->alias);
	free(cfg->local_path);
	free(cfg->loaded_from);
	config_init(cfg);
}

static void set_str(char **dst, const char *value) {
	free(*dst);
	*dst = (value && *value) ? xstrdup(value) : NULL;
}

static bool fail(char **err, const char *fmt, const char *a, const char *b) {
	strbuf sb;
	sb_init(&sb);
	sb_appendf(&sb, fmt, a, b);
	*err = sb_detach(&sb);
	return false;
}

bool config_set(client_config *cfg, const char *key, const char *value, char **err) {
	if (strcmp(key, "DUCKDB_HOST_BIND") == 0) {
		set_str(&cfg->host, value);
	} else if (strcmp(key, "DUCKDB_HOST_PORT") == 0) {
		unsigned long long port;
		if (!parse_uint(value, 65535, &port) || port == 0) {
			return fail(err, "invalid %s value '%s': expected a port number 1-65535", key, value);
		}
		cfg->port = (unsigned)port;
	} else if (strcmp(key, "DUCKDB_QUACK_TOKEN") == 0) {
		set_str(&cfg->token, value);
	} else if (strcmp(key, "DUCKDB_QUACK_URI") == 0) {
		if (*value && !str_istarts_with(value, "quack:")) {
			return fail(err, "invalid %s value '%s': must start with quack:", key, value);
		}
		set_str(&cfg->uri, value);
	} else if (strcmp(key, "DUCKDB_QUACK_SSL") == 0) {
		bool on;
		if (!*value) {
			cfg->ssl = SSL_DEFAULT;
		} else if (parse_bool(value, &on)) {
			cfg->ssl = on ? SSL_ON : SSL_OFF;
		} else {
			return fail(err, "invalid %s value '%s': expected true or false", key, value);
		}
	} else if (strcmp(key, "DUCKDB_QUACK_SSL_FINGERPRINT") == 0) {
		set_str(&cfg->ssl_fingerprint, value);
	} else if (strcmp(key, "DUCKDB_QUACK_ALIAS") == 0) {
		set_str(&cfg->alias, value);
	} else if (strcmp(key, "DUCKDB_LOCAL_DATABASE") == 0) {
		set_str(&cfg->local_path, value);
	}
	return true;
}

static char *unquote(char *v) {
	size_t n = strlen(v);
	if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
		v[n - 1] = '\0';
		return v + 1;
	}
	/* Unquoted values may carry a trailing " # comment". */
	char *hash = strstr(v, " #");
	if (hash) {
		*hash = '\0';
	}
	return str_trim(v);
}

bool config_load_file(client_config *cfg, const char *path, char **err) {
	FILE *f = plat_fopen(path, "rb");
	if (!f) {
		return fail(err, "cannot open config file '%s'%s", path, "");
	}
	bool ok = true;
	char *line;
	unsigned lineno = 0;
	while (ok && plat_read_line_stream(f, &line) == PLAT_LINE_OK) {
		lineno++;
		char *s = str_trim(line);
		/* Skip a UTF-8 byte order mark on the first line. */
		if (lineno == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
		    (unsigned char)s[2] == 0xBF) {
			s += 3;
		}
		if (*s == '\0' || *s == '#') {
			free(line);
			continue;
		}
		if (strncmp(s, "export ", 7) == 0) {
			s = str_trim(s + 7);
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			strbuf sb;
			sb_init(&sb);
			sb_appendf(&sb, "%s:%u: expected KEY=VALUE", path, lineno);
			*err = sb_detach(&sb);
			ok = false;
		} else {
			*eq = '\0';
			char *key = str_trim(s);
			char *value = unquote(str_trim(eq + 1));
			char *set_err = NULL;
			if (!config_set(cfg, key, value, &set_err)) {
				strbuf sb;
				sb_init(&sb);
				sb_appendf(&sb, "%s:%u: %s", path, lineno, set_err);
				free(set_err);
				*err = sb_detach(&sb);
				ok = false;
			}
		}
		free(line);
	}
	fclose(f);
	if (ok) {
		set_str(&cfg->loaded_from, path);
		if (cfg->token && plat_file_is_shared(path)) {
			fprintf(stderr, "Warning: config file '%s' contains a token and is readable by other users "
			                "(chmod 600 it).\n",
			        path);
		}
	}
	return ok;
}

char *config_default_path(void) {
	const char *env = getenv("DUCKB2CLI_CONFIG");
	if (env && *env) {
		return xstrdup(env);
	}
	if (plat_file_exists("duckb2cli.env")) {
		return xstrdup("duckb2cli.env");
	}
	char *dir = plat_config_dir();
	if (!dir) {
		return NULL;
	}
	strbuf sb;
	sb_init(&sb);
	sb_appendf(&sb, "%s%sduckb2cli%sconfig.env", dir, plat_path_sep(), plat_path_sep());
	free(dir);
	char *path = sb_detach(&sb);
	if (plat_file_exists(path)) {
		return path;
	}
	free(path);
	return NULL;
}

bool config_apply_env(client_config *cfg, char **err) {
	for (size_t i = 0; i < sizeof(CONFIG_KEYS) / sizeof(CONFIG_KEYS[0]); i++) {
		const char *v = getenv(CONFIG_KEYS[i]);
		if (v && !config_set(cfg, CONFIG_KEYS[i], v, err)) {
			return false;
		}
	}
	return true;
}

char *config_server_uri(const client_config *cfg) {
	if (cfg->uri) {
		return xstrdup(cfg->uri);
	}
	if (!cfg->host) {
		return NULL;
	}
	strbuf sb;
	sb_init(&sb);
	bool ipv6 = strchr(cfg->host, ':') != NULL && cfg->host[0] != '[';
	sb_appendf(&sb, ipv6 ? "quack://[%s]:%u" : "quack://%s:%u", cfg->host, cfg->port);
	return sb_detach(&sb);
}
