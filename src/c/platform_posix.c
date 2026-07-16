#include "water.h"
#include "isocline.h"
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>

void *platform_reserve(size_t requested, size_t *reserved_out) {
	size_t reservation = requested ? requested : ARENA_RESERVE;
	void *base = mmap(NULL, reservation, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	*reserved_out = reservation;
	return base;
}

void platform_init(void) {
	signal(SIGPIPE, SIG_IGN);
}

#ifdef __GLIBC__
typedef struct {
	void *thunk;
	int (*cmp)(void *, const void *, const void *);
} QsortContext;

static int qsort_context_cmp(const void *left, const void *right, void *context) {
	QsortContext *adapter = context;
	return adapter->cmp(adapter->thunk, left, right);
}

void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *)) {
	QsortContext adapter = { .thunk = thunk, .cmp = cmp };
	qsort_r(base, n, size, qsort_context_cmp, &adapter);
}
#else
void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *)) {
	qsort_r(base, n, size, thunk, cmp);
}
#endif

static Interpreter *repl_interp;

#include "repl_highlight_groups.h"

static int lf_token_in(const char *const *set, const char *tok, long len) {
	for (int i = 0; set[i]; i++)
		if ((long)strlen(set[i]) == len && memcmp(set[i], tok, (size_t)len) == 0)
			return 1;
	return 0;
}

static int lf_is_number(const char *s, long len) {
	long i = 0;
	if (i < len && (s[i] == '-' || s[i] == '+'))
		i++;
	long digits = 0;
	while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
	}
	if (digits == 0)
		return 0;
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < len && (s[i] == '-' || s[i] == '+'))
			i++;
		long exp_digits = 0;
		while (i < len && s[i] >= '0' && s[i] <= '9') { i++; exp_digits++; }
		if (exp_digits == 0)
			return 0;
	}
	return i == len;
}

