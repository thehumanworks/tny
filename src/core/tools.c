/* tools.c — registry, permission gate, dispatch, result bounding. */
#include "core/tools.h"
#include "core/image.h"
#include "util/image_io.h"
#include "core/speech.h"
#include "core/image_service.h"
#include "core/tools_image.h"
#include "core/tools_jobs.h"
#include "core/intercept.h"
#include "core/subagent.h"
#include "lib/custom_tools.h"
#include "util/alloc.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

char *tool_err(const char *fmt, ...) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "error: ");
    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_appends(&b, line);
    return buf_detach(&b);
}

char *tool_resolve_path(tools_env *env, const char *path, char **err_out) {
    *err_out = NULL;
    if (!path || !*path) {
        *err_out = tool_err("missing path");
        return NULL;
    }
    char *expanded = NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        char *home = path_home();
        expanded = path[1] == '/' ? path_join(home, path + 2) : xstrdup(home);
        free(home);
        path = expanded;
    }
    char *abs;
    if (path[0] == '/') abs = path_abs(path);
    else {
        char *joined = path_join(env->ctx->cwd, path);
        abs = path_abs(joined);
        if (!abs) abs = joined;
        else free(joined);
    }
    if (!abs) {
        *err_out = tool_err("cannot resolve path %s", path);
        free(expanded);
        return NULL;
    }
    free(expanded);
    return abs;
}

char *tool_bound_result(tools_env *env, const char *data, size_t len) {
    size_t maxb = env->ctx->max_tool_result_bytes;
    if (len <= maxb) return xstrndup(data, len);
    char *handle = env->session ? session_store_result(env->session, data, len) : NULL;
    buf_t b;
    buf_init(&b);
    buf_append(&b, data, maxb / 2);
    buf_appendf(&b, "\n…[truncated: %zu of %zu bytes shown]", maxb / 2, len);
    if (handle)
        buf_appendf(&b,
                    "\nFull output stored as handle \"%s\" — read more with "
                    "read_tool_result(handle, offset, length).",
                    handle);
    free(handle);
    return buf_detach(&b);
}

/* ---- schema ----
 * Compact hand-written JSON; one entry per tool. */
