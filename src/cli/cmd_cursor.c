/* cmd_cursor.c -- provider-specific sdk.v1 management and raw RPC access. */
#include "cli/cli.h"

#include "backends/cursor/management.h"
#include "backends/cursor/options.h"
#include "json/json.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CURSOR_COMMAND_TIMEOUT_MS (5 * 60 * 1000)

int cursor_cli_base64_decode_strict(const char *encoded, size_t encoded_len, uint8_t *decoded,
                                    size_t capacity, size_t *decoded_len);

static void cursor_usage(FILE *out) {
    fputs("Usage: tny cursor COMMAND [ARGS]\n"
          "  ping | version | me | models | repositories\n"
          "  create [NAME] | resume AGENT_ID | reload AGENT_ID | close AGENT_ID\n"
          "  send AGENT_ID MESSAGE | wait RUN_ID | run RUN_ID | runs AGENT_ID\n"
          "  conversation RUN_ID | observe RUN_ID [AFTER_OFFSET]\n"
          "  cancel RUN_ID [AGENT_ID]\n"
          "  agent AGENT_ID | agents | archive AGENT_ID | unarchive AGENT_ID\n"
          "  delete AGENT_ID --yes | messages AGENT_ID\n"
          "  artifacts AGENT_ID | download AGENT_ID PATH | usage AGENT_ID [RUN_ID]\n"
          "  rpc SERVICE METHOD [JSON|-] [--yes]\n",
          out);
}

static const char *cursor_key(const tny_ctx *ctx) {
    const char *key = getenv("CURSOR_API_KEY");
    if (key && *key) return key;
    if (ctx->backend == TNY_BK_CURSOR && ctx->api_key && *ctx->api_key) return ctx->api_key;
    return NULL;
}

static int output_json(const char *json) {
    size_t len = strlen(json);
    if (fwrite(json, 1, len, stdout) != len) return -1;
    if ((!len || json[len - 1] != '\n') && putchar('\n') == EOF) return -1;
    return fflush(stdout) == 0 ? 0 : -1;
}

static char *read_stdin_bounded(bool json, char *err, size_t errlen) {
    buf_t input;
    buf_init(&input);
    char chunk[8192];
    while (!feof(stdin)) {
        size_t n = fread(chunk, 1, sizeof chunk, stdin);
        if (n && input.len + n > CURSOR_MAX_MSG_BYTES) {
            snprintf(err, errlen, "cursor: stdin exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
            buf_free(&input);
            return NULL;
        }
        if (n) buf_append(&input, chunk, n);
        if (ferror(stdin)) {
            snprintf(err, errlen, "cursor: could not read stdin");
            buf_free(&input);
            return NULL;
        }
    }
    if (buf_oom(&input)) {
        snprintf(err, errlen, "cursor: out of memory reading stdin");
        buf_free(&input);
        return NULL;
    }
    if (!utf8_valid_bytes(input.data ? input.data : "", input.len)) {
        snprintf(err, errlen, "cursor: stdin is not valid UTF-8");
        buf_free(&input);
        return NULL;
    }
    if (json && cursor_management_validate_json(input.data ? input.data : "", err, errlen) != 0) {
        buf_free(&input);
        return NULL;
    }
    return buf_detach(&input);
}

static char *request_argument(int argc, char **argv, bool required, char *err, size_t errlen) {
    if (argc > 1) {
        snprintf(err, errlen, "cursor: expected one JSON request object");
        return NULL;
    }
    if (argc == 1 && strcmp(argv[0], "-") != 0) {
        if (cursor_management_validate_json(argv[0], err, errlen) != 0) return NULL;
        return xstrdup(argv[0]);
    }
    if (argc == 1 || !isatty(STDIN_FILENO)) return read_stdin_bounded(true, err, errlen);
    if (required) {
        snprintf(err, errlen, "cursor: a JSON request object is required");
        return NULL;
    }
    return xstrdup("{}");
}

static char *text_argument(int argc, char **argv, char *err, size_t errlen) {
    if (argc > 1) {
        snprintf(err, errlen, "cursor: expected one message argument");
        return NULL;
    }
    if (argc == 1 && strcmp(argv[0], "-") != 0) {
        size_t len = strlen(argv[0]);
        if (len > CURSOR_MAX_MSG_BYTES || !utf8_valid_bytes(argv[0], len)) {
            snprintf(err, errlen, "cursor: invalid or oversized message");
            return NULL;
        }
        return xstrdup(argv[0]);
    }
    if (argc == 1 || !isatty(STDIN_FILENO)) return read_stdin_bounded(false, err, errlen);
    snprintf(err, errlen, "cursor: a message argument is required");
    return NULL;
}

static char *catalog_request(const char *key) {
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"options\":{\"apiKey\":");
    jescape(&body, key);
    buf_appends(&body, "}}");
    return buf_oom(&body) ? (buf_free(&body), NULL) : buf_detach(&body);
}

