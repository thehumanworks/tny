#include "core/tasks.h"
#include "util/util.h"

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct {
    const char *name;
    const char *body;
    const char *description;
} BUILTINS[] = {
    {"review",
     "Act as a rigorous code reviewer. Inspect the requested change and directly "
     "relevant repository context before judging it. Prioritize correctness, security, "
     "regressions, concurrency, error handling, API compatibility, portability, tests, "
     "documentation, and maintainability. Be evidence-driven: cite concrete files/lines "
     "or behavior, distinguish defects from preferences, and rank actionable findings by "
     "severity. Do not modify code unless the user explicitly asks you to fix findings. "
     "If there are no material findings, say so and state what you validated.",
     "Rigorous evidence-driven code review"},
    {"optimizer",
     "Act as a performance and complexity optimizer. Preserve externally observable behavior "
     "unless the user explicitly requests a semantic change. Measure or establish evidence before "
     "optimizing when practical. Look for algorithmic complexity, unnecessary allocations/copies, "
     "avoidable I/O or network work, contention, serialization, excessive abstraction, large "
     "dependency/runtime footprint, and hard-to-maintain control flow. Prefer the smallest change "
     "that improves both runtime/resource efficiency and implementation simplicity. Run relevant "
     "tests and benchmarks, report before/after evidence where available, and call out tradeoffs "
     "rather than hiding them.",
     "Performance and complexity optimization"},
    {"document",
     "Act as a documentation expert. Read the implementation, tests, existing documentation, "
     "examples, and public API before writing. Produce accurate, concise, task-oriented "
     "documentation that matches real behavior exactly. Update the most relevant user-facing and "
     "contributor-facing docs, runnable examples, references, and generated documentation when "
     "required. Do not invent flags, APIs, guarantees, or examples that have not been verified. "
     "Check links, commands, terminology, and cross-references, and run available "
     "documentation/site validation before finishing.",
     "Implementation-grounded documentation"},
    {"retro",
     "Perform a retrospective of the work/session. Reconstruct the important decisions, mistakes, "
     "friction, successful techniques, validation gaps, and reusable lessons from the available "
     "conversation context, git diff/history, tests, and repository state. Turn durable lessons "
     "into repository improvements only when justified: optionally update AGENTS.md when a rule "
     "should guide future work in this repository, and optionally create or update a skill when a "
     "repeatable multi-step procedure would materially improve future execution. Avoid recording "
     "transient details, secrets, personal information, or one-off instructions as durable "
     "guidance. Keep any AGENTS.md/skill changes concise, scoped, and evidence-based; test or "
     "validate them where possible. End with concrete follow-ups and unresolved risks.",
     "Retrospective and durable lessons"}};

bool tny_task_name_valid(const char *name) {
    if (!name || !*name || strlen(name) >= TNY_TASK_NAME_MAX) return false;
    if (name[0] == '.' || strstr(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.'))
            return false;
    return true;
}

bool tny_task_source_valid(const char *source) {
    return source && (strcmp(source, "builtin") == 0 || strcmp(source, "project") == 0 ||
                      strcmp(source, "user") == 0 || strcmp(source, "workflow") == 0 ||
                      strcmp(source, "explicit") == 0);
}

const char *tny_task_builtin_body(const char *name) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++)
        if (strcmp(name, BUILTINS[i].name) == 0) return BUILTINS[i].body;
    return NULL;
}

typedef enum { TASK_ABSENT = 0, TASK_VALID, TASK_BROKEN, TASK_NOMEM } task_read_result;

/* Walk only the fixed .tny/tasks suffix with O_NOFOLLOW. The workspace/home
 * root is caller-selected, but neither controlled suffix component may be a
 * symlink into another tree. */
static task_read_result custom_dir_open(const char *root, int *out_fd) {
    *out_fd = -1;
    if (!root || !*root) return TASK_ABSENT;
    int root_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_DIRECTORY
    root_flags |= O_DIRECTORY;
#endif
    int root_fd = open(root, root_flags);
    if (root_fd < 0) return errno == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    int child_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_DIRECTORY
    child_flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    child_flags |= O_NOFOLLOW;
#endif
    int tny_fd = openat(root_fd, ".tny", child_flags);
    int saved = errno;
    close(root_fd);
    if (tny_fd < 0) return saved == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    int tasks_fd = openat(tny_fd, "tasks", child_flags);
    saved = errno;
    close(tny_fd);
    if (tasks_fd < 0) return saved == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    *out_fd = tasks_fd;
    return TASK_VALID;
}