static const char *SCHEMA_JSON =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"list_files\",\"description\":\"List directory "
    "entries. Args: path (default workspace "
    "root).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}"
    "},"
    "{\"type\":\"function\",\"function\":{\"name\":\"glob_files\",\"description\":\"Find files "
    "matching a glob pattern (e.g. src/**/*.c) under the "
    "workspace.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":"
    "\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"grep_files\",\"description\":\"Search file "
    "contents for a substring or simple pattern. Returns matching lines with file:line "
    "prefixes.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":"
    "\"string\"},\"path\":{\"type\":\"string\"},\"case_insensitive\":{\"type\":\"boolean\"}},"
    "\"required\":[\"pattern\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a text "
    "file. Args: path, optional offset (line), limit (lines). For png/jpeg/gif/webp use "
    "read_image.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
    "\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":["
    "\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_image\",\"description\":\"View an image "
    "file (png/jpeg/gif/webp). The pixels are shown to you on the next turn. Args: "
    "path.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or "
    "overwrite a file with the given "
    "content.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit_file\",\"description\":\"Replace an "
    "exact substring in a file once (or all occurrences with "
    "replace_all).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
    "\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},"
    "\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}"
    "}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"delete_file\",\"description\":\"Delete a "
    "file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"rename_file\",\"description\":\"Rename or "
    "move a "
    "file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"new_path\":{\"type\":\"string\"}},\"required\":[\"path\",\"new_path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"copy_file\",\"description\":\"Copy a "
    "file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"new_path\":{\"type\":\"string\"}},\"required\":[\"path\",\"new_path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"create_folder\",\"description\":\"Create a "
    "directory (with "
    "parents).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}"
    "},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"file_info\",\"description\":\"Stat a path: "
    "size, type, "
    "mtime.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"semantic_search\",\"description\":\"Lexical "
    "relevance search over workspace files for a natural-language query. Returns the best-matching "
    "files and "
    "lines.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},"
    "\"required\":[\"query\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"open_file\",\"description\":\"Open a file or "
    "URL with the OS default "
    "handler.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}"
    ",\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"terminal\",\"description\":\"Run a shell "
    "command in the workspace. Args: command, optional timeout_s (default 120), background "
    "(returns immediately with a log "
    "path).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}"
    ",\"timeout_s\":{\"type\":\"integer\"},\"background\":{\"type\":\"boolean\"}},\"required\":["
    "\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"web_fetch\",\"description\":\"HTTP GET a URL "
    "and return the (bounded) body "
    "text.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"description\":\"Search the web "
    "(requires a configured search "
    "provider).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":"
    "\"string\"}},\"required\":[\"query\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"memory\",\"description\":\"Persist or recall "
    "a user-level note. Args: action get|set|list, key, "
    "value.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},"
    "\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"action\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_tool_result\",\"description\":\"Read a "
    "byte range of a stored large tool result. Args: handle, offset, "
    "length.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"handle\":{\"type\":\"string\"}"
    ",\"offset\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}},\"required\":["
    "\"handle\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"skill\",\"description\":\"Load a skill body "
    "by name (discovered from SKILL.md "
    "files).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"required\":[\"name\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"install_skill\",\"description\":\"Install a "
    "skill directory into "
    "~/.tny/"
    "skills.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"subagent\",\"description\":\"Run a durable "
    "child agent in its own tny session. create: {action, prompt}; omit id, the result returns "
    "the child id. message: {action, id, prompt} continues that child. inspect and lifecycle: "
    "{action, id} read its stored state.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"message\",\"inspect\",\"lifecycle\"]},"
    "\"id\":{\"type\":\"string\",\"description\":\"Child id returned by create; never set on "
    "create.\"},\"prompt\":{\"type\":\"string\"}},\"required\":[\"action\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"job_submit\",\"description\":\"Submit durable "
    "ask or image work that keeps running after this turn. One item, or a bounded batch of 1-64 "
    "items of the same kind with concurrency 1-16. Returns the job id, its metadata path and the "
    "per-item log paths immediately; read it back with "
    "job_status.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":"
    "\"string\",\"enum\":[\"ask\",\"image\"]},\"concurrency\":{\"type\":\"integer\",\"description\""
    ":\"Items running at once (1-16, default 2).\"},\"items\":{\"type\":\"array\",\"minItems\":1,"
    "\"maxItems\":64,\"items\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":"
    "\"string\"},\"model\":{\"type\":\"string\"},\"effort\":{\"type\":\"string\"},\"task\":{"
    "\"type\":\"string\"},\"operation\":{\"type\":\"string\",\"enum\":[\"generate\",\"edit\"]},"
    "\"output_file\":{\"type\":\"string\",\"description\":\"Image destination; must not exist "
    "unless overwrite is true.\"},\"quality\":{\"type\":\"string\"},\"size\":{\"type\":\"string\"},"
    "\"persist_manifest\":{\"type\":\"boolean\",\"description\":\"Image items: false omits the "
    "generation manifest; default true.\"},"
    "\"strict_size\":{\"type\":\"boolean\"},\"overwrite\":{\"type\":\"boolean\"},\"images\":{"
    "\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":5},\"persist_request\":{"
    "\"type\":\"boolean\",\"description\":\"false keeps the prompt and references out of the job "
    "record; retrying then needs a new submission.\"}},\"required\":[\"prompt\"]}}},\"required\":["
    "\"kind\",\"items\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"job_control\",\"description\":\"Change a "
    "durable job: cancel selected or all items, retry failed items (successful items are never "
    "re-executed), or remove a finished job's "
    "record.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\","
    "\"enum\":[\"cancel\",\"retry\",\"rm\"]},\"id\":{\"type\":\"string\",\"description\":\"The "
    "32-character job id.\"},\"items\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},"
    "\"description\":\"Item indexes; omit for every applicable item.\"},\"failed\":{\"type\":"
    "\"boolean\",\"description\":\"retry: select every failed, cancelled or interrupted "
    "item.\"}},\"required\":[\"action\",\"id\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"job_status\",\"description\":\"Read durable "
    "jobs: status of one job, wait for it with a timeout, read an item's bounded log tail, or list "
    "jobs. Read-only: it can never submit, cancel or "
    "retry.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\","
    "\"enum\":[\"status\",\"wait\",\"logs\",\"list\"]},\"id\":{\"type\":\"string\"},\"item\":{"
    "\"type\":\"integer\"},\"max_bytes\":{\"type\":\"integer\"},\"timeout_s\":{\"type\":"
    "\"integer\"}},\"required\":[\"action\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"mcp_search_tools\",\"description\":\"Search "
    "configured MCP servers for tools. Space-separated keywords must all match a tool's name or "
    "description; an empty query lists the cached "
    "catalog.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":"
    "\"string\"}}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"mcp_select_tool\",\"description\":\"Select an "
    "MCP tool by server and name, then call it with JSON "
    "arguments.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"server\":{\"type\":"
    "\"string\"},\"tool\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":["
    "\"server\",\"tool\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"mcp_features\",\"description\":\"List "
    "configured MCP servers and their advertised "
    "capabilities.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"image_preview\",\"description\":\"Preview one "
    "successful manifest or job/item artifact without generating or "
    "converting.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"manifest\":{\"type\":"
    "\"string\"},\"job\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{32}$\"},\"item\":{\"type\":"
    "\"integer\",\"minimum\":0,\"maximum\":63}},\"oneOf\":[{\"required\":[\"manifest\"],\"not\":{"
    "\"anyOf\":[{\"required\":[\"job\"]},{\"required\":[\"item\"]}]}},{\"required\":[\"job\","
    "\"item\"],\"not\":{\"required\":[\"manifest\"]}}],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"image_generate\",\"description\":\"Generate "
    "an image from a prompt. Uses the selected image provider (codex default: ChatGPT allowance). "
    "Saves one image and returns metadata. "
    "Use read_image to inspect the saved "
    "result.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\","
    "\"description\":\"Required unless from_manifest reuses a recorded prompt.\"}"
    ",\"output_file\":{\"type\":\"string\",\"description\":\"Required local output path; "
    "atomically replaces an existing file. Result reports actual MIME type and the width/height "
    "read from the returned bytes.\"},\"provider\":{\"type\":\"string\",\"description\":\"Image "
    "provider independent of "
    "conversation provider; default "
    "codex.\"},\"model\":{\"type\":\"string\",\"description\":\"Optional image model; codex "
    "default "
    "gpt-image-2.5-sunburst; override e.g. "
    "gpt-image-2.5-flare.\"},\"quality\":{\"type\":\"string\","
    "\"description\":\"Image quality; codex default high.\","
    "\"enum\":[\"auto\",\"low\",\"medium\",\"high\",\"xhigh\",\"max\"]},"
    "\"size\":{\"type\":\"string\",\"description\":\"Provider size, e.g. 1024x1024; "
    "default auto.\"},"
    "\"strict_size\":{\"type\":\"boolean\",\"description\":\"Fail instead of saving when the "
    "returned image is not exactly the requested WIDTHxHEIGHT; needs an exact size, never "
    "auto.\"},"
    "\"preview\":{\"type\":\"boolean\",\"description\":\"Explicitly request original-byte "
    "conversation preview; default false.\"},"
    "\"persist_manifest\":{\"type\":\"boolean\",\"description\":\"Write the private per-operation "
    "manifest beside the output recording prompt, references and settings; default true. False "
    "records nothing and leaves no rerunnable history.\"},"
    "\"from_manifest\":{\"type\":\"string\",\"description\":\"Path of an earlier manifest to "
    "rerun. Its prompt and settings are reused unless given here; it must record a generate.\"}},"
    "\"required\":[\"output_file\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"image_edit\",\"description\":\"Edit images "
    "using a prompt and reference images. Uses the selected image provider (codex default: ChatGPT "
    "allowance). Saves one image and "
    "returns metadata. Use read_image to inspect the saved "
    "result.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\","
    "\"description\":\"Required unless from_manifest reuses a recorded prompt.\"}"
    ",\"output_file\":{\"type\":\"string\",\"description\":\"Required local output path; "
    "atomically replaces an existing file. Result reports actual MIME type and the width/height "
    "read from the returned bytes.\"},\"provider\":{\"type\":\"string\",\"description\":\"Image "
    "provider independent of "
    "conversation provider; default "
    "codex.\"},\"model\":{\"type\":\"string\",\"description\":\"Optional image model; codex "
    "default "
    "gpt-image-2.5-sunburst; override e.g. "
    "gpt-image-2.5-flare.\"},\"quality\":{\"type\":\"string\","
    "\"description\":\"Image quality; codex default high.\","
    "\"enum\":[\"auto\",\"low\",\"medium\",\"high\",\"xhigh\",\"max\"]},"
    "\"size\":{\"type\":\"string\",\"description\":\"Provider size, e.g. 1024x1024; "
    "default "
    "auto.\"},"
    "\"strict_size\":{\"type\":\"boolean\",\"description\":\"Fail instead of saving when the "
    "returned image is not exactly the requested WIDTHxHEIGHT; needs an exact size, never "
    "auto.\"},"
    "\"preview\":{\"type\":\"boolean\",\"description\":\"Explicitly request original-byte "
    "conversation preview; default false.\"},"
    "\"persist_manifest\":{\"type\":\"boolean\",\"description\":\"Write the private per-operation "
    "manifest beside the output recording prompt, references and settings; default true. False "
    "records nothing and leaves no rerunnable history.\"},"
    "\"from_manifest\":{\"type\":\"string\",\"description\":\"Path of an earlier manifest to "
    "rerun. Its prompt, references and settings are reused unless given here; it must record an "
    "edit and excludes artifact and images.\"},"
    "\"artifact\":{\"type\":\"string\",\"description\":\"Path of an earlier manifest whose "
    "verified output becomes the first reference, ahead of any images.\"},"
    "\"job\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{32}$\",\"description\":\"Succeeded image "
    "job; item required. Adds one final reference after artifact and "
    "images.\"},\"item\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":63},"
    "\"images\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1,"
    "\"maxItems\":5,\"description\":\"Local PNG/JPEG/WebP reference paths uploaded to the image "
    "provider; each at most 8 MiB. Required unless artifact or from_manifest supplies "
    "them.\"}},\"required\":[\"output_file\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"image_export\",\"description\":\"Resize, "
    "crop, pad or re-encode one local image into a new file, using the optional ImageMagick 7 "
    "`magick` executable. No provider is called and nothing is generated: the output is derived "
    "from bytes that already exist, its canvas is exactly the requested size, and the source file "
    "is never modified.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"sources\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":1,\"description\":\"Exactly one "
    "entry: {\\\"image\\\": PATH} for a local PNG/JPEG/WebP file (at most 8 MiB), or "
    "{\\\"artifact\\\": RECORD} for an earlier manifest's verified output.\"},"
    "\"output_file\":{\"type\":\"string\",\"description\":\"Required local destination; fails if "
    "it already exists unless overwrite is true.\"},"
    "\"size\":{\"type\":\"string\",\"description\":\"Required exact canvas WIDTHxHEIGHT, each "
    "edge 1-16384 and at most 64M pixels.\"},"
    "\"fit\":{\"type\":\"string\",\"description\":\"fit contains then pads, crop covers then "
    "crops at the gravity, pad never enlarges; default fit.\",\"enum\":[\"fit\",\"crop\",\"pad\"]},"
    "\"gravity\":{\"type\":\"string\",\"description\":\"Where content sits in the canvas, and "
    "which part a crop keeps; default center.\",\"enum\":[\"center\",\"north\",\"south\",\"east\","
    "\"west\",\"northeast\",\"northwest\",\"southeast\",\"southwest\"]},"
    "\"background\":{\"type\":\"string\",\"description\":\"transparent, #RRGGBB or #RRGGBBAA for "
    "padding; default transparent, and black for JPEG, which has no alpha.\"},"
    "\"format\":{\"type\":\"string\",\"description\":\"Output encoding; default png. Never "
    "guessed from the destination's extension.\",\"enum\":[\"png\",\"jpeg\",\"webp\"]},"
    "\"overwrite\":{\"type\":\"boolean\",\"description\":\"Replace an existing regular "
    "destination file; default false.\"},"
    "\"preview\":{\"type\":\"boolean\",\"description\":\"Explicitly request original-byte "
    "conversation preview; default false.\"},"
    "\"persist_manifest\":{\"type\":\"boolean\",\"description\":\"Write the private manifest "
    "beside the output recording the transform and its source hashes; default true.\"}},"
    "\"required\":[\"sources\",\"output_file\",\"size\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"image_contact_sheet\",\"description\":"
    "\"Compose 1-64 local images into one ordered grid image, using the optional ImageMagick 7 "
    "`magick` executable. No provider is called: the sheet is derived from bytes that already "
    "exist, the canvas is exactly the requested size, and no source file is "
    "modified.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"sources\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":64,\"description\":\"Ordered "
    "entries, each {\\\"image\\\": PATH} or {\\\"artifact\\\": RECORD}. Cell order is this "
    "order.\"},"
    "\"output_file\":{\"type\":\"string\",\"description\":\"Required local destination; fails if "
    "it already exists unless overwrite is true.\"},"
    "\"size\":{\"type\":\"string\",\"description\":\"Required exact canvas WIDTHxHEIGHT, each "
    "edge 1-16384 and at most 64M pixels.\"},"
    "\"columns\":{\"type\":\"integer\",\"description\":\"Grid columns, 1..the number of sources; "
    "default ceil(sqrt(sources)). Rows follow, cells are floor(width/columns) by "
    "floor(height/rows), and the unused remainder stays background.\"},"
    "\"labels\":{\"type\":\"string\",\"description\":\"Cell labels: none, or fixed numeric "
    "bitmaps numbering the sources 1..N in order; default none. No font and no free "
    "text.\",\"enum\":[\"none\",\"numbers\"]},"
    "\"fit\":{\"type\":\"string\",\"description\":\"How each source fills its cell; default "
    "fit.\",\"enum\":[\"fit\",\"crop\",\"pad\"]},"
    "\"gravity\":{\"type\":\"string\",\"description\":\"Placement inside each cell; default "
    "center.\",\"enum\":[\"center\",\"north\",\"south\",\"east\",\"west\",\"northeast\","
    "\"northwest\",\"southeast\",\"southwest\"]},"
    "\"background\":{\"type\":\"string\",\"description\":\"transparent, #RRGGBB or #RRGGBBAA for "
    "padding and the unused remainder; default transparent, and black for JPEG.\"},"
    "\"format\":{\"type\":\"string\",\"description\":\"Output encoding; default "
    "png.\",\"enum\":[\"png\",\"jpeg\",\"webp\"]},"
    "\"overwrite\":{\"type\":\"boolean\",\"description\":\"Replace an existing regular "
    "destination file; default false.\"},"
    "\"preview\":{\"type\":\"boolean\",\"description\":\"Explicitly request original-byte "
    "conversation preview; default false.\"},"
    "\"persist_manifest\":{\"type\":\"boolean\",\"description\":\"Write the private manifest "
    "beside the output recording the grid and its source hashes; default true.\"}},"
    "\"required\":[\"sources\",\"output_file\",\"size\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"speak\",\"description\":\"Speak a message "
    "aloud to the user using their ChatGPT login. Waits for playback; no audio file is kept.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},"
    "\"voice\":{\"type\":\"string\",\"description\":\"Omit to use the default cove voice.\"}},"
    "\"required\":[\"text\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"ask_user_question\",\"description\":\"Ask the "
    "user a clarifying question (interactive sessions "
    "only).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"question\":{\"type\":"
    "\"string\"}},\"required\":[\"question\"]}}}"
    "]";

