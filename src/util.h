#ifndef DUCKB2CLI_UTIL_H
#define DUCKB2CLI_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Growable, always NUL-terminated byte buffer. */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_clear(strbuf *sb);
void sb_append(strbuf *sb, const char *s);
void sb_append_len(strbuf *sb, const char *s, size_t n);
void sb_append_char(strbuf *sb, char c);
void sb_appendf(strbuf *sb, const char *fmt, ...);
/* Returns the buffer contents (caller frees) and resets the buffer. */
char *sb_detach(strbuf *sb);

void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

bool str_ieq(const char *a, const char *b);
bool str_istarts_with(const char *s, const char *prefix);
/* Trims ASCII whitespace in place and returns the trimmed start. */
char *str_trim(char *s);
/* Replaces every occurrence of needle in s; returns a new string. */
char *str_replace_all(const char *s, const char *needle, const char *replacement);
bool parse_bool(const char *s, bool *out);
bool parse_uint(const char *s, unsigned long long max, unsigned long long *out);

/* Appends s as a SQL string literal ('...' with quotes doubled). */
void sql_quote_literal(strbuf *sb, const char *s);
/* Appends s as a SQL identifier ("..." with quotes doubled). */
void sql_quote_ident(strbuf *sb, const char *s);

/* Number of terminal columns the UTF-8 text occupies. */
size_t utf8_width(const char *s, size_t len);
/* Longest prefix (in bytes) of s that fits in max_width columns; its width goes to *out_width. */
size_t utf8_prefix_fitting(const char *s, size_t len, size_t max_width, size_t *out_width);

#endif
