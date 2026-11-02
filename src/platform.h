#ifndef DUCKB2CLI_PLATFORM_H
#define DUCKB2CLI_PLATFORM_H

#include <stdbool.h>
#include <stdio.h>

typedef enum { PLAT_LINE_OK, PLAT_LINE_EOF, PLAT_LINE_INTERRUPTED } plat_line_status;

/* Called on Ctrl+C, on a dedicated thread (never in signal context), so it may call into DuckDB. */
typedef void (*plat_interrupt_fn)(void);

/* Console setup: UTF-8 output on Windows, line editing defaults elsewhere. */
void plat_init(void);
/* Windows: replaces argv with UTF-8 arguments. No-op elsewhere. */
void plat_utf8_args(int *argc, char ***argv);

bool plat_stdin_is_tty(void);
bool plat_stdout_is_tty(void);
bool plat_stderr_is_tty(void);
int plat_terminal_width(void);

void plat_set_interrupt_handler(plat_interrupt_fn fn);

/* Interactive line input with editing/history where available. *out is malloc'ed, without the newline. */
plat_line_status plat_read_line_interactive(const char *prompt, char **out);
/* Line input from a non-interactive stream. */
plat_line_status plat_read_line_stream(FILE *in, char **out);
/* Prompt the next interactive read should show after an interrupt cleared the line. */
void plat_set_reset_prompt(const char *prompt);

void plat_history_load(const char *path);
void plat_history_add(const char *line);
void plat_history_save(const char *path);

/* fopen that accepts UTF-8 paths on every platform. */
FILE *plat_fopen(const char *path, const char *mode);
bool plat_file_exists(const char *path);
/* True if the file can be read by users other than its owner (POSIX only). */
bool plat_file_is_shared(const char *path);

/* Per-user directories (malloc'ed, NULL if unknown). */
char *plat_home_dir(void);
char *plat_config_dir(void);
const char *plat_path_sep(void);

double plat_now_seconds(void);

#endif
