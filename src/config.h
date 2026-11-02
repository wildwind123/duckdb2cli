#ifndef DUCKB2CLI_CONFIG_H
#define DUCKB2CLI_CONFIG_H

#include <stdbool.h>

#define DEFAULT_QUACK_PORT 9494
#define DEFAULT_ALIAS      "remote"

typedef enum { SSL_DEFAULT, SSL_ON, SSL_OFF } ssl_mode;

/* Connection settings. Every string is malloc'ed or NULL. */
typedef struct {
	char *host;
	unsigned port;
	char *token;
	/* Full quack:// URI; takes precedence over host/port. */
	char *uri;
	ssl_mode ssl;
	char *ssl_fingerprint;
	/* Name the remote database is attached as. */
	char *alias;
	/* Open this local database file instead of connecting to a server (":memory:" allowed). */
	char *local_path;
	/* Config file that was loaded, for diagnostics. */
	char *loaded_from;
} client_config;

void config_init(client_config *cfg);
void config_free(client_config *cfg);

/* Applies one KEY=VALUE setting. Unknown keys are ignored; returns false on a bad value. */
bool config_set(client_config *cfg, const char *key, const char *value, char **err);
/* Loads a .env style file (KEY=VALUE lines, # comments, optional quotes and "export "). */
bool config_load_file(client_config *cfg, const char *path, char **err);
/* Path of the config file to use when none is given explicitly (malloc'ed, NULL if none exists). */
char *config_default_path(void);
/* Overrides settings from DUCKDB_* environment variables. */
bool config_apply_env(client_config *cfg, char **err);

/* The quack:// URI to connect to (malloc'ed), or NULL if no server is configured. */
char *config_server_uri(const client_config *cfg);

#endif