static bool body_nonempty(const char *body, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isspace((unsigned char)body[i])) return true;
    return false;
}

/* Validate and strip the deliberately tiny Markdown frontmatter dialect.
 * The input belongs to the caller and is rewritten in place to contain only
 * the instruction body. */
static task_read_result parse_body(char *raw, size_t n, const char *expected_name,
                                   char **description) {
    if (description) *description = NULL;
    if (!raw || n == 0 || n > TNY_TASK_BODY_MAX || !utf8_valid_bytes(raw, n)) return TASK_BROKEN;
    size_t body_at = 0;
    size_t first = 0;
    if (n >= 4 && memcmp(raw, "---\n", 4) == 0) first = 4;
    else if (n >= 5 && memcmp(raw, "---\r\n", 5) == 0) first = 5;
    if (first) {
        bool saw_name = false, saw_description = false, closed = false;
        size_t at = first;
        while (at < n) {
            size_t end = at;
            while (end < n && raw[end] != '\n') end++;
            size_t len = end - at;
            if (len && raw[at + len - 1] == '\r') len--;
            if (len == 3 && memcmp(raw + at, "---", 3) == 0) {
                body_at = end < n ? end + 1 : end;
                closed = true;
                break;
            }
            const char *line = raw + at;
            const char *colon = memchr(line, ':', len);
            if (!colon || colon == line) return TASK_BROKEN;
            size_t key_len = (size_t)(colon - line);
            const char *value = colon + 1;
            const char *value_end = line + len;
            while (value < value_end && (*value == ' ' || *value == '\t')) value++;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
                value_end--;
            size_t value_len = (size_t)(value_end - value);
            if (key_len == 4 && memcmp(line, "name", 4) == 0) {
                if (saw_name || !value_len || strlen(expected_name) != value_len ||
                    memcmp(value, expected_name, value_len) != 0)
                    return TASK_BROKEN;
                saw_name = true;
            } else if (key_len == 11 && memcmp(line, "description", 11) == 0) {
                if (saw_description || !value_len || value_len > TNY_TASK_DESCRIPTION_MAX)
                    return TASK_BROKEN;
                if (description) {
                    *description = xstrndup(value, value_len);
                    if (!*description) return TASK_NOMEM;
                }
                saw_description = true;
            } else return TASK_BROKEN;
            at = end < n ? end + 1 : end;
        }
        if (!closed) {
            if (description) {
                free(*description);
                *description = NULL;
            }
            return TASK_BROKEN;
        }
        /* Frontmatter separators are not model instructions.  Discard blank
         * lines immediately after the closing delimiter. */
        while (body_at < n) {
            size_t end = body_at;
            while (end < n && raw[end] != '\n') end++;
            size_t len = end - body_at;
            if (len && raw[body_at + len - 1] == '\r') len--;
            bool blank = true;
            for (size_t i = 0; i < len; i++)
                if (raw[body_at + i] != ' ' && raw[body_at + i] != '\t') blank = false;
            if (!blank) break;
            body_at = end < n ? end + 1 : end;
        }
    }
    size_t body_len = n - body_at;
    if (!body_nonempty(raw + body_at, body_len)) {
        if (description) {
            free(*description);
            *description = NULL;
        }
        return TASK_BROKEN;
    }
    if (body_at) memmove(raw, raw + body_at, body_len);
    raw[body_len] = 0;
    return TASK_VALID;
}