static const char *lf_token_style(const char *s, long len) {
	if (lf_is_number(s, len))
		return "ansi-teal";
	if (len == 2 && (memcmp(s, "[:", 2) == 0 || memcmp(s, ":]", 2) == 0
			|| memcmp(s, "[(", 2) == 0 || memcmp(s, ")]", 2) == 0
			|| memcmp(s, "[|", 2) == 0 || memcmp(s, "[>", 2) == 0))
		return "ansi-blue";
	if (s[0] == ':' && len > 1)
		return "ansi-olive";
	if (s[0] == '/' && len > 1 && ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z')))
		return "ansi-olive";
	if (s[0] >= 'A' && s[0] <= 'Z')
		return "ansi-purple";
	if (lf_token_in(lf_control, s, len))
		return "ansi-maroon";
	if (lf_token_in(lf_defining, s, len))
		return "ansi-blue";
	if (lf_token_in(lf_logicwords, s, len))
		return "ansi-fuchsia";
	if (len > 0 && len < 128) {
		char buf[128];
		memcpy(buf, s, (size_t)len);
		buf[len] = 0;
		if (find(buf))
			return "ansi-navy";
	}
	return NULL;
}

static int lf_is_ws(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static long lf_token_end(const char *input, long n, long start) {
	char lead = input[start];
	char after_lead = start + 1 < n ? input[start + 1] : 0;
	if (lead == ';' || lead == ']' || lead == '}')
		return start + 1;
	if ((lead == ':' || lead == ')') && after_lead == ']')
		return start + 2;
	if (lead == '[') {
		int two_char_opener = after_lead == ':' || after_lead == '('
			|| after_lead == '|' || after_lead == '>';
		return start + (two_char_opener ? 2 : 1);
	}
	if (lead == '{')
		return start + 1;

	long i = start;
	int bracket_depth = 0;
	int brace_depth = 0;
	while (i < n) {
		char c = input[i];
		if (lf_is_ws(c) || c == ';')
			break;
		if (c == ']' && bracket_depth == 0) {
			char preceding = input[i - 1];
			if ((preceding == ':' || preceding == ')') && i - 1 > start)
				i--;
			break;
		}
		if (c == '}' && brace_depth == 0)
			break;
		if (c == '[')
			bracket_depth++;
		if (c == ']')
			bracket_depth--;
		if (c == '{')
			brace_depth++;
		if (c == '}')
			brace_depth--;
		i++;
	}
	return i;
}

static void repl_highlighter(ic_highlight_env_t *henv, const char *input, void *arg) {
	(void)arg;
	long n = (long)strlen(input);
	long i = 0;
	while (i < n) {
		if (lf_is_ws(input[i])) { i++; continue; }
		long start = i;
		if (input[i] == '\\' && (i + 1 >= n || lf_is_ws(input[i + 1]))) {
			while (i < n && input[i] != '\n') i++;
			ic_highlight(henv, start, i - start, "ansi-silver");
			continue;
		}
		if (input[i] == '(' && i + 1 < n && lf_is_ws(input[i + 1])) {
			while (i < n && input[i] != ')') i++;
			if (i < n) i++;
			ic_highlight(henv, start, i - start, "ansi-silver");
			continue;
		}
		if (input[i] == '"') {
			i++;
			while (i < n) {
				if (input[i] == '"') {
					if (i + 1 < n && input[i + 1] == '"') { i += 2; continue; }
					i++;
					break;
				}
				i++;
			}
			ic_highlight(henv, start, i - start, "ansi-green");
			continue;
		}
		i = lf_token_end(input, n, i);
		const char *style = lf_token_style(input + start, i - start);
		if (style)
			ic_highlight(henv, start, i - start, style);
	}
}

static void repl_complete_word(ic_completion_env_t *cenv, const char *word) {
	size_t word_len = strlen(word);
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		const char *name = &vocab.name_pool[WORD_NAME(cfa)];
		if (strncmp(name, word, word_len) == 0)
			if (!ic_add_completion(cenv, name))
				return;
	}
}

static void repl_completer(ic_completion_env_t *cenv, const char *prefix) {
	ic_complete_word(cenv, prefix, repl_complete_word, &ic_char_is_nonwhite);
	ic_complete_filename(cenv, prefix, '/', NULL, NULL);
}

int platform_repl_begin(struct Interpreter *interp, int want_interactive) {
	if (want_interactive) {
		printf("water %s\n", VERSION);
		ic_set_history(".water_history", -1);
		repl_interp = interp;
		ic_set_default_completer(repl_completer, NULL);
		ic_set_default_highlighter(repl_highlighter, NULL);
		ic_enable_brace_matching(true);
		ic_set_matching_braces("[]{}");
		ic_enable_brace_insertion(false);
		ic_set_prompt_marker(NULL, "..");
		ic_enable_multiline_indent(true);
	}
	return want_interactive;
}

int platform_read_chunk(char *dst, int dst_avail, int interactive) {
	if (interactive) {
		char *entered = ic_readline("");
		if (!entered)
			return 0;
		int len = (int)strlen(entered);
		if (len + 1 >= dst_avail) {
			ic_free(entered);
			return -1;
		}
		memcpy(dst, entered, (size_t)len);
		dst[len] = '\n';
		dst[len + 1] = '\0';
		ic_free(entered);
		return len + 1;
	}
	if (!fgets(dst, dst_avail, stdin))
		return 0;
	return (int)strlen(dst);
}

static char *resolve_program_path(const char *name) {
	if (strchr(name, '/')) {
		size_t length = strlen(name);
		char *copy = malloc(length + 1);
		memcpy(copy, name, length + 1);
		return copy;
	}

	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/bin:/bin";

	size_t name_len = strlen(name);
	for (const char *segment = path; ; ) {
		const char *colon = strchr(segment, ':');
		const char *dir = segment;
		size_t dir_len = colon ? (size_t)(colon - segment) : strlen(segment);
		if (dir_len == 0) {
			dir = ".";
			dir_len = 1;
		}

		char *candidate = malloc(dir_len + 1 + name_len + 1);
		memcpy(candidate, dir, dir_len);
		candidate[dir_len] = '/';
		memcpy(candidate + dir_len + 1, name, name_len + 1);

		if (access(candidate, X_OK) == 0)
			return candidate;
		free(candidate);

		if (!colon)
			return NULL;
		segment = colon + 1;
	}
}

void p_start_process(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val argv_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(argv_val, T_ARRAY, "start-process", "an array");
	Object *argv_array = OBJECT_AT(VAL_DATA(argv_val));
	int argc = argv_array->len;
	if (argc < 1) {
		fail(interp, "argv needs at least the program name");
		return;
	}

	char **argv = malloc(sizeof(char *) * (size_t)(argc + 1));
	for (int i = 0; i < argc; i++) {
		if (VAL_TAG(argv_array->items[i]) != T_STRING) {
			free(argv);
			fail(interp, "argv element %d is %s, expected a string",
					i, tag_name(VAL_TAG(argv_array->items[i])));
			return;
		}
		argv[i] = OBJECT_AT(VAL_DATA(argv_array->items[i]))->bytes;
	}
	argv[argc] = NULL;

	char *program_path = resolve_program_path(argv[0]);
	if (!program_path) {
		const char *name = argv[0];
		free(argv);
		fail(interp, "%s: command not found", name);
		return;
	}

	int in_pipe[2];
	int out_pipe[2];
	int err_pipe[2];
	if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
		free(argv);
		free(program_path);
		fail(interp, "pipe failed");
		return;
	}

	int pipe_fds[6] = { in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1] };
	for (int i = 0; i < 6; i++)
		fcntl(pipe_fds[i], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();
	if (pid < 0) {
		free(argv);
		free(program_path);
		fail(interp, "fork failed");
		return;
	}

	if (pid == 0) {
		dup2(in_pipe[0], 0);
		dup2(out_pipe[1], 1);
		dup2(err_pipe[1], 2);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		execv(program_path, argv);
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	close(err_pipe[1]);
	free(argv);
	free(program_path);

	NEW_FRAME(proc_handle, proc);
	frame_put(proc, intern_symbol(interp, "pid"), make_float((double)pid));
	frame_put(proc, intern_symbol(interp, "in"), make_stream(in_pipe[1]));
	frame_put(proc, intern_symbol(interp, "out"), make_stream(out_pipe[0]));
	frame_put(proc, intern_symbol(interp, "err"), make_stream(err_pipe[0]));

	chain_sp[-1] = make_frame(proc_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_wait(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val pid_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(pid_val, T_FLOAT, "wait", "a float pid");
	int pid = (int)VAL_NUMBER(pid_val);
	if (pid <= 0) {
		fail(interp, "invalid pid %d (expected a spawned process id)", pid);
		return;
	}

	int status;
	pid_t result;
	do {
		result = waitpid((pid_t)pid, &status, 0);
	} while (result < 0 && errno == EINTR);

	if (result < 0) {
		fail(interp, "%s", strerror(errno));
		return;
	}

	int code;
	if (WIFEXITED(status))
		code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		code = 128 + WTERMSIG(status);
	else
		code = -1;
	chain_sp[-1] = make_float((double)code);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_stop_process(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val pid_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(pid_val, T_FLOAT, "stop", "a float pid");
	int pid = (int)VAL_NUMBER(pid_val);
	if (pid <= 0) {
		fail(interp, "invalid pid %d (expected a spawned process id)", pid);
		return;
	}

	kill((pid_t)pid, SIGKILL);

	int status;
	pid_t result;
	do {
		result = waitpid((pid_t)pid, &status, 0);
	} while (result < 0 && errno == EINTR);

	if (result < 0) {
		fail(interp, "%s", strerror(errno));
		return;
	}

	int code;
	if (WIFEXITED(status))
		code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		code = 128 + WTERMSIG(status);
	else
		code = -1;
	chain_sp[-1] = make_float((double)code);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_running(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val pid_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(pid_val, T_FLOAT, "running?", "a float pid");
	int pid = (int)VAL_NUMBER(pid_val);

	siginfo_t info;
	info.si_pid = 0;
	int result;
	do {
		result = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
	} while (result < 0 && errno == EINTR);

	chain_sp[-1] = make_bool(result == 0 && info.si_pid == 0);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