static void append_operation_options(buf_t *body, const tny_ctx *ctx, const char *key) {
    buf_appends(body, "{\"cwd\":");
    jescape(body, ctx->cwd);
    buf_appends(body, ",\"apiKey\":");
    jescape(body, key);
    buf_appends(body, "}");
}

static char *id_request(const char *field, const char *id) {
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{");
    jescape(&body, field);
    buf_appends(&body, ":");
    jescape(&body, id);
    buf_appends(&body, "}");
    return buf_oom(&body) ? (buf_free(&body), NULL) : buf_detach(&body);
}

static char *operation_request(const tny_ctx *ctx, const char *key, const char *agent_id) {
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"agentId\":");
    jescape(&body, agent_id);
    buf_appends(&body, ",\"options\":");
    append_operation_options(&body, ctx, key);
    buf_appends(&body, "}");
    return buf_oom(&body) ? (buf_free(&body), NULL) : buf_detach(&body);
}

static int print_frame(uint8_t flags, const char *payload, size_t len, void *ud, char *err,
                       size_t errlen) {
    (void)flags;
    (void)ud;
    if (!len) return 0;
    if (fwrite(payload, 1, len, stdout) != len || putchar('\n') == EOF || fflush(stdout) != 0) {
        snprintf(err, errlen, "cursor: could not write stream output");
        return -1;
    }
    return 0;
}

static int base64_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

/* Internal test seam: strict RFC 4648 base64, including canonical tail bits.
 * Returns 0 and writes the decoded length, or -1 without partial success. */
int cursor_cli_base64_decode_strict(const char *encoded, size_t encoded_len, uint8_t *decoded,
                                    size_t capacity, size_t *decoded_len) {
    if (!encoded || !decoded_len || (encoded_len && !decoded) || encoded_len % 4u != 0) return -1;
    size_t padding = 0;
    if (encoded_len && encoded[encoded_len - 1] == '=') padding++;
    if (encoded_len > 1 && encoded[encoded_len - 2] == '=') padding++;
    size_t needed = (encoded_len / 4u) * 3u - padding;
    if (needed > capacity) return -1;
    size_t written = 0;
    for (size_t i = 0; i < encoded_len; i += 4u) {
        bool last = i + 4u == encoded_len;
        int a = base64_value((unsigned char)encoded[i]);
        int b = base64_value((unsigned char)encoded[i + 1u]);
        int c = encoded[i + 2u] == '=' ? -2 : base64_value((unsigned char)encoded[i + 2u]);
        int d = encoded[i + 3u] == '=' ? -2 : base64_value((unsigned char)encoded[i + 3u]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (!last && (c == -2 || d == -2)) ||
            (c == -2 && d != -2) || (c == -2 && (b & 0x0f) != 0) ||
            (d == -2 && c >= 0 && (c & 0x03) != 0))
            return -1;
        decoded[written++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0) {
            decoded[written++] = (uint8_t)((b << 4) | (c >> 2));
            if (d >= 0) decoded[written++] = (uint8_t)((c << 6) | d);
        }
    }
    *decoded_len = written;
    return 0;
}