static task_read_result read_fd(int fd, const char *name, char **body, char **description) {
    if (body) *body = NULL;
    if (description) *description = NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > TNY_TASK_BODY_MAX) {
        return TASK_BROKEN;
    }
    buf_t contents;
    buf_init(&contents);
    char chunk[8192];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof chunk)) > 0) {
        size_t got_size = (size_t)got;
        if (got_size > TNY_TASK_BODY_MAX - contents.len) {
            buf_free(&contents);
            return TASK_BROKEN;
        }
        buf_append(&contents, chunk, got_size);
    }
    bool oom = buf_oom(&contents);
    bool read_failed = got < 0 || oom;
    if (read_failed || contents.len == 0) {
        buf_free(&contents);
        return oom ? TASK_NOMEM : TASK_BROKEN;
    }
    size_t n = contents.len;
    char *raw = buf_detach(&contents);
    if (!raw) return TASK_NOMEM;
    task_read_result result = parse_body(raw, n, name, description);
    if (result == TASK_VALID && body) *body = raw;
    else free(raw);
    if (result != TASK_VALID && description && *description) {
        free(*description);
        *description = NULL;
    }
    return result;
}

static task_read_result read_custom(const char *dir, const char *name, char **body,
                                    char **description) {
    if (body) *body = NULL;
    if (description) *description = NULL;
    if (!tny_task_name_valid(name)) return TASK_BROKEN;
    int dir_fd = -1;
    task_read_result result = custom_dir_open(dir, &dir_fd);
    if (result != TASK_VALID) return result;
    char filename[TNY_TASK_NAME_MAX + 3];
    snprintf(filename, sizeof filename, "%s.md", name);
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = openat(dir_fd, filename, flags);
    int saved = errno;
    close(dir_fd);
    if (fd < 0) return saved == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    result = read_fd(fd, name, body, description);
    close(fd);
    return result;
}

static task_read_result read_workflow(const char *dir, const char *name, char **body,
                                      char **description) {
    if (body) *body = NULL;
    if (description) *description = NULL;
    if (!dir || !*dir || !tny_task_name_valid(name)) return TASK_ABSENT;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int dir_fd = open(dir, flags);
    if (dir_fd < 0) return errno == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = openat(dir_fd, name, flags);
    int saved = errno;
    close(dir_fd);
    if (fd < 0) return saved == ENOENT ? TASK_ABSENT : TASK_BROKEN;
    task_read_result result = read_fd(fd, name, body, description);
    close(fd);
    return result;
}

/* User preset discovery must not inherit path_home()'s general state-directory
 * fallback. With HOME absent, /tmp is shared ambient state rather than the
 * invoking user's configuration. Relative HOME values are likewise ignored
 * so a hostile environment cannot reinterpret them beneath the workspace. */
static char *user_task_root(void) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return NULL;
    return xstrdup(home);
}

int tny_task_set_explicit(tny_ctx *ctx, const char *name, const char *body, const char *source) {
    const char *resolved_source = source ? source : "explicit";
    if (!ctx || !tny_task_name_valid(name) || !body || !tny_task_source_valid(resolved_source))
        return TNY_TASK_INVALID;
    size_t body_len = strlen(body);
    if (!body_len || body_len > TNY_TASK_BODY_MAX || !body_nonempty(body, body_len) ||
        !utf8_valid_bytes(body, body_len))
        return TNY_TASK_INVALID;
    char *new_name = xstrdup(name);
    char *new_source = xstrdup(resolved_source);
    char *new_instructions = xstrdup(body);
    if (!new_name || !new_source || !new_instructions) {
        free(new_name);
        free(new_source);
        free(new_instructions);
        return TNY_TASK_OOM;
    }
    char digest[TNY_TASK_DIGEST_HEX_LEN + 1];
    uint8_t digest_bytes[TNY_TASK_DIGEST_HEX_LEN / 2];
    if (!sha1((const uint8_t *)body, body_len, digest_bytes)) {
        free(new_name);
        free(new_source);
        free(new_instructions);
        return TNY_TASK_OOM;
    }
    for (size_t i = 0; i < sizeof digest_bytes; i++)
        snprintf(digest + i * 2, 3, "%02x", digest_bytes[i]);
    free(ctx->task_name);
    free(ctx->task_source);
    free(ctx->task_instructions);
    ctx->task_name = new_name;
    ctx->task_source = new_source;
    ctx->task_instructions = new_instructions;
    memcpy(ctx->task_digest, digest, sizeof digest);
    ctx->task_explicit = true;
    return TNY_TASK_OK;
}

int tny_task_apply(tny_ctx *ctx, const char *name) {
    if (!ctx || !tny_task_name_valid(name)) return -1;
    char *body = NULL;
    /* Project-local definitions win over user definitions, then builtins. For
     * SSH, only remote discovery is allowed; no local launch tree is read. */
    if (ctx->ssh_host) {
        const char *builtin = tny_task_builtin_body(name);
        if (builtin) return tny_task_set_explicit(ctx, name, builtin, "builtin");
        return TNY_TASK_INVALID;
    }
    task_read_result result = read_workflow(getenv("TNY_WORKFLOW_TASK_DIR"), name, &body, NULL);
    const char *source = "workflow";
    if (result == TASK_NOMEM) return TNY_TASK_OOM;
    if (result == TASK_BROKEN) return TNY_TASK_INVALID;
    if (result == TASK_ABSENT) {
        result = read_custom(ctx->cwd, name, &body, NULL);
        source = "project";
        if (result == TASK_NOMEM) return TNY_TASK_OOM;
        if (result == TASK_BROKEN) return TNY_TASK_INVALID;
    }
    if (result == TASK_ABSENT) {
        char *home = user_task_root();
        if (home) {
            result = read_custom(home, name, &body, NULL);
            free(home);
            source = "user";
            if (result == TASK_NOMEM) return TNY_TASK_OOM;
            if (result == TASK_BROKEN) return TNY_TASK_INVALID;
        }
    }
    if (result == TASK_ABSENT) {
        const char *builtin = tny_task_builtin_body(name);
        if (builtin) return tny_task_set_explicit(ctx, name, builtin, "builtin");
        return TNY_TASK_INVALID;
    }
    int rc = tny_task_set_explicit(ctx, name, body, source);
    free(body);
    return rc;
}

void tny_task_collect(const tny_ctx *ctx, buf_t *out) {
    if (!ctx || !out || !ctx->task_instructions) return;
    buf_appends(out, "# Task preset: ");
    buf_appends(out, ctx->task_name ? ctx->task_name : "task");
    buf_appends(out, "\n\n");
    buf_appends(out, ctx->task_instructions);
    buf_appends(out, "\n\n");
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
    bool too_many;
} task_names;

static int names_add(task_names *names, const char *name) {
    if (!tny_task_name_valid(name)) return 0;
    for (size_t i = 0; i < names->count; i++)
        if (strcmp(names->items[i], name) == 0) return 0;
    if (names->count >= TNY_TASK_COUNT_MAX) {
        names->too_many = true;
        return -1;
    }
    if (names->count == names->capacity) {
        size_t capacity = names->capacity ? names->capacity * 2 : 8;
        char **items = realloc(names->items, capacity * sizeof *items);
        if (!items) return -1;
        names->items = items;
        names->capacity = capacity;
    }
    names->items[names->count] = xstrdup(name);
    if (!names->items[names->count]) return -1;
    names->count++;
    return 0;
}

static int names_scan(task_names *names, const char *dir, bool markdown_suffix) {
    if (!dir || !*dir) return 0;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(dir, flags);
    if (fd < 0) return 0;
    DIR *stream = fdopendir(fd);
    if (!stream) close(fd);
    if (!stream) return 0;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        size_t len = strlen(entry->d_name);
        if (markdown_suffix) {
            if (len <= 3 || strcmp(entry->d_name + len - 3, ".md") != 0) continue;
            char *name = xstrndup(entry->d_name, len - 3);
            if (!name || names_add(names, name) != 0) result = -1;
            free(name);
        } else if (names_add(names, entry->d_name) != 0) result = -1;
        if (result != 0) break;
    }
    closedir(stream);
    return result;
}

static int names_scan_custom(task_names *names, const char *root) {
    int fd = -1;
    if (custom_dir_open(root, &fd) != TASK_VALID) return 0;
    DIR *stream = fdopendir(fd);
    if (!stream) {
        close(fd);
        return 0;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        size_t len = strlen(entry->d_name);
        if (len <= 3 || strcmp(entry->d_name + len - 3, ".md") != 0) continue;
        char *name = xstrndup(entry->d_name, len - 3);
        if (!name || names_add(names, name) != 0) result = -1;
        free(name);
        if (result != 0) break;
    }
    closedir(stream); /* owns fd */
    return result;
}

static int compare_names(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static task_read_result describe_task(tny_ctx *ctx, const char *name, const char **source,
                                      char **description) {
    *source = NULL;
    *description = NULL;
    char *discard = NULL;
    if (!ctx->ssh_host) {
        task_read_result result =
            read_workflow(getenv("TNY_WORKFLOW_TASK_DIR"), name, &discard, description);
        free(discard);
        if (result != TASK_ABSENT) {
            *source = "workflow";
            return result;
        }
        result = read_custom(ctx->cwd, name, &discard, description);
        free(discard);
        if (result != TASK_ABSENT) {
            *source = "project";
            return result;
        }
        char *home = user_task_root();
        if (home) {
            result = read_custom(home, name, &discard, description);
            free(discard);
            free(home);
            if (result != TASK_ABSENT) {
                *source = "user";
                return result;
            }
        }
    }
    for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
        if (strcmp(name, BUILTINS[i].name) == 0) {
            *source = "builtin";
            *description = xstrdup(BUILTINS[i].description);
            return *description ? TASK_VALID : TASK_NOMEM;
        }
    }
    return TASK_ABSENT;
}

void tny_task_list_free(tny_task_info *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].source);
        free(items[i].description);
    }
    free(items);
}

int tny_task_list(tny_ctx *ctx, tny_task_info **out, size_t *count) {
    if (!ctx || !out || !count) return TNY_TASK_INVALID;
    *out = NULL;
    *count = 0;
    task_names names = {0};
    for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++)
        if (names_add(&names, BUILTINS[i].name) != 0) goto oom;
    if (!ctx->ssh_host) {
        if (names_scan(&names, getenv("TNY_WORKFLOW_TASK_DIR"), false) != 0) goto scan_failed;
        char *home = user_task_root();
        if (names_scan_custom(&names, ctx->cwd) != 0 ||
            (home && names_scan_custom(&names, home) != 0)) {
            free(home);
            goto scan_failed;
        }
        free(home);
    }
    if (names.too_many) goto scan_failed;
    qsort(names.items, names.count, sizeof *names.items, compare_names);
    tny_task_info *items = calloc(names.count, sizeof *items);
    if (!items) goto oom;
    size_t used = 0;
    for (size_t i = 0; i < names.count; i++) {
        const char *source = NULL;
        char *description = NULL;
        task_read_result result = describe_task(ctx, names.items[i], &source, &description);
        if (result == TASK_ABSENT) continue;
        if (result == TASK_NOMEM) {
            tny_task_list_free(items, used);
            goto oom;
        }
        items[used].name = xstrdup(names.items[i]);
        items[used].source = xstrdup(source);
        items[used].description = description;
        items[used].valid = result == TASK_VALID;
        if (!items[used].name || !items[used].source) {
            tny_task_list_free(items, used + 1);
            goto oom;
        }
        used++;
    }
    for (size_t i = 0; i < names.count; i++) free(names.items[i]);
    free(names.items);
    *out = items;
    *count = used;
    return TNY_TASK_OK;
scan_failed:
    if (names.too_many) {
        for (size_t i = 0; i < names.count; i++) free(names.items[i]);
        free(names.items);
        return TNY_TASK_INVALID;
    }
oom:
    for (size_t i = 0; i < names.count; i++) free(names.items[i]);
    free(names.items);
    return TNY_TASK_OOM;
}

char *tny_task_names_joined(tny_ctx *ctx) {
    tny_task_info *items = NULL;
    size_t count = 0;
    if (tny_task_list(ctx, &items, &count) != TNY_TASK_OK) return NULL;
    buf_t out;
    buf_init(&out);
    for (size_t i = 0; i < count; i++)
        if (items[i].valid) buf_appendf(&out, "%s%s", out.len ? "|" : "", items[i].name);
    tny_task_list_free(items, count);
    return buf_detach(&out);
}