static bool schema_tool_disabled(const tools_env *env, const char *name) {
    if (!env || !env->ctx || !name) return false;
    if (env->ctx->prompt_optimisation)
        return strcmp(name, "list_files") != 0 && strcmp(name, "glob_files") != 0 &&
               strcmp(name, "grep_files") != 0 && strcmp(name, "read_file") != 0 &&
               strcmp(name, "file_info") != 0 && strcmp(name, "read_tool_result") != 0;
    if (env->ctx->mcp_disabled && str_starts(name, "mcp_")) return true;
    /* Image input configured off (docs/adr/0089): hide read_image from the
     * schema and refuse it in tools_call_prepare with the same condition, so
     * native and --ssh calls agree. Image *generation* is independent and
     * stays available. */
    if (strcmp(name, "read_image") == 0 && tny_image_input_refused(env->ctx)) return true;
    if (strcmp(name, "image_generate") == 0 || strcmp(name, "image_edit") == 0)
        return env->ctx->library_mode || env->ctx->ssh_host ||
               !tny_image_capabilities(env->ctx, strcmp(name, "image_edit") == 0, NULL);
    /* Local exports depend on a host that can run the optional converter, not
     * on an image provider. Whether the executable is installed is answered at
     * call time with actionable guidance, never by probing at schema time
     * (docs/adr/0094). */
    if (strcmp(name, "image_preview") == 0) return env->ctx->library_mode || env->ctx->ssh_host;
    if (strcmp(name, "image_export") == 0 || strcmp(name, "image_contact_sheet") == 0)
        return env->ctx->library_mode || env->ctx->ssh_host || !tny_image_export_supported();
    if (strcmp(name, "speak") == 0)
        return env->ctx->library_mode || !tny_speech_available(env->ctx, NULL, true, NULL, 0);
    /* a child would run its tools locally, not on the --ssh host */
    if (strcmp(name, "subagent") == 0) return env->ctx->library_mode || env->ctx->ssh_host;
    /* Jobs own real child processes: the execution tools disappear where none
     * can exist, while bounded record reads remain (docs/adr/0093). */
    if (tool_jobs_is_tool(name)) return !tool_jobs_available(env->ctx, name);
    if (!env->ctx->library_mode) return false;
    return strcmp(name, "terminal") == 0 || strcmp(name, "open_file") == 0 ||
           strcmp(name, "skill") == 0 || strcmp(name, "install_skill") == 0 ||
           strcmp(name, "memory") == 0 || strcmp(name, "ask_user_question") == 0;
}