int cursor_cli_artifact_frame(uint8_t flags, const char *payload, size_t len, void *ud, char *err,
                              size_t errlen) {
    (void)flags;
    if (!len) return 0;
    cursor_artifact_output *output = ud;
    yyjson_doc *doc = jparse(payload, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *data = jget(root, "data");
    const char *encoded = yyjson_is_str(data) ? yyjson_get_str(data) : NULL;
    if (!yyjson_is_obj(root) || !encoded) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: malformed artifact chunk");
        return -1;
    }
    size_t encoded_len = yyjson_get_len(data);
    size_t capacity = (encoded_len / 4u) * 3u + 3u;
    if (capacity > CURSOR_MAX_MSG_BYTES) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: artifact exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
        return -1;
    }
    uint8_t *decoded = malloc(capacity ? capacity : 1u);
    size_t decoded_len = 0;
    if (cursor_cli_base64_decode_strict(encoded, encoded_len, decoded, capacity, &decoded_len) !=
        0) {
        free(decoded);
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: artifact chunk is not valid base64");
        return -1;
    }
    if (output->total > CURSOR_MAX_MSG_BYTES - decoded_len) {
        free(decoded);
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: artifact exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
        return -1;
    }
    if (decoded_len && fwrite(decoded, 1, decoded_len, output->stream) != decoded_len) {
        free(decoded);
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: could not write artifact");
        return -1;
    }
    output->total += decoded_len;
    free(decoded);
    yyjson_doc_free(doc);
    return 0;
}

static int invoke(cursor_management *management, const cursor_sdk_route *route, const char *body,
                  cursor_management_frame_cb callback, void *ud, char *err, size_t errlen) {
    if (route->kind == CURSOR_SDK_SERVER_STREAM)
        return cursor_management_stream(management, route->id, body, CURSOR_COMMAND_TIMEOUT_MS,
                                        callback ? callback : print_frame, ud, err, errlen);
    char *response = cursor_management_unary(management, route->id, body, CURSOR_COMMAND_TIMEOUT_MS,
                                             err, errlen);
    if (!response) return -1;
    int rc = output_json(response);
    free(response);
    if (rc != 0) snprintf(err, errlen, "cursor: could not write JSON output");
    return rc;
}

static char *first_model(cursor_management *management, const tny_ctx *ctx, const char *key,
                         char *err, size_t errlen) {
    if (ctx->model && *ctx->model) return xstrdup(ctx->model);
    char *request = catalog_request(key);
    char *response = request
                         ? cursor_management_unary(management, CURSOR_SDK_RPC_LIST_MODELS, request,
                                                   CURSOR_COMMAND_TIMEOUT_MS, err, errlen)
                         : NULL;
    free(request);
    if (!response) return NULL;
    yyjson_doc *doc = jparse(response, strlen(response));
    yyjson_val *items = doc ? jget(yyjson_doc_get_root(doc), "items") : NULL;
    yyjson_val *first = yyjson_is_arr(items) ? yyjson_arr_get_first(items) : NULL;
    const char *id = jget_str(first, "id");
    char *model = id && *id ? xstrdup(id) : NULL;
    yyjson_doc_free(doc);
    free(response);
    if (!model) snprintf(err, errlen, "cursor: ListModels returned no usable model");
    return model;
}

