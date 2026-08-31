/* callbacks.c — authenticated Connect JSON callbacks served on one loopback socket. */
#include "backends/cursor/callbacks.h"

#include "json/json.h"
#include "lib/custom_tools.h"
#include "net/http_server.h"
#include "util/tny_poll.h"
#include "util/tny_wake.h"
#include "util/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOOL_PATH           "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool"
#define STORE_PATH          "/sdk.v1.SdkStoreCallbackService/CallStore"
#define STORE_LIMIT_DEFAULT 100u
#define STORE_LIMIT_MAX     1000u

typedef struct pending_tool {
    uint64_t request_id;
    tny_tool_call *call;
    struct pending_tool *next;
} pending_tool;

struct cursor_callbacks {
    http_server *server;
    custom_tool_registry *tools;
    char *token;
    char *store_root;
    bool tools_started;
    bool store_started;
    bool allow_sensitive;
    pending_tool *pending;
    buf_t reply;
    tny_wake pump_wake;
    pthread_t pump_thread;
    atomic_bool pump_stop;
    atomic_bool blocking_mode;
    bool pump_wake_ready;
    bool pump_running;
    cursor_callbacks_thread_create_fn thread_create;
};

static void fail(char *err, size_t errlen, const char *message) {
    if (err && errlen) snprintf(err, errlen, "%s", message);
}

