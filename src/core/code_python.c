/* The user's own python3 (docs/adr/0222).
 *
 * Code cells and the tool-extension host run on the interpreter the user
 * would run themselves, so they see the user's environment, PYTHON* settings,
 * site-packages and virtual environment. tny adds no restriction layer
 * (docs/adr/0180): sandboxing is the user's choice of where to run tny.
 *
 * Finding it: TNY_PYTHON (a path, a command on PATH, or "off"), else the
 * first python3 on PATH. A candidate counts only after it runs and reports at
 * least TNY_PYTHON_MIN_MAJOR.TNY_PYTHON_MIN_MINOR; without one, run_code is
 * not offered and the native tools are model-facing instead. */
#include "core/code_python.h"
#include "util/process.h"
#include "util/util.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Bounded: a broken interpreter must not stall a turn. */
#define PROBE_TIMEOUT_MS 5000

const char TNY_PYTHON_CELL_LOADER[] =
    "import os\n"
    "os.set_blocking(3, True)\n"
    "def _tny_read(n):\n"
    "    data = b''\n"
    "    while len(data) < n:\n"
    "        chunk = os.read(3, n - len(data))\n"
    "        if not chunk:\n"
    "            os._exit(2)\n"
    "        data += chunk\n"
    "    return data\n"
    "exec(compile(_tny_read(int.from_bytes(_tny_read(4), 'big')), '<tny-cell>', 'exec'))\n";

static bool executable_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* A command name resolves like a shell would, but only through absolute PATH
 * entries; the file is not canonicalized, so a venv's bin/python3 symlink
 * keeps its venv. */
static char *lookup(const char *command) {
    if (!command || !*command) return NULL;
    if (strchr(command, '/')) {
        if (command[0] != '/') return NULL;
        return executable_file(command) ? xstrdup(command) : NULL;
    }
    const char *path = getenv("PATH");
    for (const char *p = path; p && *p;) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n && *p == '/') {
            char *dir = xstrndup(p, n);
            char *candidate = dir ? path_join(dir, command) : NULL;
            free(dir);
            if (executable_file(candidate)) return candidate;
            free(candidate);
        }
        p = end ? end + 1 : NULL;
    }
    return NULL;
}

/* stdin, stdout and stderr on /dev/null (unmapped standard descriptors are). */
static int spawn_quiet(char *const argv[], pid_t *pid) {
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null < 0) return errno;
    const tny_fd_mapping maps[] = {{null, 0}};
    int rc = tny_process_spawn_mapped(argv, environ, maps, 1, pid);
    close(null);
    return rc;
}

#ifdef __APPLE__
/* /usr/bin/python3 is a shim that opens an installer dialog when the Command
 * Line Tools are missing; ask xcode-select first, which never prompts. */
static bool apple_shim_usable(const char *path) {
    if (strcmp(path, "/usr/bin/python3") != 0) return true;
    char *argv[] = {(char *)"/usr/bin/xcode-select", (char *)"-p", NULL};
    pid_t pid = -1;
    if (spawn_quiet(argv, &pid)) return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

/* Runs the candidate exactly as a cell would start it (no -I: the user's
 * own configuration applies) and checks its version. */
static bool usable(const char *path) {
#ifdef __APPLE__
    if (!apple_shim_usable(path)) return false;
#endif
    char check[96];
    snprintf(check, sizeof check, "import sys; sys.exit(0 if sys.version_info >= (%d, %d) else 3)",
             TNY_PYTHON_MIN_MAJOR, TNY_PYTHON_MIN_MINOR);
    char *argv[] = {(char *)path, (char *)"-c", check, NULL};
    pid_t pid = -1;
    if (spawn_quiet(argv, &pid)) return false;
    int64_t deadline = monotonic_ms() + PROBE_TIMEOUT_MS;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) return false;
        if (monotonic_ms() >= deadline) {
            (void)tny_process_kill_tree(pid);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        usleep(2000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool disabled(const char *value) {
    return strcmp(value, "off") == 0 || strcmp(value, "0") == 0 || strcmp(value, "none") == 0 ||
           strcmp(value, "false") == 0;
}

static struct {
    pthread_mutex_t lock;
    char *key; /* TNY_PYTHON and PATH the answer was computed for */
    char *path;
} found = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL};

const char *tny_python_path(void) {
    const char *choice = getenv("TNY_PYTHON");
    const char *path_env = getenv("PATH");
    buf_t k;
    buf_init(&k);
    buf_appendf(&k, "%c%s\n%s", choice ? '1' : '0', choice ? choice : "", path_env ? path_env : "");
    char *key = buf_detach(&k);
    if (!key) return NULL;
    pthread_mutex_lock(&found.lock);
    if (!found.key || strcmp(found.key, key) != 0) {
        free(found.path);
        found.path = NULL;
        if (!(choice && *choice && disabled(choice))) {
            char *candidate = lookup(choice && *choice ? choice : "python3");
            if (candidate && usable(candidate)) found.path = candidate;
            else free(candidate);
        }
        free(found.key);
        found.key = key;
        key = NULL;
    }
    const char *path = found.path;
    pthread_mutex_unlock(&found.lock);
    free(key);
    return path;
}

bool tny_code_python_build(void) { return true; }

bool tny_code_python_available(void) {
    return tny_process_tree_supported() && tny_python_path() != NULL;
}