static bool profile_allows_builtin(const tools_env *env, const char *name) {
    if (!env || !env->ctx || !name || env->ctx->library_mode ||
        env->ctx->tool_profile == TNY_TOOLS_ALL)
        return true;
    if (strcmp(name, "terminal") == 0 || strcmp(name, "read_image") == 0) return true;
    return env->ctx->tool_profile == TNY_TOOLS_TERMINAL_EDIT &&
           (strcmp(name, "edit_file") == 0 ||
            (strcmp(name, "ask_user_question") == 0 && env->prompt));
}

/* Hidden from the advertised schema but still callable directly: web_search
 * without a configured provider (docs/adr/0055) keeps its runtime error. */
static bool schema_tool_hidden(const tools_env *env, const char *name) {
    if (schema_tool_disabled(env, name)) return true;
    if (!env || !env->ctx || !name) return false;
    if (!profile_allows_builtin(env, name)) return true;
    return strcmp(name, "web_search") == 0 && !tool_web_search_configured(env->ctx);
}

static char *append_custom_schema(char *base, custom_tool_registry *registry) {
    char *custom = custom_tools_schema_json(registry);
    if (!base || !custom) {
        free(base);
        free(custom);
        return NULL;
    }
    if (strcmp(custom, "[]") == 0) {
        free(custom);
        return base;
    }
    size_t base_len = strlen(base), custom_len = strlen(custom);
    buf_t merged;
    buf_init(&merged);
    if (base_len > 1) buf_append(&merged, base, base_len - 1);
    if (base_len > 2) buf_appends(&merged, ",");
    buf_append(&merged, custom + 1, custom_len - 1);
    free(base);
    free(custom);
    return buf_detach(&merged);
}

char *tools_schema_json(tools_env *env) {
    if (env && env->ctx &&
        (env->ctx->prompt_optimisation || env->ctx->mcp_disabled || env->ctx->library_mode ||
         env->ctx->tool_profile != TNY_TOOLS_ALL || !tool_web_search_configured(env->ctx) ||
         !tny_speech_available(env->ctx, NULL, true, NULL, 0) || env->ctx->ssh_host ||
         !tny_image_capabilities(env->ctx, false, NULL) || !tny_image_export_supported() ||
         tny_image_input_refused(env->ctx))) {
        yyjson_doc *doc = jparse(SCHEMA_JSON, strlen(SCHEMA_JSON));
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        yyjson_mut_doc *mut = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *out = mut ? yyjson_mut_arr(mut) : NULL;
        if (root && out) {
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(root, idx, max, item) {
                const char *name = jget_str(jget(item, "function"), "name");
                if (schema_tool_hidden(env, name)) continue;
                yyjson_mut_arr_add_val(out, yyjson_val_mut_copy(mut, item));
            }
            yyjson_mut_doc_set_root(mut, out);
            char *json = jwrite(mut);
            yyjson_mut_doc_free(mut);
            yyjson_doc_free(doc);
            return json ? append_custom_schema(json, env->ctx->custom_tools) : NULL;
        }
        yyjson_mut_doc_free(mut);
        yyjson_doc_free(doc);
        return NULL; /* Never expose disabled tools on an allocation failure. */
    }
    return append_custom_schema(xstrdup(SCHEMA_JSON),
                                env && env->ctx ? env->ctx->custom_tools : NULL);
}