static bool random_token(char out[65]) {
    unsigned char bytes[32];
    int fd;
    do fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < sizeof bytes) {
        ssize_t n = read(fd, bytes + offset, sizeof bytes - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (offset != sizeof bytes) {
        secure_zero(bytes, sizeof bytes);
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof bytes; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
    secure_zero(bytes, sizeof bytes);
    return true;
}

static bool exact_path(const char *path, size_t len, const char *expected) {
    return strlen(expected) == len && memcmp(path, expected, len) == 0;
}

static void set_reply(cursor_callbacks *cb, http_server_response *response, int status,
                      const char *json) {
    buf_clear(&cb->reply);
    buf_appends(&cb->reply, json);
    response->status = status;
    response->content_type = "application/json";
    response->body = cb->reply.data;
    response->body_len = cb->reply.len;
}

static bool append_tool_result(buf_t *out, const char *result, bool is_error) {
    yyjson_doc *doc = result ? jparse(result, strlen(result)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    buf_appends(out, "{\"result\":");
    if (is_error) {
        buf_appends(out, "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":");
        jescape(out, result ? result : "tool failed");
        buf_appends(out, "}]}");
    } else if (root && yyjson_is_obj(root)) {
        buf_appends(out, result);
    } else if (root) {
        buf_appends(out, "{\"value\":");
        buf_appends(out, result);
        buf_appends(out, "}");
    } else {
        buf_appends(out, "{\"content\":[{\"type\":\"text\",\"text\":");
        jescape(out, result ? result : "");
        buf_appends(out, "}]}");
    }
    buf_appends(out, "}");
    yyjson_doc_free(doc);
    return !buf_oom(out);
}

static int tool_request(cursor_callbacks *cb, yyjson_val *root, http_server_response *response) {
    const char *name = jget_str(root, "toolName");
    if (!name) name = jget_str(root, "tool_name");
    yyjson_val *args = jget(root, "args");
    if (!name || !*name || !args || !yyjson_is_obj(args)) {
        set_reply(cb, response, 400, "{\"error\":\"toolName and object args are required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    tny_tool_registration *tool = custom_tools_find(cb->tools, name);
    if (!tool) {
        set_reply(cb, response, 404, "{\"error\":\"unknown custom tool\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    if (custom_tool_sensitive(tool) && !cb->allow_sensitive) {
        set_reply(cb, response, 403, "{\"error\":\"sensitive custom tool denied by policy\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    char *arguments = jwrite_val(args);
    if (!arguments) {
        set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    tny_tool_call *call = NULL;
    char *result = NULL;
    bool is_error = false;
    int32_t status = custom_tool_invoke(tool, arguments, &call, &result, &is_error);
    free(arguments);
    if (status == TNY_TOOL_INVOKE_ASYNC) {
        pending_tool *pending = calloc(1, sizeof *pending);
        if (!pending) {
            custom_tool_invalidate(call);
            set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
            return HTTP_SERVER_POST_HANDLED;
        }
        pending->request_id = response->request_id;
        pending->call = call;
        pending->next = cb->pending;
        cb->pending = pending;
        return HTTP_SERVER_POST_DEFERRED;
    }
    if (status != TNY_TOOL_INVOKE_SYNC) {
        set_reply(cb, response, status == TNY_STATUS_BACKPRESSURE ? 429 : 422,
                  "{\"error\":\"custom tool invocation failed\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    buf_clear(&cb->reply);
    if (!append_tool_result(&cb->reply, result, is_error)) {
        free(result);
        set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    free(result);
    response->status = 200;
    response->body = cb->reply.data;
    response->body_len = cb->reply.len;
    return HTTP_SERVER_POST_HANDLED;
}

static const char *store_dir_name(const char *substore) {
    if (strcmp(substore, "agents") == 0) return "agents";
    if (strcmp(substore, "runs") == 0) return "runs";
    if (strcmp(substore, "runEvents") == 0 || strcmp(substore, "run_events") == 0)
        return "run-events";
    if (strcmp(substore, "checkpoints") == 0) return "checkpoints";
    return NULL;
}

static char *store_dir(cursor_callbacks *cb, const char *substore) {
    const char *name = store_dir_name(substore);
    return name ? path_join(cb->store_root, name) : NULL;
}

static char *record_key(const char *substore, yyjson_val *record) {
    const char *a = NULL, *b = NULL;
    if (strcmp(substore, "agents") == 0) a = jget_str(record, "agentId");
    else if (strcmp(substore, "runs") == 0) {
        a = jget_str(record, "agentId");
        b = jget_str(record, "runId");
    } else if (strcmp(substore, "checkpoints") == 0) {
        a = jget_str(record, "agentId");
        b = jget_str(record, "blobId");
    }
    if (!a || !*a || (b && !*b)) return NULL;
    buf_t key;
    buf_init(&key);
    buf_appends(&key, a);
    if (b) {
        buf_appends(&key, "\n");
        buf_appends(&key, b);
    }
    return buf_detach(&key);
}

static char *input_key(const char *substore, yyjson_val *input) {
    return record_key(substore, input);
}

static char *hash_path(const char *dir, const char *key) {
    uint8_t digest[20];
    if (!sha1((const uint8_t *)key, strlen(key), digest)) return NULL;
    char name[46];
    for (size_t i = 0; i < sizeof digest; i++) snprintf(name + i * 2, 3, "%02x", digest[i]);
    memcpy(name + 40, ".json", 6);
    return path_join(dir, name);
}

static char *envelope_json(const char *key, yyjson_val *value) {
    char *json = jwrite_val(value);
    if (!json) return NULL;
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"key\":");
    jescape(&out, key);
    buf_appends(&out, ",\"value\":");
    buf_appends(&out, json);
    buf_appends(&out, "}");
    free(json);
    return buf_detach(&out);
}

static int save_record(const char *dir, const char *key, yyjson_val *value, bool create_only) {
    char *path = hash_path(dir, key);
    char *data = envelope_json(key, value);
    if (!path || !data) {
        free(path);
        free(data);
        return -1;
    }
    if (file_exists(path)) {
        yyjson_doc *existing = jparse_file(path);
        const char *existing_key = existing ? jget_str(yyjson_doc_get_root(existing), "key") : NULL;
        bool collision = !existing_key || strcmp(existing_key, key) != 0;
        yyjson_doc_free(existing);
        if (collision || create_only) {
            free(path);
            free(data);
            return collision ? -2 : 1;
        }
    }
    int rc = file_write_atomic(path, data, strlen(data));
    free(path);
    free(data);
    return rc == 0 ? 0 : -1;
}

static bool valid_base64(const char *value) {
    if (!value) return false;
    size_t len = strlen(value);
    if (len % 4 != 0) return false;
    size_t padding = len && value[len - 1] == '=' ? 1 : 0;
    if (len > 1 && value[len - 2] == '=') padding++;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        bool alphabet = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        if (!alphabet && !(ch == '=' && i >= len - padding)) return false;
    }
    return true;
}

static yyjson_doc *load_record(const char *dir, const char *key) {
    char *path = hash_path(dir, key);
    yyjson_doc *doc = path ? jparse_file(path) : NULL;
    free(path);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root) || !jget_str(root, "key") || strcmp(jget_str(root, "key"), key) != 0 ||
        !jget(root, "value")) {
        yyjson_doc_free(doc);
        return NULL;
    }
    return doc;
}

static bool string_array_has(yyjson_val *array, const char *value) {
    if (!array || !yyjson_is_arr(array)) return false;
    size_t index, max;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, max, item) {
        if (yyjson_is_str(item) && strcmp(yyjson_get_str(item), value) == 0) return true;
    }
    return false;
}

static bool matches_filter(const char *substore, yyjson_val *record, yyjson_val *filter) {
    if (!filter || !yyjson_is_obj(filter)) return true;
    const char *agent = jget_str(record, "agentId");
    const char *run = jget_str(record, "runId");
    const char *blob = jget_str(record, "blobId");
    yyjson_val *agent_ids = jget(filter, "agentIds");
    yyjson_val *run_ids = jget(filter, "runIds");
    yyjson_val *blob_ids = jget(filter, "blobIds");
    if (agent_ids && yyjson_arr_size(agent_ids) && (!agent || !string_array_has(agent_ids, agent)))
        return false;
    if (run_ids && yyjson_arr_size(run_ids) && (!run || !string_array_has(run_ids, run)))
        return false;
    if (blob_ids && yyjson_arr_size(blob_ids) && (!blob || !string_array_has(blob_ids, blob)))
        return false;
    const char *cwd = jget_str(filter, "cwd");
    if (cwd && (!jget_str(record, "cwd") || strcmp(cwd, jget_str(record, "cwd")) != 0))
        return false;
    (void)substore;
    return true;
}

typedef struct {
    char *key;
    char *json;
    uint64_t seq;
} listed_record;

static int listed_compare(const void *left, const void *right) {
    const listed_record *a = left, *b = right;
    if (a->seq || b->seq) return a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0;
    return strcmp(a->key, b->key);
}

static void listed_free(listed_record *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(items[i].key);
        free(items[i].json);
    }
    free(items);
}

static void output_value(cursor_callbacks *cb, http_server_response *response, yyjson_val *value) {
    char *json = value ? jwrite_val(value) : NULL;
    if (value && !json) {
        set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
        return;
    }
    buf_clear(&cb->reply);
    if (!value) buf_appends(&cb->reply, "{}");
    else {
        buf_appends(&cb->reply, "{\"output\":");
        buf_appends(&cb->reply, json);
        buf_appends(&cb->reply, "}");
    }
    free(json);
    response->status = 200;
    response->body = cb->reply.data;
    response->body_len = cb->reply.len;
}

static int list_records(cursor_callbacks *cb, const char *substore, const char *dir,
                        yyjson_val *input, http_server_response *response) {
    yyjson_val *filter = jget(input, "filter");
    const char *cursor = jget_str(filter, "cursor");
    uint64_t limit = (uint64_t)jget_int(filter, "limit", STORE_LIMIT_DEFAULT);
    if (strcmp(substore, "runEvents") == 0 || strcmp(substore, "run_events") == 0) {
        filter = NULL;
        cursor = jget_str(input, "afterOffset");
        limit = (uint64_t)jget_int(input, "limit", STORE_LIMIT_DEFAULT);
    }
    if (!limit || limit > STORE_LIMIT_MAX) limit = STORE_LIMIT_DEFAULT;
    bool event_store = strcmp(substore, "runEvents") == 0 || strcmp(substore, "run_events") == 0;
    const char *event_run = event_store ? jget_str(input, "runId") : NULL;
    if (event_store && (!event_run || !*event_run)) {
        set_reply(cb, response, 400, "{\"error\":\"runEvents.list requires runId\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    DIR *scan = opendir(dir);
    listed_record *items = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    while (scan && (entry = readdir(scan))) {
        if (!str_ends(entry->d_name, ".json")) continue;
        char *path = path_join(dir, entry->d_name);
        yyjson_doc *doc = path ? jparse_file(path) : NULL;
        free(path);
        yyjson_val *envelope = doc ? yyjson_doc_get_root(doc) : NULL;
        yyjson_val *record = jget(envelope, "value");
        if (!record || !yyjson_is_obj(record) || !matches_filter(substore, record, filter) ||
            (event_store &&
             (!jget_str(record, "runId") || strcmp(jget_str(record, "runId"), event_run) != 0))) {
            yyjson_doc_free(doc);
            continue;
        }
        const char *position = event_store ? jget_str(record, "offset") : jget_str(envelope, "key");
        char *json = jwrite_val(record);
        char *key = position ? xstrdup(position) : NULL;
        if (!json || !key) {
            free(json);
            free(key);
            yyjson_doc_free(doc);
            listed_free(items, count);
            if (scan) closedir(scan);
            set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
            return HTTP_SERVER_POST_HANDLED;
        }
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 16;
            listed_record *next = realloc(items, next_capacity * sizeof *next);
            if (!next) {
                free(json);
                free(key);
                yyjson_doc_free(doc);
                listed_free(items, count);
                if (scan) closedir(scan);
                set_reply(cb, response, 500, "{\"error\":\"out of memory\"}");
                return HTTP_SERVER_POST_HANDLED;
            }
            items = next;
            capacity = next_capacity;
        }
        items[count++] =
            (listed_record){key, json, event_store ? (uint64_t)jget_int(record, "seq", 0) : 0};
        yyjson_doc_free(doc);
    }
    if (scan) closedir(scan);
    if (count > 1) qsort(items, count, sizeof *items, listed_compare);
    uint64_t after_seq = event_store && cursor ? (uint64_t)strtoull(cursor, NULL, 10) : 0;
    size_t start = 0;
    while (start < count && ((event_store && items[start].seq <= after_seq) ||
                             (!event_store && cursor && strcmp(items[start].key, cursor) <= 0)))
        start++;
    size_t available = count - start;
    size_t emit_count = available < limit ? available : (size_t)limit;
    bool more = available > emit_count;
    buf_clear(&cb->reply);
    buf_appends(&cb->reply, "{\"output\":{\"items\":[");
    for (size_t i = 0; i < emit_count; i++) {
        listed_record *item = &items[start + i];
        if (i) buf_appends(&cb->reply, ",");
        if (strcmp(substore, "checkpoints") == 0) {
            yyjson_doc *doc = jparse(item->json, strlen(item->json));
            yyjson_val *record = doc ? yyjson_doc_get_root(doc) : NULL;
            jescape(&cb->reply, jget_str(record, "blobId"));
            yyjson_doc_free(doc);
        } else buf_appends(&cb->reply, item->json);
    }
    buf_appends(&cb->reply, "]");
    if (more && emit_count) {
        buf_appends(&cb->reply, event_store ? ",\"nextOffset\":" : ",\"nextCursor\":");
        jescape(&cb->reply, items[start + emit_count - 1].key);
    }
    buf_appends(&cb->reply, "}}");
    listed_free(items, count);
    response->status = 200;
    response->body = cb->reply.data;
    response->body_len = cb->reply.len;
    return HTTP_SERVER_POST_HANDLED;
}

static int delete_records(cursor_callbacks *cb, const char *substore, const char *dir,
                          yyjson_val *input, http_server_response *response) {
    yyjson_val *filter = jget(input, "filter");
    if (!filter || !yyjson_is_obj(filter)) {
        set_reply(cb, response, 400, "{\"error\":\"delete filter is required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    DIR *scan = opendir(dir);
    struct dirent *entry;
    while (scan && (entry = readdir(scan))) {
        if (!str_ends(entry->d_name, ".json")) continue;
        char *path = path_join(dir, entry->d_name);
        yyjson_doc *doc = path ? jparse_file(path) : NULL;
        yyjson_val *record = doc ? jget(yyjson_doc_get_root(doc), "value") : NULL;
        if (path && record && yyjson_is_obj(record) && matches_filter(substore, record, filter))
            (void)unlink(path);
        free(path);
        yyjson_doc_free(doc);
    }
    if (scan) closedir(scan);
    output_value(cb, response, NULL);
    return HTTP_SERVER_POST_HANDLED;
}

static int append_event(cursor_callbacks *cb, const char *dir, yyjson_val *input,
                        http_server_response *response) {
    const char *run_id = jget_str(input, "runId");
    const char *event_type = jget_str(input, "eventType");
    if (!run_id || !event_type) {
        set_reply(cb, response, 400, "{\"error\":\"runId and eventType are required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    const char *idempotency = jget_str(input, "idempotencyKey");
    DIR *scan = opendir(dir);
    struct dirent *entry;
    uint64_t max_seq = 0;
    while (scan && (entry = readdir(scan))) {
        if (!str_ends(entry->d_name, ".json")) continue;
        char *path = path_join(dir, entry->d_name);
        yyjson_doc *doc = path ? jparse_file(path) : NULL;
        free(path);
        yyjson_val *record = doc ? jget(yyjson_doc_get_root(doc), "value") : NULL;
        if (record && yyjson_is_obj(record) && jget_str(record, "runId") &&
            strcmp(jget_str(record, "runId"), run_id) == 0) {
            uint64_t seq = (uint64_t)jget_int(record, "seq", 0);
            if (seq > max_seq) max_seq = seq;
            if (idempotency && jget_str(record, "idempotencyKey") &&
                strcmp(idempotency, jget_str(record, "idempotencyKey")) == 0) {
                output_value(cb, response, record);
                yyjson_doc_free(doc);
                closedir(scan);
                return HTTP_SERVER_POST_HANDLED;
            }
        }
        yyjson_doc_free(doc);
    }
    if (scan) closedir(scan);
    uint64_t seq = max_seq + 1;
    buf_t json;
    buf_init(&json);
    buf_appends(&json, "{\"runId\":");
    jescape(&json, run_id);
    buf_appendf(&json, ",\"seq\":%llu,\"offset\":\"%llu\",\"eventType\":", (unsigned long long)seq,
                (unsigned long long)seq);
    jescape(&json, event_type);
    yyjson_val *payload = jget(input, "payload");
    char *payload_json = payload ? jwrite_val(payload) : NULL;
    buf_appends(&json, ",\"payload\":");
    buf_appends(&json, payload_json ? payload_json : "null");
    free(payload_json);
    buf_appends(&json, ",\"payloadRef\":");
    jget_str(input, "payloadRef") ? jescape(&json, jget_str(input, "payloadRef"))
                                  : buf_appends(&json, "null");
    buf_appends(&json, ",\"idempotencyKey\":");
    idempotency ? jescape(&json, idempotency) : buf_appends(&json, "null");
    buf_appendf(&json, ",\"createdAt\":%lld}", (long long)now_ms());
    yyjson_doc *event_doc = jparse(json.data, json.len);
    yyjson_val *event = event_doc ? yyjson_doc_get_root(event_doc) : NULL;
    char key[512];
    snprintf(key, sizeof key, "%s\n%020llu", run_id, (unsigned long long)seq);
    int saved = event ? save_record(dir, key, event, true) : -1;
    if (saved == 0) output_value(cb, response, event);
    else set_reply(cb, response, 500, "{\"error\":\"could not append run event\"}");
    yyjson_doc_free(event_doc);
    buf_free(&json);
    return HTTP_SERVER_POST_HANDLED;
}

static int store_request(cursor_callbacks *cb, yyjson_val *root, http_server_response *response) {
    const char *substore = jget_str(root, "substore");
    const char *method = jget_str(root, "method");
    yyjson_val *input = jget(root, "input");
    if (!substore || !method || !input || !yyjson_is_obj(input) || !store_dir_name(substore)) {
        set_reply(cb, response, 400,
                  "{\"error\":\"valid substore, method, and input are required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    char *dir = store_dir(cb, substore);
    if (!dir || mkdir_p(dir) != 0) {
        free(dir);
        set_reply(cb, response, 500, "{\"error\":\"store unavailable\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    if (strcmp(method, "list") == 0) {
        int rc = list_records(cb, substore, dir, input, response);
        free(dir);
        return rc;
    }
    if (strcmp(method, "delete") == 0) {
        int rc = delete_records(cb, substore, dir, input, response);
        free(dir);
        return rc;
    }
    if ((strcmp(substore, "runEvents") == 0 || strcmp(substore, "run_events") == 0) &&
        strcmp(method, "append") == 0) {
        int rc = append_event(cb, dir, input, response);
        free(dir);
        return rc;
    }
    yyjson_val *record = input;
    if (strcmp(substore, "agents") == 0 && jget(input, "agent")) record = jget(input, "agent");
    if (strcmp(substore, "runs") == 0 && jget(input, "run")) record = jget(input, "run");
    char *key = input_key(substore, strcmp(method, "get") == 0 ? input : record);
    if (!key) {
        free(dir);
        set_reply(cb, response, 400, "{\"error\":\"store identity fields are required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    if (strcmp(method, "get") == 0) {
        yyjson_doc *doc = load_record(dir, key);
        yyjson_val *value = doc ? jget(yyjson_doc_get_root(doc), "value") : NULL;
        if (strcmp(substore, "checkpoints") == 0) {
            buf_clear(&cb->reply);
            if (value) {
                buf_appends(&cb->reply, "{\"output\":{\"found\":true,\"data\":");
                jescape(&cb->reply, jget_str(value, "data"));
                buf_appends(&cb->reply, "}}");
            } else {
                buf_appends(&cb->reply, "{\"output\":{\"found\":false}}");
            }
            response->status = 200;
            response->body = cb->reply.data;
            response->body_len = cb->reply.len;
        } else output_value(cb, response, value);
        yyjson_doc_free(doc);
    } else if (strcmp(method, "create") == 0 || strcmp(method, "update") == 0) {
        char *existing_path = hash_path(dir, key);
        bool exists = existing_path && file_exists(existing_path);
        free(existing_path);
        if (strcmp(method, "update") == 0 && !exists) {
            set_reply(cb, response, 404, "{\"error\":\"record not found\"}");
        } else if (strcmp(substore, "checkpoints") == 0 &&
                   !valid_base64(jget_str(record, "data"))) {
            set_reply(cb, response, 400, "{\"error\":\"checkpoint data must be base64\"}");
        } else {
            int saved = save_record(dir, key, record, strcmp(method, "create") == 0);
            if (saved == 1) set_reply(cb, response, 409, "{\"error\":\"record already exists\"}");
            else if (saved == -2)
                set_reply(cb, response, 409, "{\"error\":\"record hash collision\"}");
            else if (saved != 0)
                set_reply(cb, response, 500, "{\"error\":\"record write failed\"}");
            else if (strcmp(substore, "checkpoints") == 0) output_value(cb, response, NULL);
            else output_value(cb, response, record);
        }
    } else {
        set_reply(cb, response, 400, "{\"error\":\"unsupported store method\"}");
    }
    free(key);
    free(dir);
    return HTTP_SERVER_POST_HANDLED;
}

static int callback_post(const http_server_request *request, http_server_response *response,
                         void *ud) {
    cursor_callbacks *cb = ud;
    bool tool = exact_path(request->path, request->path_len, TOOL_PATH);
    bool store = exact_path(request->path, request->path_len, STORE_PATH);
    if ((!tool && !store) || (tool && !cb->tools_started) || (store && !cb->store_started))
        return HTTP_SERVER_POST_NOT_FOUND;
    if (!request->connect_protocol_v1) {
        set_reply(cb, response, 400, "{\"error\":\"Connect-Protocol-Version: 1 is required\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    if (tool && atomic_load_explicit(&cb->blocking_mode, memory_order_acquire)) {
        set_reply(cb, response, 503,
                  "{\"error\":\"custom tools unavailable during blocking bridge RPC\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    yyjson_doc *doc = jparse(request->body, request->body_len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        set_reply(cb, response, 400, "{\"error\":\"request must be a JSON object\"}");
        return HTTP_SERVER_POST_HANDLED;
    }
    int rc = tool ? tool_request(cb, root, response) : store_request(cb, root, response);
    yyjson_doc_free(doc);
    return rc;
}

typedef struct {
    buf_t *out;
    yyjson_val *configured;
    bool first;
} definitions_ctx;

static bool append_definition(const tny_tool_registration *tool, void *ud) {
    definitions_ctx *ctx = ud;
    if (!ctx->first) buf_appends(ctx->out, ",");
    ctx->first = false;
    jescape(ctx->out, custom_tool_name(tool));
    buf_appends(ctx->out, ":{\"description\":");
    jescape(ctx->out, custom_tool_description(tool));
    buf_appends(ctx->out, ",\"inputSchema\":");
    buf_appends(ctx->out, custom_tool_schema(tool));
    yyjson_val *configured = ctx->configured ? jget(ctx->configured, custom_tool_name(tool)) : NULL;
    yyjson_val *output = jget(configured, "outputSchema");
    if (!output) output = jget(configured, "output_schema");
    if (output && yyjson_is_obj(output)) {
        char *json = jwrite_val(output);
        if (!json) return false;
        buf_appends(ctx->out, ",\"outputSchema\":");
        buf_appends(ctx->out, json);
        free(json);
    }
    buf_appends(ctx->out, "}");
    return !buf_oom(ctx->out);
}

cursor_callbacks *cursor_callbacks_start(const cursor_callbacks_options *options, char *err,
                                         size_t errlen) {
    if (!options || (!options->enable_tools && !options->enable_store) ||
        (options->enable_tools && !options->tools) ||
        (options->enable_store && (!options->state_dir || !options->state_dir[0]))) {
        fail(err, errlen, "invalid Cursor callback options");
        return NULL;
    }
    cursor_callbacks *cb = calloc(1, sizeof *cb);
    if (!cb) return NULL;
    buf_init(&cb->reply);
    cb->tools = options->tools;
    cb->tools_started = options->enable_tools;
    cb->store_started = options->enable_store;
    cb->allow_sensitive = options->allow_sensitive_tools;
    cb->thread_create = options->thread_create ? options->thread_create : pthread_create;
    atomic_init(&cb->pump_stop, false);
    atomic_init(&cb->blocking_mode, false);
    if (tny_wake_init(&cb->pump_wake) != 0) {
        fail(err, errlen, "could not initialize callback pump wakeup");
        cursor_callbacks_destroy(&cb);
        return NULL;
    }
    cb->pump_wake_ready = true;
    char token[65];
    if (!random_token(token)) {
        fail(err, errlen, "could not generate callback bearer token");
        cursor_callbacks_destroy(&cb);
        return NULL;
    }
    cb->token = xstrdup(token);
    secure_zero(token, sizeof token);
    if (options->enable_store) {
        cb->store_root = path_join(options->state_dir, "cursor-sdk-store");
        if (!cb->store_root || mkdir_p(cb->store_root) != 0) {
            fail(err, errlen, "could not initialize Cursor callback store");
            cursor_callbacks_destroy(&cb);
            return NULL;
        }
    }
    if (!cb->token) {
        cursor_callbacks_destroy(&cb);
        return NULL;
    }
    cb->server = http_server_start(cb->token, callback_post, cb, err, errlen);
    if (!cb->server) {
        cursor_callbacks_destroy(&cb);
        return NULL;
    }
    return cb;
}

const char *cursor_callbacks_url(const cursor_callbacks *cb) {
    return cb ? http_server_url(cb->server) : NULL;
}
const char *cursor_callbacks_token(const cursor_callbacks *cb) { return cb ? cb->token : NULL; }
bool cursor_callbacks_tools_started(const cursor_callbacks *cb) {
    return cb && cb->server && cb->tools_started;
}
bool cursor_callbacks_store_started(const cursor_callbacks *cb) {
    return cb && cb->server && cb->store_started;
}

char *cursor_callbacks_tool_definitions(cursor_callbacks *cb, const char *configured_json) {
    if (!cb || !cb->tools) return xstrdup("{}");
    yyjson_doc *doc = configured_json ? jparse(configured_json, strlen(configured_json)) : NULL;
    yyjson_val *configured = doc ? yyjson_doc_get_root(doc) : NULL;
    if (configured && !yyjson_is_obj(configured)) configured = NULL;
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{");
    definitions_ctx ctx = {&out, configured, true};
    bool ok = custom_tools_visit(cb->tools, append_definition, &ctx);
    buf_appends(&out, "}");
    yyjson_doc_free(doc);
    if (!ok || buf_oom(&out)) {
        buf_free(&out);
        return NULL;
    }
    return buf_detach(&out);
}

static void pending_dispatch(cursor_callbacks *cb) {
    pending_tool **link = &cb->pending;
    while (*link) {
        pending_tool *pending = *link;
        if (!http_server_request_alive(cb->server, pending->request_id)) {
            custom_tool_invalidate(pending->call);
            *link = pending->next;
            free(pending);
            continue;
        }
        char *result = NULL;
        bool is_error = false;
        int state = custom_tool_take(pending->call, &result, &is_error);
        if (state == 0) {
            link = &pending->next;
            continue;
        }
        if (state > 0) {
            buf_clear(&cb->reply);
            if (append_tool_result(&cb->reply, result, is_error)) {
                http_server_response response = {200, "application/json", cb->reply.data,
                                                 cb->reply.len, pending->request_id};
                (void)http_server_complete(cb->server, pending->request_id, &response);
            } else {
                static const char oom[] = "{\"error\":\"out of memory\"}";
                http_server_response response = {500, "application/json", oom, sizeof oom - 1,
                                                 pending->request_id};
                (void)http_server_complete(cb->server, pending->request_id, &response);
            }
        }
        free(result);
        *link = pending->next;
        free(pending);
    }
}

int cursor_callbacks_pollfds(cursor_callbacks *cb, struct pollfd *fds, int max) {
    if (!cb || !cb->server || !fds || max <= 0 || cb->pump_running) return 0;
    int count = http_server_pollfds(cb->server, fds, max);
    int wake = cb->tools_started ? custom_tools_wake_fd(cb->tools) : -1;
    if (wake >= 0 && count < max) fds[count++] = (struct pollfd){wake, POLLIN, 0};
    return count;
}

int cursor_callbacks_dispatch(cursor_callbacks *cb, const struct pollfd *fds, int n) {
    if (!cb || !cb->server || cb->pump_running) return 0;
    int wake = cb->tools_started ? custom_tools_wake_fd(cb->tools) : -1;
    for (int i = 0; i < n; i++)
        if (fds[i].fd == wake && (fds[i].revents & POLLIN)) custom_tools_wake_drain(cb->tools);
    int rc = http_server_dispatch(cb->server, fds, n);
    pending_dispatch(cb);
    return rc;
}

static short pump_revents(const struct pollfd *fds, int count, int fd) {
    short result = 0;
    for (int i = 0; i < count; i++)
        if (fds[i].fd == fd) result |= fds[i].revents;
    return result;
}

static void *blocking_pump(void *opaque) {
    cursor_callbacks *cb = opaque;
    while (!atomic_load_explicit(&cb->pump_stop, memory_order_acquire)) {
        struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + 1];
        int count = http_server_pollfds(cb->server, fds, HTTP_SERVER_POLLFD_CAPACITY);
        int wake = tny_wake_fd(&cb->pump_wake);
        fds[count++] = (struct pollfd){wake, POLLIN, 0};
        int rc;
        do rc = tny_poll(fds, (nfds_t)count, -1);
        while (rc < 0 && errno == EINTR);
        if (rc < 0) break;
        if (pump_revents(fds, count, wake) & POLLIN) tny_wake_drain(&cb->pump_wake);
        if (atomic_load_explicit(&cb->pump_stop, memory_order_acquire)) break;
        if (http_server_dispatch(cb->server, fds, count) != 0) break;
    }
    return NULL;
}

int cursor_callbacks_blocking_begin(cursor_callbacks *cb, char *err, size_t errlen) {
    if (!cb || !cb->server || cb->pump_running) {
        fail(err, errlen, "callback server is not ready for a blocking bridge RPC");
        return -1;
    }
    atomic_store_explicit(&cb->pump_stop, false, memory_order_release);
    atomic_store_explicit(&cb->blocking_mode, true, memory_order_release);
    if (cb->thread_create(&cb->pump_thread, NULL, blocking_pump, cb) != 0) {
        atomic_store_explicit(&cb->blocking_mode, false, memory_order_release);
        fail(err, errlen, "could not start callback pump thread");
        return -1;
    }
    cb->pump_running = true;
    return 0;
}

void cursor_callbacks_blocking_end(cursor_callbacks *cb) {
    if (!cb || !cb->pump_running) return;
    atomic_store_explicit(&cb->pump_stop, true, memory_order_release);
    tny_wake_signal(&cb->pump_wake);
    (void)pthread_join(cb->pump_thread, NULL);
    cb->pump_running = false;
    atomic_store_explicit(&cb->blocking_mode, false, memory_order_release);
}

void cursor_callbacks_stop(cursor_callbacks *cb) {
    if (!cb) return;
    cursor_callbacks_blocking_end(cb);
    pending_tool *pending = cb->pending;
    cb->pending = NULL;
    while (pending) {
        pending_tool *next = pending->next;
        custom_tool_invalidate(pending->call);
        free(pending);
        pending = next;
    }
    http_server_stop(cb->server);
    cb->tools_started = false;
    cb->store_started = false;
}

void cursor_callbacks_destroy(cursor_callbacks **cbp) {
    if (!cbp || !*cbp) return;
    cursor_callbacks *cb = *cbp;
    cursor_callbacks_stop(cb);
    http_server_destroy(&cb->server);
    if (cb->pump_wake_ready) tny_wake_close(&cb->pump_wake);
    secure_free(cb->token);
    free(cb->store_root);
    buf_free(&cb->reply);
    free(cb);
    *cbp = NULL;
}
