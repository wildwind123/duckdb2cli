#include "platform.h"

#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <shellapi.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif
#endif

static plat_interrupt_fn g_interrupt_fn = NULL;
static char *g_reset_prompt = NULL;

void plat_set_reset_prompt(const char *prompt) {
	free(g_reset_prompt);
	g_reset_prompt = prompt ? xstrdup(prompt) : NULL;
}

plat_line_status plat_read_line_stream(FILE *in, char **out) {
	strbuf sb;
	sb_init(&sb);
	char chunk[4096];
	for (;;) {
		if (!fgets(chunk, sizeof(chunk), in)) {
			if (ferror(in) && errno == EINTR) {
				clearerr(in);
				sb_free(&sb);
				return PLAT_LINE_INTERRUPTED;
			}
			if (sb.len == 0) {
				sb_free(&sb);
				return PLAT_LINE_EOF;
			}
			break;
		}
		sb_append(&sb, chunk);
		if (sb.len > 0 && sb.data[sb.len - 1] == '\n') {
			break;
		}
	}
	while (sb.len > 0 && (sb.data[sb.len - 1] == '\n' || sb.data[sb.len - 1] == '\r')) {
		sb.data[--sb.len] = '\0';
	}
	*out = sb_detach(&sb);
	return PLAT_LINE_OK;
}

#ifdef _WIN32

static char *wide_to_utf8(const wchar_t *w, int wlen) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
	char *s = xmalloc((size_t)n + 1);
	WideCharToMultiByte(CP_UTF8, 0, w, wlen, s, n, NULL, NULL);
	s[n] = '\0';
	return s;
}

static wchar_t *utf8_to_wide(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
	if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
		w[0] = L'\0';
	}
	return w;
}

static BOOL WINAPI console_ctrl_handler(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
		if (g_interrupt_fn) {
			g_interrupt_fn();
		}
		return TRUE;
	}
	return FALSE;
}

void plat_init(void) {
	SetConsoleOutputCP(CP_UTF8);
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode;
	if (GetConsoleMode(h, &mode)) {
		SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
}

void plat_utf8_args(int *argc, char ***argv) {
	int n = 0;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
	if (!wargv) {
		return;
	}
	char **args = xcalloc((size_t)n + 1, sizeof(char *));
	for (int i = 0; i < n; i++) {
		args[i] = wide_to_utf8(wargv[i], -1);
	}
	LocalFree(wargv);
	*argc = n;
	*argv = args;
}

static bool handle_is_console(DWORD which) {
	DWORD mode;
	return GetConsoleMode(GetStdHandle(which), &mode) != 0;
}

bool plat_stdin_is_tty(void) {
	return handle_is_console(STD_INPUT_HANDLE);
}

bool plat_stdout_is_tty(void) {
	return handle_is_console(STD_OUTPUT_HANDLE);
}

bool plat_stderr_is_tty(void) {
	return handle_is_console(STD_ERROR_HANDLE);
}

int plat_terminal_width(void) {
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
		return info.srWindow.Right - info.srWindow.Left + 1;
	}
	return 0;
}