static bool json_type_matches(yyjson_val *value, const char *type) {
    if (!value || !type) return false;
    if (strcmp(type, "string") == 0) return yyjson_is_str(value);
    if (strcmp(type, "integer") == 0) return yyjson_is_int(value) || yyjson_is_uint(value);
    if (strcmp(type, "number") == 0) return yyjson_is_num(value);
    if (strcmp(type, "boolean") == 0) return yyjson_is_bool(value);
    if (strcmp(type, "object") == 0) return yyjson_is_obj(value);
    if (strcmp(type, "array") == 0) return yyjson_is_arr(value);
    return true; /* unknown future schema type is validated by the tool */
}

static int validate_parameters(const char *name, yyjson_val *args, yyjson_val *parameters,
                               char **error) {
    if (!args || !yyjson_is_obj(args)) {
        *error = tool_err("arguments for %s must be a JSON object", name);
        return -1;
    }
    if (!parameters || !yyjson_is_obj(parameters)) {
        *error = tool_err("unknown tool %s", name);
        return -1;
    }
    yyjson_val *required = jget(parameters, "required");
    if (required && yyjson_is_arr(required)) {
        size_t idx, max;
        yyjson_val *field;
        yyjson_arr_foreach(required, idx, max, field) {
            const char *key = yyjson_get_str(field);
            if (key && !jget(args, key)) {
                *error = tool_err("%s needs argument %s", name, key);
                return -1;
            }
        }
    }
    yyjson_val *properties = jget(parameters, "properties");
    if (properties && yyjson_is_obj(properties)) {
        size_t idx, max;
        yyjson_val *key_value, *schema;
        yyjson_obj_foreach(properties, idx, max, key_value, schema) {
            const char *key = yyjson_get_str(key_value);
            yyjson_val *value = key ? jget(args, key) : NULL;
            if (!value) continue;
            const char *type = jget_str(schema, "type");
            if (type && !json_type_matches(value, type)) {
                *error = tool_err("%s argument %s must be %s", name, key, type);
                return -1;
            }
        }
    }
    yyjson_val *additional = jget(parameters, "additionalProperties");
    if (additional && yyjson_is_bool(additional) && !yyjson_get_bool(additional) &&
        yyjson_is_obj(args)) {
        size_t idx, max;
        yyjson_val *argument_key, *argument_value;
        yyjson_obj_foreach(args, idx, max, argument_key, argument_value) {
            (void)argument_value;
            const char *key = yyjson_get_str(argument_key);
            if (!key || !properties || !jget(properties, key)) {
                *error = tool_err("%s does not allow argument %s", name, key ? key : "<invalid>");
                return -1;
            }
        }
    }
    return 0;
}

static int validate_call_schema(const char *name, yyjson_val *args, char **error) {
    yyjson_doc *schemas = jparse(SCHEMA_JSON, strlen(SCHEMA_JSON));
    yyjson_val *root = schemas ? yyjson_doc_get_root(schemas) : NULL;
    yyjson_val *parameters = NULL;
    if (root && yyjson_is_arr(root)) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(root, idx, max, item) {
            yyjson_val *function = jget(item, "function");
            const char *candidate = jget_str(function, "name");
            if (candidate && strcmp(candidate, name) == 0) {
                parameters = jget(function, "parameters");
                break;
            }
        }
    }
    if (!parameters) {
        yyjson_doc_free(schemas);
        *error = tool_err("unknown tool %s", name);
        return -1;
    }
    int status = validate_parameters(name, args, parameters, error);
    yyjson_doc_free(schemas);
    return status;
}

char *tools_path_detail(tools_env *env, const char *p) {
    if (!p) return NULL;
    if (p[0] == '/') return xstrdup(p);
    if (env->ctx->ssh_host) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "%s/%s", env->ctx->ssh_cwd ? env->ctx->ssh_cwd : "", p);
        return buf_detach(&b);
    }
    char *err = NULL;
    char *abs = tool_resolve_path(env, p, &err);
    free(err);
    return abs;
}

/* Extract one path-like human detail used for permission rules/prompts. */
static char *path_detail(tools_env *env, yyjson_val *args, const char *field) {
    return tools_path_detail(env, jget_str(args, field));
}

/* Extract the primary human detail used for permission rules and prompts. */
static char *call_detail(tools_env *env, const char *name, yyjson_val *args) {
    if (strcmp(name, "terminal") == 0 || strcmp(name, "run_command") == 0) {
        const char *cmd = jget_str(args, "command");
        return cmd ? xstrdup(cmd) : NULL;
    }
    char *detail = path_detail(env, args, "path");
    if (detail) return detail;
    const char *url = jget_str(args, "url");
    return url ? xstrdup(url) : NULL;
}

static const char *canonical_name(const char *name) {
    if (strcmp(name, "run_command") == 0) return "terminal"; /* fx alias */
    if (strcmp(name, "vision") == 0) return "read_image";    /* fx name */
    return name;
}

int tools_call_prepare(tools_env *env, const char *name, const char *args_json, tools_call *call) {
    if (!call) return -1;
    memset(call, 0, sizeof *call);
    if (!env || !name) return -1;
    call->name = xstrdup(canonical_name(name));
    call->permission_tool = call->name ? xstrdup(call->name) : NULL;
    if (!call->name || !call->permission_tool) return -1;
    call->doc = args_json ? jparse(args_json, strlen(args_json)) : NULL;
    call->args = call->doc ? yyjson_doc_get_root(call->doc) : NULL;
    /* Stable SUBAGENT_* answers before any permission prompt, extension
     * event or child process (docs/features/mcp-and-skills.md#subagents). */
    if (strcmp(call->name, "subagent") == 0) {
        call->error = tny_subagent_prepare_error(env, call->args);
        if (call->error) return -1;
    }
    if (env->ctx->prompt_optimisation && schema_tool_disabled(env, call->name)) {
        call->error = tool_err("tool %s is unavailable during prompt optimisation", call->name);
        return -1;
    }
    call->custom = custom_tools_find(env->ctx->custom_tools, call->name);
    if (call->custom) {
        if (!args_json || strlen(args_json) > custom_tool_argument_limit(call->custom)) {
            call->error = tool_err("arguments for %s exceed the registered limit", call->name);
            return -1;
        }
        yyjson_doc *schema =
            jparse(custom_tool_schema(call->custom), strlen(custom_tool_schema(call->custom)));
        yyjson_val *parameters = schema ? yyjson_doc_get_root(schema) : NULL;
        int valid = validate_parameters(call->name, call->args, parameters, &call->error);
        yyjson_doc_free(schema);
        if (valid != 0) return -1;
    } else {
        if (schema_tool_disabled(env, call->name)) {
            call->error = tool_err("tool %s is unavailable in this runtime", call->name);
            return -1;
        }
        if (!profile_allows_builtin(env, call->name)) {
            call->error = tool_err("unknown tool %s", call->name);
            return -1;
        }
        if (validate_call_schema(call->name, call->args, &call->error) != 0) return -1;
    }
    if (strcmp(call->name, "mcp_select_tool") == 0) {
        const char *server = jget_str(call->args, "server");
        const char *tool = jget_str(call->args, "tool");
        free(call->permission_tool);
        buf_t identity;
        buf_init(&identity);
        buf_appendf(&identity, "mcp:%s/%s", server, tool);
        call->permission_tool = buf_detach(&identity);
    }
    if (strcmp(call->name, "image_generate") == 0 || strcmp(call->name, "image_edit") == 0) {
        /* Resolving once here is what makes the permission decision and the
         * execution talk about the same operation (ADR 0095). */
        call->detail = tool_image_detail(env, call->args, strcmp(call->name, "image_edit") == 0,
                                         &call->image_plan, &call->error);
        if (call->error || !call->detail) return -1;
    } else if (strcmp(call->name, "image_preview") == 0) {
        call->detail =
            tool_image_preview_detail(env, call->args, &call->image_selection, &call->error);
        if (call->error || !call->detail) return -1;
    } else if (strcmp(call->name, "image_export") == 0 ||
               strcmp(call->name, "image_contact_sheet") == 0) {
        /* The grant scope is the whole operation: ordered sources with the
         * hash of their exact bytes, the destination and every setting that
         * changes what is written (docs/adr/0094). */
        call->detail = tool_image_export_detail(
            env, call->args, strcmp(call->name, "image_contact_sheet") == 0, &call->error);
        if (call->error || !call->detail) return -1;
    } else if (tool_jobs_is_tool(call->name)) {
        /* Every job operation carries its own exact permission identity, and
         * the detail names the job, items, outputs and request digest. */
        tny_jobs_op op = tool_jobs_op(call->name, call->args);
        if (op == TNY_JOBS_OP_NONE) {
            call->error = tool_err("%s does not support that action", call->name);
            return -1;
        }
        free(call->permission_tool);
        call->permission_tool = xstrdup(tny_jobs_permission_tool(op));
        char *why = NULL;
        call->detail = tny_jobs_detail(env->ctx, op, call->args, &why);
        if (!call->permission_tool || why || !call->detail) {
            call->error = why ? tool_err("%s", why) : tool_err("invalid %s request", call->name);
            free(why);
            return -1;
        }
    } else call->detail = call_detail(env, call->name, call->args);
    if (strcmp(call->name, "rename_file") == 0 || strcmp(call->name, "copy_file") == 0)
        call->detail2 = path_detail(env, call->args, "new_path");
    /* A first-party tny verb typed into `terminal` becomes the typed tool it
     * stands for: same permission identity, same detail, same executor
     * (docs/adr/0063). A background command keeps its detached contract and
     * is never intercepted. */
    if (!call->custom && strcmp(call->name, "terminal") == 0 &&
        !jget_bool(call->args, "background", false)) {
        call->intercept = tny_intercept_parse(env, call->detail);
        if (call->intercept) {
            if (call->intercept->kind == TNY_INTERCEPT_REFUSED) {
                call->error = tool_err("%s", call->intercept->message);
                return -1;
            }
            free(call->permission_tool);
            call->permission_tool = xstrdup(call->intercept->permission_tool);
            free(call->detail);
            call->detail = call->intercept->detail ? xstrdup(call->intercept->detail) : NULL;
            if (!call->permission_tool) return -1;
        }
    }
    call->verdict = call->custom && !custom_tool_sensitive(call->custom)
                        ? PERM_ALLOW
                        : perm_check(env->perm, call->permission_tool, call->detail);
    if (call->detail2) {
        perm_verdict second = perm_check(env->perm, call->permission_tool, call->detail2);
        if (second == PERM_DENY || call->verdict == PERM_DENY) call->verdict = PERM_DENY;
        else if (second == PERM_PROMPT || call->verdict == PERM_PROMPT) call->verdict = PERM_PROMPT;
    }
    if (call->verdict == PERM_PROMPT) {
        buf_t summary;
        buf_init(&summary);
        if (call->intercept) buf_appendf(&summary, "%s -> ", call->intercept->label);
        buf_appendf(&summary, "%s %s", call->permission_tool, call->detail ? call->detail : "");
        if (call->detail2) buf_appendf(&summary, " -> %s", call->detail2);
        call->summary = buf_detach(&summary);
    }
    if (tny_alloc_scope_failed()) return -1;
    return 0;
}

void tools_call_grant(tools_env *env, const tools_call *call) {
    if (!env || !call) return;
    perm_grant(env->perm, call->permission_tool, call->detail);
    if (call->detail2) perm_grant(env->perm, call->permission_tool, call->detail2);
}

const char *tools_call_label(const tools_call *call) {
    return call && call->intercept ? call->intercept->label : NULL;
}

char *tools_call_execute(tools_env *env, tools_call *call) {
    const char *name = call->name;
    yyjson_val *args = call->args;

    if (call->intercept) return tny_intercept_execute(env, call->intercept);

    if (call->custom) {
        char *result = NULL;
        bool is_error = false;
        char *arguments = call->doc ? yyjson_write(call->doc, 0, NULL) : NULL;
        if (!arguments) return tool_err("could not copy custom tool arguments");
        int32_t status =
            custom_tool_invoke(call->custom, arguments, &call->custom_call, &result, &is_error);
        free(arguments);
        if (status == TNY_TOOL_INVOKE_ASYNC) return NULL;
        if (status != TNY_STATUS_OK)
            return tool_err("custom tool %s callback failed (%d)", name, (int)status);
        if (is_error && !str_starts(result, "error:")) {
            char *wrapped = tool_err("%s", result);
            free(result);
            result = wrapped;
            if (!result) return NULL;
        }
        char *bounded = tool_bound_result(env, result, strlen(result));
        free(result);
        return bounded;
    }

    /* Image tools run the plan prepared above, so they are dispatched with
     * that plan rather than through the name-only executor chain. */
    if (strcmp(name, "image_preview") == 0)
        return tool_image_preview_execute(env, call->image_selection);
    if (strcmp(name, "image_generate") == 0 || strcmp(name, "image_edit") == 0)
        return tool_image_execute(env, args, strcmp(name, "image_edit") == 0, call->image_plan);

    /* Execution authorizes this prepared identity, including ALLOW_ONCE.
     * Pass it directly, without promoting it to a lasting session grant. */
    if (strcmp(name, "image_export") == 0 || strcmp(name, "image_contact_sheet") == 0)
        return tool_image_export_execute(env, args, strcmp(name, "image_contact_sheet") == 0,
                                         call->detail);

    bool handled;
    char *out = tool_ssh_execute(env, name, args, &handled);
    if (!handled) out = tool_fs_execute(env, name, args, &handled);
    if (!handled) out = tool_shell_execute(env, name, args, &handled);
    if (!handled) out = tool_web_execute(env, name, args, &handled);
    if (!handled) out = tool_ext_execute(env, name, args, &handled);
    if (!handled) out = tool_err("unknown tool %s", name);
    return out;
}

bool tools_call_pending(const tools_call *call) { return call && call->custom_call; }

int tools_call_take_async(tools_call *call, char **result, bool *is_error) {
    return call && call->custom_call ? custom_tool_take(call->custom_call, result, is_error) : -1;
}

void tools_call_invalidate_async(tools_call *call) {
    if (call && call->custom_call) custom_tool_invalidate(call->custom_call);
}

void tools_call_free(tools_call *call) {
    if (!call) return;
    free(call->name);
    free(call->permission_tool);
    free(call->detail);
    free(call->detail2);
    free(call->summary);
    free(call->error);
    tool_image_plan_free(call->image_plan);
    tny_image_preview_selection_free(call->image_selection);
    tny_intercept_free(call->intercept);
    yyjson_doc_free(call->doc);
    memset(call, 0, sizeof *call);
}

bool tools_pending_images_have_preview(const tools_env *env) {
    if (!env) return false;
    for (int i = 0; i < env->n_pending_images; i++)
        if (env->pending_capture[i].origin == TNY_IMAGE_QUEUE_PREVIEW) return true;
    return false;
}

void tools_discard_pending_images(tools_env *env) {
    if (!env) return;
    for (int i = 0; i < env->n_pending_images; i++) {
        free(env->pending_images[i]);
        env->pending_images[i] = NULL;
        free(env->pending_capture[i].data);
        memset(&env->pending_capture[i], 0, sizeof env->pending_capture[i]);
    }
    env->n_pending_images = 0;
}

/* Truthful wording for what this batch actually is. It never says a model saw
 * anything: the pixels are being sent now, nothing has been perceived. */
static const char *pending_images_text(const tools_env *env) {
    bool manual = false, preview = false;
    for (int i = 0; i < env->n_pending_images; i++) {
        if (env->pending_capture[i].origin == TNY_IMAGE_QUEUE_PREVIEW) preview = true;
        else manual = true;
    }
    if (preview && manual) return "Images attached by explicit tool requests.";
    if (preview) return "Images queued by explicitly requested generation/edit preview.";
    return "Image attached by read_image.";
}

int tools_flush_images(tools_env *env, char *err, size_t errlen) {
    return tools_flush_images_ex(env, NULL, err, errlen);
}

int tools_flush_images_ex(tools_env *env, tools_image_flush_outcome *outcome, char *err,
                          size_t errlen) {
    if (outcome) *outcome = TNY_IMAGE_FLUSH_OK;
    if (!env || env->n_pending_images <= 0) return 0;
    bool preview = tools_pending_images_have_preview(env);
    tools_image_flush_outcome failure =
        preview ? TNY_IMAGE_FLUSH_PREVIEW_FATAL : TNY_IMAGE_FLUSH_FAILED;
    /* Every entry is checked before anything is mutated: a partially delivered
     * batch would need partial-delivery and retry rules this feature does not
     * have (docs/adr/0096). A provider switch after the queue filled must not
     * flush those bytes into a provider configured without image input; the
     * refusal keeps the pending entries and count intact (docs/adr/0089). */
    if (tny_image_input_refused(env->ctx)) {
        if (outcome) *outcome = failure;
        if (err && errlen) snprintf(err, errlen, "%s", TNY_IMAGE_INPUT_REFUSAL);
        return -1;
    }
    if (preview && !tny_image_input_auto_preview_allowed(env->ctx)) {
        if (outcome) *outcome = TNY_IMAGE_FLUSH_PREVIEW_FATAL;
        if (err && errlen)
            snprintf(err, errlen,
                     "queued image preview cannot be delivered: this provider is not configured "
                     "for image input");
        return -1;
    }

    /* Every admission, including SSH, owns captured bytes. No path fallback. */
    tny_image_part parts[8];
    int n = env->n_pending_images;
    int rc = -1;
    if (n <= 8) {
        for (int i = 0; i < n; i++) {
            tools_pending_capture *cap = &env->pending_capture[i];
            parts[i] = (tny_image_part){cap->data, cap->len, cap->mime};
        }
        rc = session_add_user_loaded_images(env->session, pending_images_text(env), parts, n, err,
                                            errlen);
    } else if (err && errlen) {
        snprintf(err, errlen, "too many images in this step (max 8)");
    }
    if (rc != 0) {
        /* A preview-bearing batch is preserved until its owner has recorded
         * the non-delivery; a manual-only load failure keeps the existing
         * drop-and-continue behavior. */
        if (outcome) *outcome = failure;
        if (!preview) tools_discard_pending_images(env);
        return -1;
    }
    tools_discard_pending_images(env);
    return 0;
}

/* One admission path for every queued image. `origin` decides the extra
 * preview policy gate and the expected-hash check; the bytes are captured
 * here and never reloaded. */
static int queue_capture(tools_env *env, const char *path, bool allowed_roots_only,
                         tny_image_queue_origin origin, const char *expected_sha256,
                         uint64_t expected_bytes, const char **resolved_out, const char **mime_out,
                         size_t *len_out, const char **code_out, char *err, size_t errlen) {
    if (resolved_out) *resolved_out = NULL;
    if (mime_out) *mime_out = NULL;
    if (len_out) *len_out = 0;
    if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_INTERNAL;
    /* Direct-call safeguard for read_image, `tny image attach`, interception
     * and preview: refuse before the path is resolved, read or appended to the
     * pending queue (docs/adr/0089). */
    if (env && tny_image_input_refused(env->ctx)) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_CAPABILITY;
        if (err && errlen) snprintf(err, errlen, "%s", TNY_IMAGE_INPUT_REFUSAL);
        return -1;
    }
    /* An automatic preview additionally needs configured-true support at
     * enqueue; unknown keeps every existing manual path working but can never
     * authorize a preview (A4, A15). */
    if (origin == TNY_IMAGE_QUEUE_PREVIEW &&
        !tny_image_input_auto_preview_allowed(env ? env->ctx : NULL)) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_CAPABILITY;
        if (err && errlen)
            snprintf(err, errlen,
                     "image preview needs this provider configured for image input in "
                     "settings.json image_input");
        return -1;
    }
    if (origin == TNY_IMAGE_QUEUE_PREVIEW && !tny_image_preview_hash_valid(expected_sha256)) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_HASH;
        if (err && errlen)
            snprintf(err, errlen, "image preview needs a 64-character lowercase hex sha256");
        return -1;
    }
    if (!env || !env->ctx || env->n_pending_images >= 8) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_CAPACITY;
        if (err && errlen) snprintf(err, errlen, "too many images in this step (max 8)");
        return -1;
    }
    char *resolve_err = NULL;
    char *abs = tool_resolve_path(env, path, &resolve_err);
    if (!abs) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_UNREADABLE;
        if (err && errlen)
            snprintf(err, errlen, "%s", resolve_err ? resolve_err : "invalid image path");
        free(resolve_err);
        return -1;
    }
    if (allowed_roots_only && !perm_path_allowed(env->ctx, abs)) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_ROOTS;
        if (err && errlen) snprintf(err, errlen, "image path is outside the allowed roots");
        free(abs);
        return -1;
    }
    size_t len = 0;
    const char *mime = NULL;
    const char *load_code = NULL;
    uint8_t *data = image_load_ex(abs, &len, &mime, &load_code, err, errlen);
    if (!data) {
        if (code_out) *code_out = load_code ? load_code : TNY_IMAGE_PREVIEW_CODE_UNREADABLE;
        free(abs);
        return -1;
    }
    tools_pending_capture cap = {0};
    cap.data = data;
    cap.len = len;
    cap.mime = mime;
    cap.origin = origin;
    if (!tny_image_io_sha256_hex(data, len, cap.sha256)) {
        if (err && errlen) snprintf(err, errlen, "cannot hash image %s", abs);
        free(data);
        free(abs);
        return -1;
    }
    /* The supplied hash is compared against exactly these captured bytes, so a
     * file replaced after the check cannot change what is sent. */
    if (origin == TNY_IMAGE_QUEUE_PREVIEW && memcmp(cap.sha256, expected_sha256, 64) != 0) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_HASH;
        if (err && errlen)
            snprintf(err, errlen, "image preview bytes do not match the expected sha256");
        free(data);
        free(abs);
        return -1;
    }
    if (origin == TNY_IMAGE_QUEUE_PREVIEW && expected_bytes && expected_bytes != len) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_BYTES;
        if (err && errlen)
            snprintf(err, errlen,
                     "image preview captured byte count does not match producer identity");
        free(data);
        free(abs);
        return -1;
    }
    int slot = env->n_pending_images++;
    env->pending_images[slot] = abs;
    env->pending_capture[slot] = cap;
    if (resolved_out) *resolved_out = abs;
    if (mime_out) *mime_out = mime;
    if (len_out) *len_out = len;
    if (code_out) *code_out = NULL;
    return 0;
}

int tools_queue_image(tools_env *env, const char *path, bool allowed_roots_only,
                      const char **resolved_out, const char **mime_out, size_t *len_out, char *err,
                      size_t errlen) {
    return queue_capture(env, path, allowed_roots_only, TNY_IMAGE_QUEUE_MANUAL, NULL, 0,
                         resolved_out, mime_out, len_out, NULL, err, errlen);
}

int tools_queue_image_preview(tools_env *env, const char *path, const char *expected_sha256,
                              uint64_t expected_bytes, const char **code_out, char *err,
                              size_t errlen) {
    return queue_capture(env, path, true, TNY_IMAGE_QUEUE_PREVIEW, expected_sha256, expected_bytes,
                         NULL, NULL, NULL, code_out, err, errlen);
}

char *tools_execute(tools_env *env, const char *name, const char *args_json) {
    name = canonical_name(name);
    tools_call call;
    if (tools_call_prepare(env, name, args_json, &call) != 0) {
        char *out =
            call.error ? xstrdup(call.error) : tool_err("cannot prepare tool call %s", name);
        tools_call_free(&call);
        return out;
    }
    perm_verdict v = call.verdict;
    if (v == PERM_PROMPT && env->prompt) {
        tny_perm_decision d = env->prompt(call.name, call.summary, env->prompt_ud);
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS) {
            tools_call_grant(env, &call);
            v = PERM_ALLOW;
        } else if (d == TNY_PERM_DECISION_ALLOW) {
            v = PERM_ALLOW;
        } else {
            v = PERM_DENY;
        }
    }
    if (v == PERM_PROMPT) {
        env->perm_blocked = true;
        char *out = tool_err("permission required for %s and no reviewer is available "
                             "(run with --auto, --yolo, or grant a rule)",
                             call.name);
        tools_call_free(&call);
        return out;
    }
    if (v == PERM_DENY) {
        char *out = tool_err("permission denied for %s", call.name);
        tools_call_free(&call);
        return out;
    }
    char *out = tools_call_execute(env, &call);
    tools_call_free(&call);
    return out;
}