static char *agent_options_request(cursor_management *management, const tny_ctx *ctx,
                                   const char *key, const char *agent_id, const char *name,
                                   char *err, size_t errlen) {
    char *options = cursor_options_agent_json(ctx, key, NULL, err, errlen);
    yyjson_doc *check = options ? jparse(options, strlen(options)) : NULL;
    yyjson_val *selected = check ? jget(yyjson_doc_get_root(check), "model") : NULL;
    const char *selected_id = jget_str(selected, "id");
    bool has_model = selected_id && *selected_id;
    yyjson_doc_free(check);
    if (!options) return NULL;
    if (!has_model) {
        char *model = first_model(management, ctx, key, err, errlen);
        if (!model) {
            free(options);
            return NULL;
        }
        tny_ctx view = *ctx;
        view.model = model;
        free(options);
        options = cursor_options_agent_json(&view, key, NULL, err, errlen);
        free(model);
        if (!options) return NULL;
    }
    if (name && *name) {
        yyjson_doc *parsed = jparse(options, strlen(options));
        yyjson_mut_doc *mut = parsed ? yyjson_doc_mut_copy(parsed, jallocator()) : NULL;
        yyjson_doc_free(parsed);
        if (!mut) {
            free(options);
            snprintf(err, errlen, "cursor: could not add agent name");
            return NULL;
        }
        yyjson_mut_obj_put(yyjson_mut_doc_get_root(mut), yyjson_mut_strcpy(mut, "name"),
                           yyjson_mut_strcpy(mut, name));
        free(options);
        options = jwrite(mut);
        yyjson_mut_doc_free(mut);
        if (!options) {
            snprintf(err, errlen, "cursor: could not serialize named agent options");
            return NULL;
        }
    }
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{");
    if (agent_id) {
        buf_appends(&body, "\"agentId\":");
        jescape(&body, agent_id);
        buf_appends(&body, ",");
    }
    buf_appends(&body, "\"options\":");
    buf_appends(&body, options);
    buf_appends(&body, "}");
    free(options);
    return buf_oom(&body) ? (buf_free(&body), NULL) : buf_detach(&body);
}

static int raw_rpc(cursor_management *management, int argc, char **argv, char *err, size_t errlen) {
    if (argc < 2 || argc > 4) {
        snprintf(err, errlen, "cursor: rpc needs SERVICE METHOD [JSON|-] [--yes]");
        return -1;
    }
    const cursor_sdk_route *route = cursor_management_route(argv[0], argv[1]);
    if (!route) {
        snprintf(err, errlen, "cursor: unknown sdk.v1 route %s/%s", argv[0], argv[1]);
        return -1;
    }
    bool yes = argc >= 3 && strcmp(argv[argc - 1], "--yes") == 0;
    int json_argc = argc - 2 - (yes ? 1 : 0);
    if (route->id == CURSOR_SDK_RPC_DELETE_AGENT && !yes) {
        snprintf(err, errlen, "cursor: DeleteAgent requires --yes");
        return -1;
    }
    if (route->id != CURSOR_SDK_RPC_DELETE_AGENT && yes) {
        snprintf(err, errlen, "cursor: --yes is only valid for DeleteAgent");
        return -1;
    }
    char *body = request_argument(json_argc, argv + 2, false, err, errlen);
    if (!body) return -1;
    int rc = invoke(management, route, body, NULL, NULL, err, errlen);
    free(body);
    return rc;
}

static int simple_alias(cursor_management *management, tny_ctx *ctx, const char *key, int argc,
                        char **argv, char *err, size_t errlen) {
    const char *command = argv[0];
    cursor_sdk_rpc_id id;
    char *body = NULL;
    cursor_management_frame_cb callback = NULL;
    cursor_artifact_output artifact;
    bool download = false;

    if (strcmp(command, "ping") == 0 || strcmp(command, "version") == 0) {
        if (argc != 1) goto usage;
        id = strcmp(command, "ping") == 0 ? CURSOR_SDK_RPC_PING : CURSOR_SDK_RPC_GET_VERSION;
        body = xstrdup("{}");
    } else if (strcmp(command, "me") == 0 || strcmp(command, "models") == 0 ||
               strcmp(command, "repositories") == 0) {
        if (argc != 1) goto usage;
        id = strcmp(command, "me") == 0       ? CURSOR_SDK_RPC_ME
             : strcmp(command, "models") == 0 ? CURSOR_SDK_RPC_LIST_MODELS
                                              : CURSOR_SDK_RPC_LIST_REPOSITORIES;
        body = catalog_request(key);
    } else if (strcmp(command, "create") == 0) {
        if (argc > 2) goto usage;
        id = CURSOR_SDK_RPC_CREATE_AGENT;
        body = agent_options_request(management, ctx, key, NULL, argc == 2 ? argv[1] : NULL, err,
                                     errlen);
    } else if (strcmp(command, "resume") == 0) {
        if (argc != 2) goto usage;
        id = CURSOR_SDK_RPC_RESUME_AGENT;
        body = agent_options_request(management, ctx, key, argv[1], NULL, err, errlen);
    } else if (strcmp(command, "reload") == 0 || strcmp(command, "close") == 0) {
        if (argc != 2) goto usage;
        id = strcmp(command, "reload") == 0 ? CURSOR_SDK_RPC_RELOAD_AGENT
                                            : CURSOR_SDK_RPC_CLOSE_AGENT;
        body = id_request("agentId", argv[1]);
    } else if (strcmp(command, "send") == 0) {
        if (argc < 2 || argc > 3) goto usage;
        char *message = text_argument(argc - 2, argv + 2, err, errlen);
        if (!message) return -1;
        char *options = cursor_options_send_json(ctx, true, err, errlen);
        if (!options) {
            free(message);
            return -1;
        }
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"agentId\":");
        jescape(&request, argv[1]);
        buf_appends(&request, ",\"message\":{\"text\":");
        jescape(&request, message);
        buf_appends(&request, "},\"options\":");
        buf_appends(&request, options);
        buf_appends(&request, "}");
        free(message);
        free(options);
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
        id = CURSOR_SDK_RPC_SEND;
    } else if (strcmp(command, "wait") == 0 || strcmp(command, "run") == 0 ||
               strcmp(command, "conversation") == 0) {
        if (argc != 2) goto usage;
        id = strcmp(command, "wait") == 0  ? CURSOR_SDK_RPC_WAIT_LIVE_RUN
             : strcmp(command, "run") == 0 ? CURSOR_SDK_RPC_GET_RUN
                                           : CURSOR_SDK_RPC_GET_RUN_CONVERSATION;
        if (id == CURSOR_SDK_RPC_GET_RUN) {
            buf_t request;
            buf_init(&request);
            buf_appends(&request, "{\"runId\":");
            jescape(&request, argv[1]);
            buf_appends(&request, ",\"options\":");
            append_operation_options(&request, ctx, key);
            buf_appends(&request, "}");
            body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
        } else body = id_request("runId", argv[1]);
    } else if (strcmp(command, "runs") == 0 || strcmp(command, "agent") == 0 ||
               strcmp(command, "archive") == 0 || strcmp(command, "unarchive") == 0 ||
               strcmp(command, "messages") == 0) {
        if (argc != 2) goto usage;
        id = strcmp(command, "runs") == 0        ? CURSOR_SDK_RPC_LIST_RUNS
             : strcmp(command, "agent") == 0     ? CURSOR_SDK_RPC_GET_AGENT
             : strcmp(command, "archive") == 0   ? CURSOR_SDK_RPC_ARCHIVE_AGENT
             : strcmp(command, "unarchive") == 0 ? CURSOR_SDK_RPC_UNARCHIVE_AGENT
                                                 : CURSOR_SDK_RPC_LIST_AGENT_MESSAGES;
        body = operation_request(ctx, key, argv[1]);
    } else if (strcmp(command, "agents") == 0) {
        if (argc != 1) goto usage;
        id = CURSOR_SDK_RPC_LIST_AGENTS;
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"options\":");
        append_operation_options(&request, ctx, key);
        buf_appends(&request, "}");
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
    } else if (strcmp(command, "observe") == 0) {
        if (argc < 2 || argc > 3) goto usage;
        id = CURSOR_SDK_RPC_OBSERVE_RUN;
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"runId\":");
        jescape(&request, argv[1]);
        if (argc == 3) {
            buf_appends(&request, ",\"afterOffset\":");
            jescape(&request, argv[2]);
        }
        buf_appends(&request, "}");
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
        callback = print_frame;
    } else if (strcmp(command, "cancel") == 0) {
        if (argc < 2 || argc > 3) goto usage;
        id = CURSOR_SDK_RPC_CANCEL_RUN;
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"runId\":");
        jescape(&request, argv[1]);
        if (argc == 3) {
            buf_appends(&request, ",\"agentId\":");
            jescape(&request, argv[2]);
        }
        buf_appends(&request, "}");
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
    } else if (strcmp(command, "delete") == 0) {
        if (argc != 3 || strcmp(argv[2], "--yes") != 0) {
            snprintf(err, errlen, "cursor: delete requires AGENT_ID --yes");
            return -1;
        }
        id = CURSOR_SDK_RPC_DELETE_AGENT;
        body = operation_request(ctx, key, argv[1]);
    } else if (strcmp(command, "artifacts") == 0) {
        if (argc != 2) goto usage;
        id = CURSOR_SDK_RPC_LIST_ARTIFACTS;
        body = id_request("agentId", argv[1]);
    } else if (strcmp(command, "download") == 0) {
        if (argc != 3) goto usage;
        id = CURSOR_SDK_RPC_DOWNLOAD_ARTIFACT;
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"agentId\":");
        jescape(&request, argv[1]);
        buf_appends(&request, ",\"path\":");
        jescape(&request, argv[2]);
        buf_appends(&request, "}");
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
        artifact = (cursor_artifact_output){stdout, 0};
        callback = cursor_cli_artifact_frame;
        download = true;
    } else if (strcmp(command, "usage") == 0) {
        if (argc < 2 || argc > 3) goto usage;
        id = CURSOR_SDK_RPC_GET_USAGE;
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"agentId\":");
        jescape(&request, argv[1]);
        if (argc == 3) {
            buf_appends(&request, ",\"runId\":");
            jescape(&request, argv[2]);
        }
        buf_appends(&request, "}");
        body = buf_oom(&request) ? (buf_free(&request), NULL) : buf_detach(&request);
    } else {
        goto usage;
    }

    if (!body) {
        snprintf(err, errlen, "cursor: out of memory building request");
        return -1;
    }
    const cursor_sdk_route *route = cursor_sdk_route_by_id(id);
    int rc = invoke(management, route, body, callback, download ? &artifact : NULL, err, errlen);
    free(body);
    if (download && rc == 0 && fflush(stdout) != 0) {
        snprintf(err, errlen, "cursor: could not flush artifact");
        rc = -1;
    }
    return rc;