void plat_set_interrupt_handler(plat_interrupt_fn fn) {
	g_interrupt_fn = fn;
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

plat_line_status plat_read_line_interactive(const char *prompt, char **out) {
	fputs(prompt, stdout);
	fflush(stdout);
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	size_t cap = 1024, len = 0;
	wchar_t *buf = xmalloc(cap * sizeof(wchar_t));
	for (;;) {
		DWORD got = 0;
		if (cap - len < 512) {
			cap *= 2;
			buf = xrealloc(buf, cap * sizeof(wchar_t));
		}
		SetLastError(0);
		if (!ReadConsoleW(in, buf + len, (DWORD)(cap - len - 1), &got, NULL)) {
			free(buf);
			return PLAT_LINE_EOF;
		}
		if (got == 0) {
			/* Ctrl+C aborts the read with no input. */
			free(buf);
			fputs("\n", stdout);
			return PLAT_LINE_INTERRUPTED;
		}
		len += got;
		if (len > 0 && buf[len - 1] == L'\n') {
			break;
		}
	}
	while (len > 0 && (buf[len - 1] == L'\n' || buf[len - 1] == L'\r')) {
		len--;
	}
	/* Ctrl+Z at the start of a line is end of input. */
	if (len >= 1 && buf[0] == 0x1A) {
		free(buf);
		return PLAT_LINE_EOF;
	}
	*out = wide_to_utf8(buf, (int)len);
	free(buf);
	return PLAT_LINE_OK;
}

/* The Windows console keeps its own per-session line history (F7, arrow keys). */
void plat_history_load(const char *path) {
	(void)path;
}

void plat_history_add(const char *line) {
	(void)line;
}

void plat_history_save(const char *path) {
	(void)path;
}

FILE *plat_fopen(const char *path, const char *mode) {
	wchar_t *wpath = utf8_to_wide(path);
	wchar_t *wmode = utf8_to_wide(mode);
	FILE *f = _wfopen(wpath, wmode);
	free(wpath);
	free(wmode);
	return f;
}

bool plat_file_exists(const char *path) {
	wchar_t *wpath = utf8_to_wide(path);
	DWORD attrs = GetFileAttributesW(wpath);
	free(wpath);
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_file_is_shared(const char *path) {
	(void)path;
	return false;
}

static char *env_utf8(const wchar_t *name) {
	const wchar_t *v = _wgetenv(name);
	return (v && *v) ? wide_to_utf8(v, -1) : NULL;
}

char *plat_home_dir(void) {
	return env_utf8(L"USERPROFILE");
}

char *plat_config_dir(void) {
	return env_utf8(L"APPDATA");
}

const char *plat_path_sep(void) {
	return "\\";
}

double plat_now_seconds(void) {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
}

#else /* POSIX */

static volatile sig_atomic_t g_sigint_at_prompt = 0;

/* Self-pipe: the signal handler only writes a byte; a watcher thread runs the callback. */
static int g_interrupt_pipe[2] = {-1, -1};

static void sigint_handler(int sig) {
	(void)sig;
	int saved_errno = errno;
	g_sigint_at_prompt = 1;
	if (g_interrupt_pipe[1] >= 0) {
		ssize_t n = write(g_interrupt_pipe[1], "!", 1);
		(void)n;
	}
	errno = saved_errno;
}

static void *interrupt_watcher(void *arg) {
	(void)arg;
	for (;;) {
		char c;
		ssize_t n = read(g_interrupt_pipe[0], &c, 1);
		if (n == 1) {
			if (g_interrupt_fn) {
				g_interrupt_fn();
			}
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return NULL;
		}
	}
}

#ifdef HAVE_READLINE
/* Runs in normal context after readline sees EINTR: discard the line being edited. */
static int on_readline_signal(void) {
	if (g_sigint_at_prompt) {
		g_sigint_at_prompt = 0;
		/* readline has already echoed ^C. */
		fputs("\n", stdout);
		rl_replace_line("", 0);
		if (g_reset_prompt) {
			rl_set_prompt(g_reset_prompt);
		}
		rl_on_new_line();
		rl_redisplay();
	}
	return 0;
}
#endif

void plat_init(void) {
#ifdef HAVE_READLINE
	rl_readline_name = "duckb2cli";
	rl_signal_event_hook = on_readline_signal;
	/* Tab inserts a tab instead of completing file names. */
	rl_bind_key('\t', rl_insert);
#endif
}

void plat_utf8_args(int *argc, char ***argv) {
	(void)argc;
	(void)argv;
}

bool plat_stdin_is_tty(void) {
	return isatty(STDIN_FILENO) != 0;
}

bool plat_stdout_is_tty(void) {
	return isatty(STDOUT_FILENO) != 0;
}

bool plat_stderr_is_tty(void) {
	return isatty(STDERR_FILENO) != 0;
}

int plat_terminal_width(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
		return ws.ws_col;
	}
	return 0;
}

void plat_set_interrupt_handler(plat_interrupt_fn fn) {
	g_interrupt_fn = fn;
	if (g_interrupt_pipe[0] < 0 && pipe(g_interrupt_pipe) == 0) {
		fcntl(g_interrupt_pipe[0], F_SETFD, FD_CLOEXEC);
		fcntl(g_interrupt_pipe[1], F_SETFD, FD_CLOEXEC);
		/* The watcher must never receive SIGINT itself, or the main thread's read would not see EINTR. */
		sigset_t block, old;
		sigemptyset(&block);
		sigaddset(&block, SIGINT);
		pthread_sigmask(SIG_BLOCK, &block, &old);
		pthread_t thread;
		if (pthread_create(&thread, NULL, interrupt_watcher, NULL) == 0) {
			pthread_detach(thread);
		}
		pthread_sigmask(SIG_SETMASK, &old, NULL);
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: a blocking read returns EINTR so the input line can be discarded. */
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
}

plat_line_status plat_read_line_interactive(const char *prompt, char **out) {
	g_sigint_at_prompt = 0;
#ifdef HAVE_READLINE
	char *line = readline(prompt);
	if (!line) {
		return PLAT_LINE_EOF;
	}
	*out = xstrdup(line);
	free(line);
	return PLAT_LINE_OK;
#else
	fputs(prompt, stdout);
	fflush(stdout);
	plat_line_status st = plat_read_line_stream(stdin, out);
	if (st == PLAT_LINE_INTERRUPTED) {
		fputs("\n", stdout);
	}
	return st;
#endif
}

void plat_history_load(const char *path) {
#ifdef HAVE_READLINE
	if (path) {
		read_history(path);
	}
#else
	(void)path;
#endif
}

void plat_history_add(const char *line) {
#ifdef HAVE_READLINE
	add_history(line);
#else
	(void)line;
#endif
}

void plat_history_save(const char *path) {
#ifdef HAVE_READLINE
	if (path) {
		stifle_history(2000);
		write_history(path);
	}
#else
	(void)path;
#endif
}

FILE *plat_fopen(const char *path, const char *mode) {
	return fopen(path, mode);
}

bool plat_file_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool plat_file_is_shared(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && (st.st_mode & (S_IRGRP | S_IROTH)) != 0;
}

char *plat_home_dir(void) {
	const char *h = getenv("HOME");
	return (h && *h) ? xstrdup(h) : NULL;
}

char *plat_config_dir(void) {
	const char *x = getenv("XDG_CONFIG_HOME");
	if (x && *x) {
		return xstrdup(x);
	}
	char *home = plat_home_dir();
	if (!home) {
		return NULL;
	}
	strbuf sb;
	sb_init(&sb);
	sb_appendf(&sb, "%s/.config", home);
	free(home);
	return sb_detach(&sb);
}

const char *plat_path_sep(void) {
	return "/";
}

double plat_now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif
