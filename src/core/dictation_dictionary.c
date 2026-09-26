/* User dictionary for transcript normalization (ADR 0175; Lean:
 * Dictation.Dictionary). Only these entries, never settings or workspace
 * content, are sent with a transcript. Validation is strict and bounded. */
#include "core/dictation_normalize.h"
#include "json/json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void tny_dictionary_free(tny_dictionary *d) {
    if (!d) return;
    for (size_t i = 0; i < d->n; i++) {
        tny_dictionary_entry *e = &d->entries[i];
        free(e->word);
        free(e->context);
        for (size_t a = 0; a < e->n_aliases; a++) free(e->aliases[a]);
        free(e->aliases);
    }
    free(d->entries);
    memset(d, 0, sizeof *d);
}

/* No control characters, valid UTF-8, bounded; `word` also forbids
 * surrounding whitespace and must carry a letter or digit (a word made only
 * of punctuation could replace a spoken word with nothing). */
static bool plain_text(const char *s, size_t n, size_t max) {
    if (n > max || memchr(s, 0, n) || !utf8_valid_bytes(s, n)) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || (c == 0xc2 && i + 1 < n && (unsigned char)s[i + 1] <= 0x9f))
            return false;
    }
    return true;
}

static bool meaningful(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
            return true;
    }
    return false;
}

static bool word_text(const char *s, size_t n) {
    return n && plain_text(s, n, TNY_DICTIONARY_WORD_MAX) && s[0] != ' ' && s[n - 1] != ' ' &&
           meaningful(s, n);
}

static bool fail(char *err, size_t len, const char *what, const char *word) {
    if (word) snprintf(err, len, "dictionary %s: %.64s", what, word);
    else snprintf(err, len, "dictionary %s", what);
    return false;
}

static bool parse_entry(yyjson_val *key, yyjson_val *value, tny_dictionary_entry *e, char *err,
                        size_t len) {
    const char *word = yyjson_get_str(key);
    size_t wn = yyjson_get_len(key);
    if (!word_text(word, wn)) return fail(err, len, "word is empty, too long or invalid", NULL);
    e->word = xstrndup(word, wn);
    if (!e->word) return fail(err, len, "out of memory", NULL);
    yyjson_val *context = value, *aliases = NULL, *casing = NULL;
    if (yyjson_is_obj(value)) {
        context = jget(value, "context");
        aliases = jget(value, "aliases");
        casing = jget(value, "case");
        size_t known = (context != NULL) + (aliases != NULL) + (casing != NULL);
        if (yyjson_obj_size(value) != known)
            return fail(err, len, "entry has an unknown field", word);
    } else if (!yyjson_is_str(value)) {
        return fail(err, len, "entry must be a string or an object", word);
    }
    if (context &&
        (!yyjson_is_str(context) ||
         !plain_text(yyjson_get_str(context), yyjson_get_len(context), TNY_DICTIONARY_CONTEXT_MAX)))
        return fail(err, len, "context is not bounded plain text", word);
    e->context =
        xstrndup(context ? yyjson_get_str(context) : "", context ? yyjson_get_len(context) : 0);
    if (!e->context) return fail(err, len, "out of memory", NULL);
    if (casing) {
        const char *c = yyjson_get_str(casing);
        if (!c || (strcmp(c, "exact") != 0 && strcmp(c, "insensitive") != 0))
            return fail(err, len, "case must be \"exact\" or \"insensitive\"", word);
        e->exact = strcmp(c, "exact") == 0;
    }
    if (aliases) {
        size_t n = yyjson_arr_size(aliases);
        if (!yyjson_is_arr(aliases) || n > TNY_DICTIONARY_ALIASES_MAX)
            return fail(err, len, "aliases must be an array of at most 8 strings", word);
        /* One slot even when empty keeps every later access non-null. */
        e->aliases = calloc(n ? n : 1, sizeof *e->aliases);
        if (!e->aliases) return fail(err, len, "out of memory", NULL);
        size_t i, max;
        yyjson_val *alias;
        yyjson_arr_foreach(aliases, i, max, alias) {
            const char *a = yyjson_get_str(alias);
            size_t an = yyjson_get_len(alias);
            if (!a || !word_text(a, an)) return fail(err, len, "alias is empty or invalid", word);
            e->aliases[e->n_aliases] = xstrndup(a, an);
            if (!e->aliases[e->n_aliases]) return fail(err, len, "out of memory", NULL);
            e->n_aliases++;
        }
    }
    return true;
}

/* Entries of a validated, allocated root object; stops at the first error. */
static bool parse_entries(yyjson_val *root, tny_dictionary *out, char *err, size_t errlen) {
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, max, key, value) {
        const char *name = yyjson_get_str(key);
        size_t name_len = yyjson_get_len(key);
        if (!name) return fail(err, errlen, "must be a JSON object of word entries", NULL);
        if (!strcmp(name, "$schema") && yyjson_is_str(value)) continue;
        for (size_t j = 0; j < out->n; j++)
            if (name_len == strlen(out->entries[j].word) &&
                memcmp(out->entries[j].word, name, name_len) == 0)
                return fail(err, errlen, "repeats word", out->entries[j].word);
        if (out->n == TNY_DICTIONARY_ENTRIES_MAX)
            return fail(err, errlen, "has more than 256 words", NULL);
        /* Count the entry before parsing so free() releases partial fields. */
        if (!parse_entry(key, value, &out->entries[out->n++], err, errlen)) return false;
    }
    return true;
}

bool tny_dictionary_parse(const char *json, size_t len, tny_dictionary *out, char *err,
                          size_t errlen) {
    memset(out, 0, sizeof *out);
    if (len > TNY_DICTIONARY_FILE_MAX) return fail(err, errlen, "exceeds 64 KiB", NULL);
    yyjson_doc *doc = jparse(json, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool ok = yyjson_is_obj(root);
    if (!ok) fail(err, errlen, "must be a JSON object of word entries", NULL);
    size_t n = ok ? yyjson_obj_size(root) : 0;
    if (ok && n > TNY_DICTIONARY_ENTRIES_MAX + 1)
        ok = fail(err, errlen, "has more than 256 words", NULL);
    if (ok && n) {
        out->entries = calloc(n, sizeof *out->entries);
        if (!out->entries) ok = fail(err, errlen, "out of memory", NULL);
    }
    if (ok) ok = parse_entries(root, out, err, errlen);
    yyjson_doc_free(doc);
    if (!ok) tny_dictionary_free(out);
    return ok;
}

bool tny_dictionary_merge(tny_dictionary *user, tny_dictionary *project, tny_dictionary *out) {
    memset(out, 0, sizeof *out);
    size_t total = project->n + user->n;
    out->entries = total ? calloc(total, sizeof *out->entries) : NULL;
    bool ok = !total || out->entries;
    for (size_t i = 0; ok && i < project->n; i++) {
        out->entries[out->n++] = project->entries[i];
        memset(&project->entries[i], 0, sizeof project->entries[i]);
    }
    for (size_t i = 0; ok && i < user->n; i++) {
        bool shadowed = false;
        for (size_t j = 0; j < out->n && !shadowed; j++)
            shadowed = out->entries[j].word && !strcmp(out->entries[j].word, user->entries[i].word);
        if (shadowed) continue;
        out->entries[out->n++] = user->entries[i];
        memset(&user->entries[i], 0, sizeof user->entries[i]);
    }
    tny_dictionary_free(user);
    tny_dictionary_free(project);
    if (!ok) tny_dictionary_free(out);
    return ok;
}

/* 0 loaded, 1 missing, -1 invalid. Never follows a FIFO or blocks on one. */
static int load_file(const char *path, tny_dictionary *out, char *err, size_t len) {
    memset(out, 0, sizeof *out);
    int fd = path ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    if (fd < 0) {
        if (path && (errno == ENOENT || errno == ENOTDIR)) return 1;
        snprintf(err, len, "cannot read %s", path ? path : "dictionary");
        return -1;
    }
    struct stat st;
    buf_t data = {0};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              (uint64_t)st.st_size <= TNY_DICTIONARY_FILE_MAX;
    while (ok) {
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (size_t)n > TNY_DICTIONARY_FILE_MAX - data.len) ok = false;
        if (n <= 0) break;
        buf_append(&data, chunk, (size_t)n);
        if (data.oom) ok = false;
    }
    close(fd);
    char detail[160] = "";
    if (ok)
        ok = tny_dictionary_parse(data.data ? data.data : "", data.len, out, detail, sizeof detail);
    else snprintf(detail, sizeof detail, "is not a regular file of at most 64 KiB");
    buf_free(&data);
    if (!ok) snprintf(err, len, "%s %s", path, detail);
    return ok ? 0 : -1;
}

bool tny_dictionary_load(const char *cwd, tny_dictionary *out, char *err, size_t len) {
    memset(out, 0, sizeof *out);
    tny_dictionary user = {0}, project = {0};
    char *dir = path_tny_dir();
    char *user_path = dir ? path_join(dir, "dictionary.json") : NULL;
    char here[4096];
    if (!cwd) cwd = getcwd(here, sizeof here);
    char *project_dir = cwd ? path_join(cwd, ".tny") : NULL;
    char *project_path = project_dir ? path_join(project_dir, "dictionary.json") : NULL;
    bool ok = user_path && project_path;
    if (!ok) snprintf(err, len, "cannot locate dictionary files");
    /* The workspace can be the home directory: then both names are one file. */
    bool same = ok && !strcmp(user_path, project_path);
    if (ok && load_file(user_path, &user, err, len) < 0) ok = false;
    if (ok && !same && load_file(project_path, &project, err, len) < 0) ok = false;
    if (ok && !tny_dictionary_merge(&user, &project, out)) {
        snprintf(err, len, "out of memory");
        ok = false;
    }
    tny_dictionary_free(&user);
    tny_dictionary_free(&project);
    free(dir);
    free(user_path);
    free(project_dir);
    free(project_path);
    return ok;
}
