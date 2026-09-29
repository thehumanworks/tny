#include "greatest.h"
#include "core/code_policy.h"
#include "core/code_runtime.h"
#include "util/process.h"
#include "util/util.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* Every cell below runs through the production path: a fresh `--code-cell`
 * child of this test binary with host authority (docs/adr/0180), not an
 * in-process shortcut. Host effects use private temporary directories,
 * loopback sockets and benign subprocesses only. */

/* Whether this host can stop a cell's process tree. The nominal seam probe
 * does not certify every syscall an instrumentor exposes: Valgrind offers
 * pidfd_open but cannot forward pidfd_send_signal (see tests/test_mcp.c).
 * Probe the exact operation with signal zero. */
static bool process_tree_signals(void) {
    bool supported = tny_process_tree_supported();
#ifdef __linux__
    int probe = (int)syscall(SYS_pidfd_open, getpid(), 0);
    supported = supported && probe >= 0 && syscall(SYS_pidfd_send_signal, probe, 0, NULL, 0) == 0;
    if (probe >= 0) close(probe);
#endif
    return supported;
}

#ifdef __linux__
static pid_t proc_parent(pid_t pid) {
    char path[64], data[512];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    bool ok = fgets(data, sizeof data, file) != NULL;
    fclose(file);
    char *end = ok ? strrchr(data, ')') : NULL;
    long parent = -1;
    if (!end || sscanf(end + 1, " %*c %ld", &parent) != 1) return -1;
    return (pid_t)parent;
}

static bool proc_is_code_cell(pid_t pid) {
    char path[64], argv[256] = {0};
    snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    size_t n = fread(argv, 1, sizeof argv - 1, file);
    fclose(file);
    size_t first = strnlen(argv, n);
    return first + 1 < n && strcmp(argv + first + 1, "--code-cell") == 0;
}

/* SIGKILL every descendant of `root`, deepest first, by /proc ancestry. */
static void kill_descendants(pid_t root) {
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        pid_t pid = (pid_t)atol(entry->d_name);
        if (pid <= 1 || proc_parent(pid) != root) continue;
        kill_descendants(pid);
        kill(pid, SIGKILL);
    }
    closedir(dir);
}
#endif

/* Test-only cleanup where cells cannot be stopped (Valgrind): the production
 * path correctly refuses raw-PID signals, which strands a timed-out cell as an
 * unreaped child of this binary. Left running, such cells spin or sleep and
 * starve every later fork/wait suite. They are direct unreaped children, so
 * their PIDs cannot be reused before this reaps them. */
static void reap_stranded_cells(void *userdata) {
    (void)userdata;
#ifdef __linux__
    if (process_tree_signals()) return;
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        pid_t pid = (pid_t)atol(entry->d_name);
        if (pid <= 1 || proc_parent(pid) != getpid() || !proc_is_code_cell(pid)) continue;
        kill_descendants(pid);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    closedir(dir);
#endif
}

static char *fake_tool(void *userdata, const char *name, const char *args) {
    unsigned *count = userdata;
    ++*count;
    if (!strcmp(name, "denied")) return xstrdup("error: permission denied");
    if (!strcmp(name, "fail")) return NULL;
    if (!strcmp(name, "huge")) {
        char *big = malloc(TNY_CODE_TOOL_RESULT_BYTES + 2);
        if (!big) return NULL;
        memset(big, 'x', TNY_CODE_TOOL_RESULT_BYTES + 1);
        big[TNY_CODE_TOOL_RESULT_BYTES + 1] = 0;
        return big;
    }
    return xstrdup(args);
}