usage:
    snprintf(err, errlen, "cursor: invalid arguments for '%s'", command);
    return -1;
}

int cmd_cursor(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    if (argc == 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        cursor_usage(argc == 0 ? stderr : stdout);
        return argc == 0 ? 1 : 0;
    }
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "tny: cursor: sdk.v1 management is unavailable in WebAssembly\n");
    return 1;
#endif
    const char *key = cursor_key(ctx);
    if (!key) {
        fprintf(stderr, "tny: cursor: CURSOR_API_KEY is not set\n");
        return 1;
    }

    /* Reject destructive/raw syntax before starting a host process. */
    char err[512];
    const cursor_sdk_route *raw_route = NULL;
    if (strcmp(argv[0], "rpc") == 0 && argc >= 3)
        raw_route = cursor_management_route(argv[1], argv[2]);
    if (strcmp(argv[0], "delete") == 0 && (argc != 3 || strcmp(argv[2], "--yes") != 0)) {
        fprintf(stderr, "tny: cursor: delete requires AGENT_ID --yes\n");
        return 1;
    }
    if (raw_route && raw_route->id == CURSOR_SDK_RPC_DELETE_AGENT &&
        strcmp(argv[argc - 1], "--yes") != 0) {
        fprintf(stderr, "tny: cursor: DeleteAgent requires --yes\n");
        return 1;
    }

    int ready_timeout_ms;
    if (cursor_bridge_ready_timeout_ms(getenv(CURSOR_BRIDGE_READY_TIMEOUT_ENV), &ready_timeout_ms,
                                       err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        return 1;
    }

    cursor_management management;
    if (cursor_management_open(&management, ctx, ready_timeout_ms, err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        return 1;
    }
    int rc;
    if (strcmp(argv[0], "rpc") == 0) rc = raw_rpc(&management, argc - 1, argv + 1, err, sizeof err);
    else rc = simple_alias(&management, ctx, key, argc, argv, err, sizeof err);
    cursor_management_close(&management);
    if (rc != 0) {
        fprintf(stderr, "tny: %s\n", err);
        if (str_starts(err, "cursor: invalid arguments")) cursor_usage(stderr);
        return 1;
    }
    return 0;
}
