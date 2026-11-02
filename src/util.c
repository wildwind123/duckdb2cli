#include "util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("duckb2cli: out of memory\n", stderr);
		exit(70);
	}
	return p;
}

void *xcalloc(size_t count, size_t n) {
	void *p = calloc(count ? count : 1, n ? n : 1);
	if (!p) {
		fputs("duckb2cli: out of memory\n", stderr);
		exit(70);
	}
	return p;
}

void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		fputs("duckb2cli: out of memory\n", stderr);
		exit(70);
	}
	return q;
}

char *xstrdup(const char *s) {
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void sb_init(strbuf *sb) {
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

void sb_free(strbuf *sb) {
	free(sb->data);
	sb_init(sb);
}

void sb_clear(strbuf *sb) {
	sb->len = 0;
	if (sb->data) {
		sb->data[0] = '\0';
	}
}

static void sb_reserve(strbuf *sb, size_t extra) {
	size_t need = sb->len + extra + 1;
	if (need <= sb->cap) {
		return;
	}
	size_t cap = sb->cap ? sb->cap : 64;
	while (cap < need) {
		cap *= 2;
	}
	sb->data = xrealloc(sb->data, cap);
	sb->cap = cap;
}

void sb_append_len(strbuf *sb, const char *s, size_t n) {
	sb_reserve(sb, n);
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
}

void sb_append(strbuf *sb, const char *s) {
	sb_append_len(sb, s, strlen(s));
}

void sb_append_char(strbuf *sb, char c) {
	sb_append_len(sb, &c, 1);
}

void sb_appendf(strbuf *sb, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n > 0) {
		sb_reserve(sb, (size_t)n);
		vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
		sb->len += (size_t)n;
	}
	va_end(ap2);
}

char *sb_detach(strbuf *sb) {
	char *p = sb->data ? sb->data : xstrdup("");
	sb_init(sb);
	return p;
}

bool str_ieq(const char *a, const char *b) {
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == *b;
}

bool str_istarts_with(const char *s, const char *prefix) {
	while (*prefix) {
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
			return false;
		}
		s++;
		prefix++;
	}
	return true;
}

char *str_trim(char *s) {
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[--n] = '\0';
	}
	return s;
}

char *str_replace_all(const char *s, const char *needle, const char *replacement) {
	strbuf sb;
	sb_init(&sb);
	size_t nlen = strlen(needle);
	if (nlen == 0) {
		sb_append(&sb, s);
		return sb_detach(&sb);
	}
	const char *p;
	while ((p = strstr(s, needle)) != NULL) {
		sb_append_len(&sb, s, (size_t)(p - s));
		sb_append(&sb, replacement);
		s = p + nlen;
	}
	sb_append(&sb, s);
	return sb_detach(&sb);
}

bool parse_bool(const char *s, bool *out) {
	if (str_ieq(s, "1") || str_ieq(s, "true") || str_ieq(s, "yes") || str_ieq(s, "on")) {
		*out = true;
		return true;
	}
	if (str_ieq(s, "0") || str_ieq(s, "false") || str_ieq(s, "no") || str_ieq(s, "off")) {
		*out = false;
		return true;
	}
	return false;
}

bool parse_uint(const char *s, unsigned long long max, unsigned long long *out) {
	if (!*s) {
		return false;
	}
	unsigned long long v = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
		v = v * 10 + (unsigned long long)(*s - '0');
		if (v > max) {
			return false;
		}
	}
	*out = v;
	return true;
}

static void sql_quote(strbuf *sb, const char *s, char q) {
	sb_append_char(sb, q);
	for (; *s; s++) {
		if (*s == q) {
			sb_append_char(sb, q);
		}
		sb_append_char(sb, *s);
	}
	sb_append_char(sb, q);
}

void sql_quote_literal(strbuf *sb, const char *s) {
	sql_quote(sb, s, '\'');
}

void sql_quote_ident(strbuf *sb, const char *s) {
	sql_quote(sb, s, '"');
}

/* Decodes one code point; invalid sequences decode as a single byte. */
static size_t utf8_decode(const unsigned char *s, size_t len, uint32_t *cp) {
	unsigned char c = s[0];
	size_t n;
	uint32_t v;
	if (c < 0x80) {
		*cp = c;
		return 1;
	} else if ((c & 0xE0) == 0xC0) {
		n = 2;
		v = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		n = 3;
		v = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		n = 4;
		v = c & 0x07;
	} else {
		*cp = 0xFFFD;
		return 1;
	}
	if (n > len) {
		*cp = 0xFFFD;
		return 1;
	}
	for (size_t i = 1; i < n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*cp = 0xFFFD;
			return 1;
		}
		v = (v << 6) | (s[i] & 0x3F);
	}
	*cp = v;
	return n;
}

static int codepoint_width(uint32_t cp) {
	if (cp == 0) {
		return 0;
	}
	/* Combining marks and zero-width characters. */
	if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
	    (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
	    (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0xFEFF || (cp >= 0xE0100 && cp <= 0xE01EF)) {
		return 0;
	}
	/* East Asian wide/fullwidth and emoji. */
	if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
	    (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
	    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
	    (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
	    (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
		return 2;
	}
	return 1;
}

size_t utf8_width(const char *s, size_t len) {
	size_t w = 0;
	size_t i = 0;
	while (i < len) {
		uint32_t cp;
		i += utf8_decode((const unsigned char *)s + i, len - i, &cp);
		w += (size_t)codepoint_width(cp);
	}
	return w;
}

size_t utf8_prefix_fitting(const char *s, size_t len, size_t max_width, size_t *out_width) {
	size_t w = 0;
	size_t i = 0;
	while (i < len) {
		uint32_t cp;
		size_t n = utf8_decode((const unsigned char *)s + i, len - i, &cp);
		size_t cw = (size_t)codepoint_width(cp);
		if (w + cw > max_width) {
			break;
		}
		w += cw;
		i += n;
	}
	*out_width = w;
	return i;
}