TEST code_composes_calls_and_json(void) {
    unsigned calls = 0;
    char *out = tny_code_run("def twice(n):\n    return n * 2\n"
                             "total = 0\n"
                             "for i in range(1, 4):\n"
                             "    reply = tools.call('echo', json.dumps({'n': twice(i)}))\n"
                             "    total += json.loads(reply)['n']\n"
                             "print(total)\n",
                             5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("12\n", out);
    ASSERT_EQ(3u, calls);
    free(out);
    PASS();
}

TEST code_state_is_fresh_and_builtins_present(void) {
    /* The ordinary builtins module, import system and __main__ namespace. */
    const char *script = "import builtins\n"
                         "present = [n for n in ('__import__', 'open', 'eval', 'exec', 'compile',\n"
                         "           'input', 'breakpoint', 'print') if n in vars(builtins)]\n"
                         "import os, subprocess, socket, pathlib\n"
                         "try:\n"
                         "    saved\n"
                         "    fresh = False\n"
                         "except NameError:\n"
                         "    fresh = True\n"
                         "saved = 42\n"
                         "print(len(present), __name__, fresh, eval('6 * 7'))\n";
    for (int i = 0; i < 2; ++i) {
        char *out = tny_code_run(script, 5000, NULL, NULL, NULL);
        ASSERT(out);
        ASSERT_STR_EQ("8 __main__ True 42\n", out);
        free(out);
    }
    PASS();
}

TEST code_limits_are_enforced(void) {
    const char *scripts[] = {
        "while True:\n    pass\n",
        "data = []\nwhile True:\n    data.append('x' * 1000000)\n",
        "for _ in range(65):\n    tools.call('echo', '{}')\n",
        "tools.call('run_code', '{}')\n",
        "tools.call('echo', '[]')\n",
        "tools.call('echo', {'path': 'x'})\n",
        "t = []\nt.append(t)\njson.dumps(t)\n",
        "raise ValueError('boom')\n",
        "import sys\nsys.exit(3)\n",
        "local x = 1\n",
        "tools.call('fail', '{}')\n",
        "tools.call('huge', '{}')\n",
    };
    for (size_t i = 0; i < sizeof(scripts) / sizeof(*scripts); ++i) {
        unsigned calls = 0;
        char *out = tny_code_run(scripts[i], 1000, NULL, fake_tool, &calls);
        ASSERT(out);
        if (!str_starts(out, "error: code: ")) FAILm(scripts[i]);
        ASSERT(strlen(out) <= TNY_CODE_RESULT_TEXT_BYTES);
        ASSERT(calls <= TNY_CODE_TOOL_CALLS);
        free(out);
    }
    PASS();
}

/* A caught limit never re-enables work: no further nested call happens and
 * the cell still ends in the limit error. */
TEST code_limits_are_sticky(void) {
    struct {
        const char *script, *reason;
        unsigned max_calls;
    } cases[] = {
        {"try:\n    data = []\n    while True:\n        data.append('x' * 1000000)\n"
         "except BaseException:\n    data = None\n"
         "tools.call('echo', '{}')\n",
         "memory limit exceeded", 0},
        {"for _ in range(64):\n    tools.call('echo', '{}')\n"
         "try:\n    tools.call('echo', '{}')\nexcept BaseException:\n    pass\n"
         "print('continued')\n",
         "tool call limit exceeded", 64},
        {"try:\n    tools.call('fail', '{}')\nexcept BaseException:\n    pass\n"
         "tools.call('echo', '{}')\n",
         "tool callback failed", 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        unsigned calls = 0;
        char *out = tny_code_run(cases[i].script, 5000, NULL, fake_tool, &calls);
        ASSERT(out);
        if (!str_starts(out, "error: code: ") || !strstr(out, cases[i].reason)) FAILm(out);
        ASSERT(!strstr(out, "continued"));
        ASSERT(calls <= cases[i].max_calls);
        free(out);
    }
    PASS();
}

/* Finalizers run while the cell is cleared or after it; tools are closed. */
TEST code_finalizers_cannot_reach_tools(void) {
    unsigned calls = 0;
    char *out = tny_code_run("class Late:\n"
                             "    def __del__(self):\n"
                             "        tools.call('echo', '{\"late\": true}')\n"
                             "keep = Late()\n"
                             "print('body')\n",
                             5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("body\n", out);
    ASSERT_EQ(0u, calls);
    free(out);
    PASS();
}

static char *temp_dir(void) {
    char *dir = xstrdup("/var/tmp/tny-code-cell-XXXXXX");
    if (dir && !mkdtemp(dir)) {
        free(dir);
        return NULL;
    }
    return dir;
}

/* A Python string literal for a temporary path (one use per statement). */
static const char *json_quote_python(const char *text) {
    static char quoted[PATH_MAX * 2 + 3];
    size_t n = 0;
    quoted[n++] = '\'';
    for (; *text && n < sizeof quoted - 3; ++text) {
        if (*text == '\\' || *text == '\'') quoted[n++] = '\\';
        quoted[n++] = *text;
    }
    quoted[n++] = '\'';
    quoted[n] = 0;
    return quoted;
}

static void remove_tree(const char *dir) {
    char command[PATH_MAX + 16];
    snprintf(command, sizeof command, "rm -rf '%s'", dir);
    if (system(command) != 0) fprintf(stderr, "could not remove %s\n", dir);
}

/* Direct Python acts on the host as the OS user: files, subprocesses and the
 * inherited environment, in the requested working directory. */
TEST code_cell_acts_on_the_host(void) {
    char *dir = temp_dir();
    ASSERT(dir);
    char *real = realpath(dir, NULL);
    ASSERT(real);
    ASSERT_EQ(0, setenv("TNY_CELL_TEST_VISIBLE", "inherited-value", 1));
    ASSERT_EQ(0, setenv("TNY_JOB_SCOPE_TEST", "private", 1));
    char code[2048];
    snprintf(code, sizeof code,
             "import os, pathlib, subprocess, sys\n"
             "assert os.getcwd() == %s, os.getcwd()\n"
             "pwd = os.environ['PWD']\n"
             "assert os.path.isabs(pwd) and os.path.samefile(pwd, '.'), pwd\n"
             "pathlib.Path('made.txt').write_text('from python')\n"
             "with open('made.txt', 'a') as f:\n"
             "    print(' and print', file=f, end='')\n"
             "r = subprocess.run(['sh', '-c', 'cat made.txt; echo; exit 7'],\n"
             "                   capture_output=True, text=True)\n"
             "print(r.returncode, r.stdout.strip())\n"
             "print(os.environ.get('TNY_CELL_TEST_VISIBLE'), 'TNY_JOB_SCOPE_TEST' in os.environ)\n"
             "print(repr(sys.executable), 'PATH' in os.environ)\n",
             json_quote_python(real));
    int64_t deadline = monotonic_ms() + 10000;
    char *out = tny_code_run_with_deadline(code, dir, &deadline, NULL, NULL, NULL);
    unsetenv("TNY_CELL_TEST_VISIBLE");
    unsetenv("TNY_JOB_SCOPE_TEST");
    ASSERT(out);
    ASSERT_STR_EQ("7 from python and print\ninherited-value False\n'' True\n", out);
    char made[PATH_MAX];
    snprintf(made, sizeof made, "%s/made.txt", dir);
    char *text = file_slurp(made, NULL);
    ASSERT(text);
    ASSERT_STR_EQ("from python and print", text);
    free(text);
    free(out);
    remove_tree(dir);
    free(real);
    free(dir);
    PASS();
}

#ifdef __linux__
/* The value of a "Name:\tvalue" line in /proc/self/status, or -1. */
static long proc_status_field(const char *name) {
    char *status = file_slurp("/proc/self/status", NULL);
    if (!status) return -1;
    long value = -1;
    size_t len = strlen(name);
    for (const char *line = status; line && *line; line = strchr(line, '\n')) {
        if (*line == '\n') line++;
        if (strncmp(line, name, len) == 0 && line[len] == ':') {
            value = strtol(line + len + 1, NULL, 10);
            break;
        }
    }
    free(status);
    return value;
}
#endif

/* No kernel confinement or resource denial: tny adds no seccomp filter, no
 * no_new_privs bit and no lower limits than the parent's. The comparison is to
 * the parent, not to absolute values: containers (Docker's default seccomp
 * profile) confine both, and valgrind lowers only its own client's NOFILE. */
TEST code_cell_has_no_confinement(void) {
#ifdef __linux__
    struct rlimit files, procs;
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &files));
    ASSERT_EQ(0, getrlimit(RLIMIT_NPROC, &procs));
    long seccomp = proc_status_field("Seccomp");
    long no_new_privs = proc_status_field("NoNewPrivs");
    ASSERT(seccomp >= 0 && no_new_privs >= 0);
    char code[1024];
    snprintf(code, sizeof code,
             "import resource\n"
             "status = open('/proc/self/status').read()\n"
             "print('Seccomp:\\t%ld\\n' in status, 'NoNewPrivs:\\t%ld\\n' in status)\n"
             "soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)\n"
             "print(soft >= %lld and hard >= %lld,\n"
             "      resource.getrlimit(resource.RLIMIT_NPROC) == (%lld, %lld))\n"
             "print(resource.getrlimit(resource.RLIMIT_FSIZE)[0] == resource.RLIM_INFINITY)\n",
             seccomp, no_new_privs, (long long)files.rlim_cur, (long long)files.rlim_max,
             (long long)procs.rlim_cur, (long long)procs.rlim_max);
    char *out = tny_code_run(code, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("True True\nTrue True\nTrue\n", out);
    free(out);
#endif
    PASS();
}

/* A loopback HTTP server and client inside one cell: sockets, threads and
 * urllib all work. */
TEST code_cell_uses_loopback_http(void) {
    const char *code = "import http.server, threading, urllib.request, socket\n"
                       "class H(http.server.BaseHTTPRequestHandler):\n"
                       "    def do_GET(self):\n"
                       "        body = ('path=' + self.path).encode()\n"
                       "        self.send_response(200)\n"
                       "        self.send_header('Content-Length', str(len(body)))\n"
                       "        self.end_headers()\n"
                       "        self.wfile.write(body)\n"
                       "    def log_message(self, *args):\n"
                       "        pass\n"
                       "server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)\n"
                       "threading.Thread(target=server.serve_forever, daemon=True).start()\n"
                       "url = 'http://127.0.0.1:%d/probe' % server.server_address[1]\n"
                       "with urllib.request.urlopen(url, timeout=5) as r:\n"
                       "    print(r.status, r.read().decode())\n"
                       "server.shutdown()\n";
    char *out = tny_code_run(code, 10000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("200 path=/probe\n", out);
    free(out);
    PASS();
}

/* stdout, stderr and inherited subprocess output form one ordered result. */
TEST code_captures_ordered_process_output(void) {
    const char *code = "import os, subprocess, sys\n"
                       "print('one')\n"
                       "sys.stderr.write('two\\n')\n"
                       "subprocess.run(['sh', '-c', 'echo three; echo four >&2'])\n"
                       "os.write(1, b'five\\n')\n"
                       "print('six', flush=True)\n";
    char *out = tny_code_run(code, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("one\ntwo\nthree\nfour\nfive\nsix\n", out);
    free(out);
    PASS();
}

/* Excess output is bounded without ending the cell: the beginning and the
 * end stay, the middle is summarized, and later code still runs. NUL and
 * malformed UTF-8 become U+FFFD so the JSON result stays valid. */
TEST code_output_is_bounded_not_terminal(void) {
    char *out = tny_code_run("import subprocess\n"
                             "print('BEGIN' + 'a' * 100000)\n"
                             "subprocess.run(['sh', '-c', 'yes b | head -c 200000'])\n"
                             "print('END')\n",
                             10000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT(strlen(out) <= TNY_CODE_OUTPUT_BYTES);
    ASSERT(str_starts(out, "BEGINaaa"));
    ASSERT(strstr(out, "bytes of output omitted]"));
    size_t len = strlen(out);
    ASSERT(len > 4 && strcmp(out + len - 4, "END\n") == 0);
    free(out);
    out = tny_code_run("import os\nos.write(1, b'a\\x00b\\xffc\\xe2\\x82\\n')\n", 5000, NULL, NULL,
                       NULL);
    ASSERT(out);
    ASSERT_STR_EQ("a\xef\xbf\xbd"
                  "b\xef\xbf\xbd"
                  "c\xef\xbf\xbd\xef\xbf\xbd\n",
                  out);
    free(out);
    PASS();
}

/* Errors put the summary line first, then the output with the traceback;
 * SystemExit behaves like the interpreter's. */
TEST code_errors_report_traceback_and_exit_status(void) {
    char *out = tny_code_run("print('before')\n"
                             "def inner():\n"
                             "    raise ValueError('boom')\n"
                             "inner()\n",
                             5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT(str_starts(out, "error: code: ValueError: boom (line 3)\n[output before the error]\n"
                           "before\nTraceback"));
    ASSERT(strstr(out, "raise ValueError('boom')"));
    free(out);
    out = tny_code_run("import sys\nprint('done')\nsys.exit(0)\nprint('not reached')\n", 5000, NULL,
                       NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("done\n", out);
    free(out);
    out = tny_code_run("raise SystemExit('stopped here')\n", 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("error: code: SystemExit: stopped here", out);
    free(out);
    PASS();
}

/* The deadline stops the cell and its ordinary descendants; output written
 * before the deadline is kept. */
TEST code_deadline_stops_owned_descendants(void) {
    bool supported = process_tree_signals();
    if (getenv("TNY_TEST_REQUIRE_PROCESS_TREE")) ASSERT(supported);
    char *dir = temp_dir();
    ASSERT(dir);
    char pidfile[PATH_MAX], code[1024];
    snprintf(pidfile, sizeof pidfile, "%s/pid", dir);
    snprintf(code, sizeof code,
             "import subprocess, time\n"
             "child = subprocess.Popen(['sleep', '30'])\n"
             "open(%s, 'w').write(str(child.pid))\n"
             "print('started', flush=True)\n"
             "child.wait()\n",
             json_quote_python(pidfile));
    int64_t started = monotonic_ms();
    char *out = tny_code_run(code, 1500, NULL, NULL, NULL);
    int64_t elapsed = monotonic_ms() - started;
    ASSERT(out);
    ASSERT(str_starts(out, "error: code: deadline exceeded"));
    ASSERT(strstr(out, "started\n"));
    char *text = file_slurp(pidfile, NULL);
    ASSERT(text);
    pid_t pid = (pid_t)atoi(text);
    ASSERT(pid > 1);
    errno = 0;
    int alive = kill(pid, 0);
    int alive_error = errno;
    if (supported) {
        ASSERT(elapsed < 5000);
        ASSERT(alive != 0 && alive_error == ESRCH);
    } else {
        /* No signal could be delivered: verify the deadline still ends the
         * call and that nothing substituted an unsafe raw-PID kill. The
         * native CI invocation requires the real capability (ci.yml). */
        ASSERT(elapsed < 12000);
        ASSERT_EQ(0, alive);
        fprintf(stderr, "code deadline: verified unavailable-signal refusal, not tree stop\n");
    }
    free(text);
    free(out);
    remove_tree(dir);
    free(dir);
    PASS();
}

/* A fork() child of the cell neither holds the protocol open nor reaches the
 * tools; the cell finishes without waiting for it. */
TEST code_fork_child_has_no_protocol(void) {
    unsigned calls = 0;
    int64_t started = monotonic_ms();
    char *out = tny_code_run("import os, time\n"
                             "pid = os.fork()\n"
                             "if pid == 0:\n"
                             "    try:\n"
                             "        tools.call('echo', '{}')\n"
                             "    except RuntimeError:\n"
                             "        pass\n"
                             "    time.sleep(3)\n"
                             "    os._exit(0)\n"
                             "print('parent', tools.call('echo', '{\"n\": 1}'))\n",
                             5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("parent {\"n\": 1}\n", out);
    ASSERT_EQ(1u, calls);
    ASSERT(monotonic_ms() - started < 2500);
    free(out);
    PASS();
}

TEST code_preserves_denial_and_catalog(void) {
    unsigned calls = 0;
    const char *catalog = "[{\"type\":\"function\",\"function\":{\"name\":\"denied\"}}]";
    char *out = tny_code_run("assert len(json.loads(tools.list())) == 1\n"
                             "entry = json.loads(tools.describe('denied'))\n"
                             "assert entry['function']['name'] == 'denied'\n"
                             "assert tools.describe('missing') is None\n"
                             "print(tools.call('denied', '{}'))\n",
                             5000, catalog, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("error: permission denied\n", out);
    ASSERT_EQ(1u, calls);
    free(out);
    PASS();
}

/* Expected text generated by CPython 3.14.7 with the stdlib json module. */
TEST code_json_matches_cpython(void) {
    struct {
        const char *code, *expected;
    } cases[] = {
        {"print(json.dumps(json.loads('{\"a\":[1,true,null,\"hi\",{}],\"b\":{},\"c\":[],\"z\":0,"
         "\"f\":false,\"e\":\"\"}')))\n",
         "{\"a\": [1, true, null, \"hi\", {}], \"b\": {}, \"c\": [], \"z\": 0, \"f\": false, "
         "\"e\": \"\"}\n"},
        {"v = json.loads('[9007199254740993, -9223372036854775808, 18446744073709551616, "
         "1e400]')\nprint(v, json.dumps(v[:3]))\n",
         "[9007199254740993, -9223372036854775808, 18446744073709551616, inf] [9007199254740993, "
         "-9223372036854775808, 18446744073709551616]\n"},
        {"print(json.dumps({\"k\": [1, 2], \"o\": {\"p\": None}, \"e\": [], \"d\": {}}, "
         "indent=2))\n",
         "{\n  \"k\": [\n    1,\n    2\n  ],\n  \"o\": {\n    \"p\": null\n  },\n  \"e\": [],\n  "
         "\"d\": {}\n}\n"},
        {"print(json.dumps({\"b\": 1, \"a\": [1.0, 0.1, 1e16, -0.0]}, separators=(\",\", \":\"), "
         "sort_keys=True))\n",
         "{\"a\":[1.0,0.1,1e+16,-0.0],\"b\":1}\n"},
        {"s = \"\\u00e9\\U0001f600\\n\\\"\\\\\\x01\\x7f\"\nprint(json.dumps(s), json.dumps(s, "
         "ensure_ascii=False))\n",
         "\"\\u00e9\\ud83d\\ude00\\n\\\"\\\\\\u0001\\u007f\" \"\xc3"
         "\xa9"
         "\xf0"
         "\x9f"
         "\x98"
         "\x80"
         "\\n\\\"\\\\\\u0001\x7f"
         "\"\n"},
        {"print(json.dumps({1: \"a\", True: \"b\", None: \"c\", 2.5: \"d\"}))\n",
         "{\"1\": \"b\", \"null\": \"c\", \"2.5\": \"d\"}\n"},
        {"print(json.dumps((1, (2, 3))), json.dumps({\"x\": {\"b\": 1, \"a\": 2}}))\n",
         "[1, [2, 3]] {\"x\": {\"b\": 1, \"a\": 2}}\n"},
        {"v = json.loads('{\"n\": null, \"f\": false, \"z\": 0}')\nprint(v[\"n\"] is None, "
         "v[\"f\"] is False, type(v[\"z\"]).__name__, v[\"z\"] is False)\n",
         "True True int False\n"},
        {"try:\n    json.loads(\"[1,]\")\nexcept json.JSONDecodeError as e:\n    print(\"decode\", "
         "isinstance(e, ValueError))\n",
         "decode True\n"},
        {"try:\n    json.dumps({\"s\": {1}})\nexcept TypeError:\n    print(\"type\")\n", "type\n"},
        {"try:\n    json.dumps(float(\"nan\"), allow_nan=False)\nexcept ValueError:\n    "
         "print(\"nan\")\nprint(json.dumps([float(\"inf\")]))\n",
         "nan\n[Infinity]\n"},
        {"print(json.loads(b'{\"a\": \"\\\\u00e9\"}')[\"a\"], json.loads('\"\\\\ud83d\\\\ude00\"') "
         "== \"\\U0001f600\")\n",
         "\xc3"
         "\xa9"
         " True\n"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        char *out = tny_code_run(cases[i].code, 5000, NULL, NULL, NULL);
        ASSERT(out);
        if (strcmp(out, cases[i].expected) != 0) FAILm(out);
        free(out);
    }
    PASS();
}

typedef struct {
    int64_t *deadline;
    bool extend;
} prompt_deadline;

static char *fake_prompt_wait(void *userdata, const char *name, const char *args) {
    (void)name;
    (void)args;
    prompt_deadline *prompt = userdata;
    /* Deterministically model elapsed prompt time, without a timing-sensitive
     * sleep. Restoring the budget is exclusively a trusted callback action. */
    *prompt->deadline = monotonic_ms() - 1;
    if (prompt->extend) *prompt->deadline += 3000;
    return xstrdup("approved");
}

TEST code_observes_trusted_prompt_deadline_updates(void) {
    int64_t deadline = monotonic_ms() + 5000;
    prompt_deadline prompt = {.deadline = &deadline, .extend = true};
    char *out = tny_code_run_with_deadline("print(tools.call('prompt', '{}'))\n"
                                           "n = 0\nfor i in range(2000):\n    n += 1\nprint(n)\n",
                                           NULL, &deadline, NULL, fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT_STR_EQ("approved\n2000\n", out);
    free(out);
    prompt.extend = false;
    deadline = monotonic_ms() + 5000;
    out = tny_code_run_with_deadline("print(tools.call('prompt', '{}'))\n", NULL, &deadline, NULL,
                                     fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT(strstr(out, "deadline exceeded"));
    free(out);
    PASS();
}

/* Boundaries of the source-linked gates (also proved in Lean). */
TEST code_policy_boundaries(void) {
    ASSERT(!tny_code_timeout_admit(0));
    ASSERT(tny_code_timeout_admit(1));
    ASSERT(tny_code_timeout_admit(TNY_CODE_MAX_TIMEOUT_MS));
    ASSERT(!tny_code_timeout_admit(TNY_CODE_MAX_TIMEOUT_MS + 1));
    ASSERT(tny_code_source_admit(TNY_CODE_SOURCE_BYTES));
    ASSERT(!tny_code_source_admit(TNY_CODE_SOURCE_BYTES + 1));
    ASSERT(tny_code_call_admit(63, 1, false, TNY_CODE_ARGUMENT_BYTES, true));
    ASSERT(!tny_code_call_admit(64, 1, false, 2, true));
    ASSERT(!tny_code_call_admit(0, 0, false, 2, true));
    ASSERT(!tny_code_call_admit(0, TNY_CODE_NAME_BYTES + 1, false, 2, true));
    ASSERT(!tny_code_call_admit(0, 4, true, 2, true));
    ASSERT(!tny_code_call_admit(0, 4, false, 2, false));
    ASSERT(!tny_code_call_admit(0, 4, false, TNY_CODE_ARGUMENT_BYTES + 1, true));
    ASSERT(tny_code_output_admit(0, TNY_CODE_OUTPUT_BYTES));
    ASSERT(!tny_code_output_admit(1, TNY_CODE_OUTPUT_BYTES));
    ASSERT(!tny_code_output_admit(TNY_CODE_OUTPUT_BYTES + 1, 0));
    ASSERT_EQ(5u, tny_code_output_take(0, 5));
    ASSERT_EQ(1u, tny_code_output_take(TNY_CODE_OUTPUT_BYTES - 1, 5));
    ASSERT_EQ(0u, tny_code_output_take(TNY_CODE_OUTPUT_BYTES, 5));
    ASSERT_EQ(0u, tny_code_output_take(UINT64_MAX, UINT64_MAX));
    ASSERT_EQ(TNY_CODE_OUTPUT_BYTES, tny_code_output_take(0, UINT64_MAX));
    ASSERT(tny_code_memory_admit(0, 10, 16, 26));
    ASSERT(!tny_code_memory_admit(0, 11, 16, 26));
    ASSERT(!tny_code_memory_admit(UINT64_MAX - 1, 1, 16, UINT64_MAX));
    ASSERT(tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_CALL, 3, 63));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_CALL, 3, 64));
    ASSERT(tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_DONE, 1, 64));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_FINISHED, TNY_CODE_FRAME_DONE, 1, 0));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_RESULT, 2, 0));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_DONE,
                                 TNY_CODE_RESULT_TEXT_BYTES + 2, 0));
    PASS();
}

static char *exact_name_tool(void *userdata, const char *name, const char *args) {
    (void)args;
    unsigned *calls = userdata;
    ++*calls;
    return xstrdup(name);
}

TEST code_tool_names_preserve_exact_bytes(void) {
    unsigned calls = 0;
    const char *source = "names = ['echo', 'echo\\n', '\\nleading', 'two\\nlines', 'with:colon', "
                         "'\\u00e9', 'x' * 256]\n"
                         "for name in names:\n"
                         "    assert tools.call(name, '{}') == name\n"
                         "print('exact')\n";
    char *out = tny_code_run(source, 5000, NULL, exact_name_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("exact\n", out);
    ASSERT_EQ(7u, calls);
    free(out);
    PASS();
}

TEST code_json_reentrant_container_lifetimes(void) {
    const char *source = "values = [object(), object()]\n"
                         "def shrink(value):\n"
                         "    values.clear()\n"
                         "    return 7\n"
                         "assert json.dumps(values, default=shrink) == '[7]'\n"
                         "class Broken(dict):\n"
                         "    def items(self):\n"
                         "        return [1]\n"
                         "try:\n"
                         "    json.dumps(Broken(a=1))\n"
                         "except (TypeError, ValueError):\n"
                         "    pass\n"
                         "else:\n"
                         "    raise AssertionError('malformed item was accepted')\n"
                         "pairs = [('a', object()), ('b', 2)]\n"
                         "class Mutable(dict):\n"
                         "    def items(self):\n"
                         "        return pairs\n"
                         "def change(value):\n"
                         "    pairs.clear()\n"
                         "    return 3\n"
                         "assert json.loads(json.dumps(Mutable(a=1), default=change)) == {'a': 3}\n"
                         "print('containers')\n";
    char *out = tny_code_run(source, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("containers\n", out);
    free(out);
    PASS();
}

TEST code_json_error_metadata_and_describe_boundaries(void) {
    const char *source = "text = '[1,]'\n"
                         "try:\n"
                         "    json.loads(text)\n"
                         "except json.JSONDecodeError as error:\n"
                         "    assert error.doc == text\n"
                         "    assert isinstance(error.msg, str)\n"
                         "    assert error.lineno == 1 and error.colno >= 1 and error.pos >= 0\n"
                         "assert json.dumps({'a': 1}, separators=[',', ':']) == '{\"a\":1}'\n"
                         "try:\n"
                         "    tools.describe('echo\\x00other')\n"
                         "except ValueError:\n"
                         "    pass\n"
                         "else:\n"
                         "    raise AssertionError('NUL name silently truncated')\n"
                         "assert tools.describe('echo') is not None\n"
                         "print('metadata')\n";
    char *out = tny_code_run(source, 5000, "[{\"name\":\"echo\"}]", NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("metadata\n", out);
    free(out);
    PASS();
}

TEST code_fatal_quota_does_not_run_finally_or_following_bytecode(void) {
    unsigned calls = 0;
    const char *source = "try:\n"
                         "    data = []\n"
                         "    while True:\n"
                         "        data.append('x' * 1000000)\n"
                         "finally:\n"
                         "    tools.call('echo', '{\"finally\":true}')\n"
                         "tools.call('echo', '{\"after\":true}')\n";
    char *out = tny_code_run(source, 10000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT(strstr(out, "memory limit exceeded"));
    ASSERT_EQ(0u, calls);
    free(out);
    out = tny_code_run("print('fresh')\n", 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("fresh\n", out);
    free(out);
    PASS();
}

TEST code_unicode_parser_is_self_contained(void) {
    const char *source = "\u03c0 = 3\n"
                         "print(\u03c0, '\\N{GREEK SMALL LETTER ALPHA}', '\\N{GRINNING FACE}')\n"
                         "\u212a = 7\n"
                         "assert K == 7\n"
                         "import unicodedata\n"
                         "print(unicodedata.name('\u03b1'), 'x'.encode('cp1252'),\n"
                         "      '\u00e9'.encode('utf-8-sig'), 'b\u00fccher.de'.encode('idna'))\n";
    char *out = tny_code_run(source, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("3 \u03b1 \U0001f600\nGREEK SMALL LETTER ALPHA b'x' b'\\xef\\xbb\\xbf\\xc3\\xa9' "
                  "b'xn--bcher-kva.de'\n",
                  out);
    free(out);
    PASS();
}

SUITE(code_runtime_suite) {
    SET_TEARDOWN(reap_stranded_cells, NULL);
    RUN_TEST(code_unicode_parser_is_self_contained);
    RUN_TEST(code_tool_names_preserve_exact_bytes);
    RUN_TEST(code_json_reentrant_container_lifetimes);
    RUN_TEST(code_json_error_metadata_and_describe_boundaries);
    RUN_TEST(code_fatal_quota_does_not_run_finally_or_following_bytecode);
    RUN_TEST(code_composes_calls_and_json);
    RUN_TEST(code_state_is_fresh_and_builtins_present);
    RUN_TEST(code_limits_are_enforced);
    RUN_TEST(code_limits_are_sticky);
    RUN_TEST(code_finalizers_cannot_reach_tools);
    RUN_TEST(code_cell_acts_on_the_host);
    RUN_TEST(code_cell_has_no_confinement);
    RUN_TEST(code_cell_uses_loopback_http);
    RUN_TEST(code_captures_ordered_process_output);
    RUN_TEST(code_output_is_bounded_not_terminal);
    RUN_TEST(code_errors_report_traceback_and_exit_status);
    RUN_TEST(code_deadline_stops_owned_descendants);
    RUN_TEST(code_fork_child_has_no_protocol);
    RUN_TEST(code_preserves_denial_and_catalog);
    RUN_TEST(code_json_matches_cpython);
    RUN_TEST(code_observes_trusted_prompt_deadline_updates);
    RUN_TEST(code_policy_boundaries);
}
