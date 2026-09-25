#include "code_runtime.h"
#include "util/util.h"
#include "yyjson.h"
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    size_t size;
} allocation;

typedef struct {
    size_t memory;
    unsigned instructions, calls;
    const int64_t *deadline;
    const char *code, *catalog;
    tny_code_call_fn call;
    void *userdata;
    char *output;
    size_t output_len;
    /* Native scratch remains reachable if Lua raises/longjmps on OOM. */
    char *pending, *serialized;
    yyjson_doc *doc;
    yyjson_mut_doc *mut;
    yyjson_alc allocator;
} code_state;

static void *code_alloc(void *ud, void *ptr, size_t old_size, size_t size) {
    (void)old_size;
    code_state *s = ud;
    allocation *old = ptr ? (allocation *)ptr - 1 : NULL;
    size_t previous = old ? sizeof(*old) + old->size : 0;
    if (!size) {
        s->memory -= previous;
        free(old);
        return NULL;
    }
    if (size > TNY_CODE_MEMORY_BYTES - sizeof(allocation) ||
        sizeof(allocation) + size > TNY_CODE_MEMORY_BYTES - (s->memory - previous))
        return NULL;
    allocation *next = realloc(old, sizeof(*next) + size);
    if (!next) return NULL;
    next->size = size;
    s->memory = s->memory - previous + sizeof(*next) + size;
    return next + 1;
}
static void *json_alloc(void *ud, size_t size) { return code_alloc(ud, NULL, 0, size); }
static void json_free(void *ud, void *ptr) { (void)code_alloc(ud, ptr, 0, 0); }
static code_state *state(lua_State *L) {
    code_state *s;
    memcpy(&s, lua_getextraspace(L), sizeof(code_state *));
    return s;
}
static void check_budget(lua_State *L) {
    code_state *s = state(L);
    if (monotonic_ms() >= *s->deadline) luaL_error(L, "deadline exceeded");
}
static void instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    code_state *s = state(L);
    s->instructions += 1000;
    if (s->instructions >= TNY_CODE_INSTRUCTIONS) luaL_error(L, "instruction limit exceeded");
    check_budget(L);
}
static void append_output(lua_State *L, const char *data, size_t len) {
    code_state *s = state(L);
    if (len > TNY_CODE_OUTPUT_BYTES - s->output_len) luaL_error(L, "output limit exceeded");
    /* C-string tool output cannot represent embedded NUL. */
    if (memchr(data, 0, len)) luaL_error(L, "output contains a NUL byte");
    memcpy(s->output + s->output_len, data, len);
    s->output_len += len;
    s->output[s->output_len] = 0;
}
static int code_print(lua_State *L) {
    int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        size_t len;
        const char *value = luaL_tolstring(L, i, &len);
        if (i > 1) append_output(L, "\t", 1);
        append_output(L, value, len);
        lua_pop(L, 1);
    }
    append_output(L, "\n", 1);
    return 0;
}
static const char *text_arg(lua_State *L, int index, size_t *len) {
    const char *value = luaL_checklstring(L, index, len);
    if (memchr(value, 0, *len)) luaL_error(L, "argument contains a NUL byte");
    return value;
}
static int code_call(lua_State *L) {
    code_state *s = state(L);
    size_t name_len, args_len;
    const char *name = text_arg(L, 1, &name_len);
    const char *args = text_arg(L, 2, &args_len);
    if (!name_len || name_len > 256) return luaL_error(L, "invalid tool name");
    if (strcmp(name, "run_code") == 0) return luaL_error(L, "recursive run_code is forbidden");
    if (args_len > TNY_CODE_SOURCE_BYTES) return luaL_error(L, "tool arguments limit exceeded");
    s->doc = yyjson_read_opts((char *)args, args_len, 0, &s->allocator, NULL);
    bool valid = s->doc && yyjson_is_obj(yyjson_doc_get_root(s->doc));
    yyjson_doc_free(s->doc);
    s->doc = NULL;
    if (!valid) return luaL_error(L, "tool arguments must be a JSON object");
    check_budget(L);
    if (s->calls >= TNY_CODE_TOOL_CALLS) return luaL_error(L, "tool call limit exceeded");
    if (!s->call) return luaL_error(L, "tool callback unavailable");
    ++s->calls;
    s->pending = s->call(s->userdata, name, args);
    if (!s->pending) return luaL_error(L, "tool callback failed");
    check_budget(L);
    size_t len = strlen(s->pending);
    if (len > TNY_CODE_MEMORY_BYTES) return luaL_error(L, "tool result limit exceeded");
    lua_pushlstring(L, s->pending, len);
    free(s->pending);
    s->pending = NULL;
    return 1;
}
static int code_list(lua_State *L) {
    lua_pushstring(L, state(L)->catalog);
    return 1;
}
static int code_describe(lua_State *L) {
    code_state *s = state(L);
    size_t len;
    const char *name = text_arg(L, 1, &len);
    s->doc = yyjson_read_opts((char *)s->catalog, strlen(s->catalog), 0, &s->allocator, NULL);
    yyjson_val *root = yyjson_doc_get_root(s->doc), *entry;
    size_t index, count;
    yyjson_arr_foreach(root, index, count, entry) {
        yyjson_val *fn = yyjson_obj_get(entry, "function");
        if (!fn) fn = entry;
        const char *candidate = yyjson_get_str(yyjson_obj_get(fn, "name"));
        if (!candidate || strcmp(name, candidate) != 0) continue;
        size_t out_len;
        s->serialized = yyjson_val_write_opts(entry, 0, &s->allocator, &out_len, NULL);
        if (!s->serialized) return luaL_error(L, "memory limit exceeded");
        lua_pushlstring(L, s->serialized, out_len);
        json_free(s, s->serialized);
        s->serialized = NULL;
        yyjson_doc_free(s->doc);
        s->doc = NULL;
        return 1;
    }
    yyjson_doc_free(s->doc);
    s->doc = NULL;
    lua_pushnil(L);
    return 1;
}

static void push_json(lua_State *L, yyjson_val *v, unsigned depth) {
    if (depth > 32 || !lua_checkstack(L, 4)) luaL_error(L, "JSON nesting limit exceeded");
    if (yyjson_is_null(v)) lua_pushlightuserdata(L, NULL);
    else if (yyjson_is_bool(v)) lua_pushboolean(L, yyjson_get_bool(v));
    else if (yyjson_is_sint(v)) lua_pushinteger(L, (lua_Integer)yyjson_get_sint(v));
    else if (yyjson_is_uint(v) && yyjson_get_uint(v) <= (uint64_t)LUA_MAXINTEGER)
        lua_pushinteger(L, (lua_Integer)yyjson_get_uint(v));
    else if (yyjson_is_num(v)) lua_pushnumber(L, yyjson_get_num(v));
    else if (yyjson_is_str(v)) lua_pushlstring(L, yyjson_get_str(v), yyjson_get_len(v));
    else if (yyjson_is_arr(v)) {
        lua_newtable(L);
        luaL_getmetatable(L, "tny.json.array");
        lua_setmetatable(L, -2);
        size_t i, count;
        yyjson_val *item;
        yyjson_arr_foreach(v, i, count, item) {
            push_json(L, item, depth + 1);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
    } else if (yyjson_is_obj(v)) {
        lua_newtable(L);
        size_t i, count;
        yyjson_val *key, *item;
        yyjson_obj_foreach(v, i, count, key, item) {
            lua_pushlstring(L, yyjson_get_str(key), yyjson_get_len(key));
            push_json(L, item, depth + 1);
            lua_rawset(L, -3);
        }
    }
}
static int code_decode(lua_State *L) {
    code_state *s = state(L);
    size_t len;
    const char *text = luaL_checklstring(L, 1, &len);
    s->doc = yyjson_read_opts((char *)text, len, 0, &s->allocator, NULL);
    if (!s->doc) return luaL_error(L, "invalid JSON or memory limit exceeded");
    push_json(L, yyjson_doc_get_root(s->doc), 0);
    yyjson_doc_free(s->doc);
    s->doc = NULL;
    return 1;
}
static yyjson_mut_val *encode_value(lua_State *L, int pos, unsigned depth) {
    code_state *s = state(L);
    if (depth > 32 || !lua_checkstack(L, 4)) luaL_error(L, "JSON nesting limit exceeded");
    pos = lua_absindex(L, pos);
    yyjson_mut_val *value = NULL;
    switch (lua_type(L, pos)) {
    case LUA_TNIL: value = yyjson_mut_null(s->mut); break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, pos)) luaL_error(L, "unsupported JSON value");
        value = yyjson_mut_null(s->mut);
        break;
    case LUA_TBOOLEAN: value = yyjson_mut_bool(s->mut, lua_toboolean(L, pos)); break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, pos)) value = yyjson_mut_sint(s->mut, lua_tointeger(L, pos));
        else {
            double number = lua_tonumber(L, pos);
            if (!isfinite(number)) luaL_error(L, "non-finite JSON number");
            value = yyjson_mut_real(s->mut, number);
        }
        break;
    case LUA_TSTRING: {
        size_t len;
        const char *str = lua_tolstring(L, pos, &len);
        value = yyjson_mut_strncpy(s->mut, str, len);
        break;
    }
    case LUA_TTABLE: {
        size_t len = lua_rawlen(L, pos), entries = 0;
        bool array = len > 0;
        if (lua_getmetatable(L, pos)) {
            luaL_getmetatable(L, "tny.json.array");
            array = array || lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
        }
        lua_pushnil(L);
        while (lua_next(L, pos)) {
            ++entries;
            if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 ||
                (lua_Unsigned)lua_tointeger(L, -2) > len)
                array = false;
            lua_pop(L, 1);
        }
        array = array && entries == len;
        value = array ? yyjson_mut_arr(s->mut) : yyjson_mut_obj(s->mut);
        if (!value) luaL_error(L, "memory limit exceeded");
        if (array) {
            for (size_t i = 1; i <= len; ++i) {
                lua_rawgeti(L, pos, (lua_Integer)i);
                if (!yyjson_mut_arr_append(value, encode_value(L, -1, depth + 1)))
                    luaL_error(L, "memory limit exceeded");
                lua_pop(L, 1);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, pos)) {
                if (lua_type(L, -2) != LUA_TSTRING)
                    luaL_error(L, "JSON object keys must be strings");
                yyjson_mut_val *key = encode_value(L, -2, depth + 1);
                if (!yyjson_mut_obj_add(value, key, encode_value(L, -1, depth + 1)))
                    luaL_error(L, "memory limit exceeded");
                lua_pop(L, 1);
            }
        }
        break;
    }
    default: luaL_error(L, "unsupported JSON value");
    }
    if (!value) luaL_error(L, "memory limit exceeded");
    return value;
}
static int code_encode(lua_State *L) {
    code_state *s = state(L);
    s->mut = yyjson_mut_doc_new(&s->allocator);
    if (!s->mut) return luaL_error(L, "memory limit exceeded");
    yyjson_mut_doc_set_root(s->mut, encode_value(L, 1, 0));
    size_t len;
    s->serialized = yyjson_mut_write_opts(s->mut, 0, &s->allocator, &len, NULL);
    if (!s->serialized) return luaL_error(L, "JSON encoding failed or memory limit exceeded");
    lua_pushlstring(L, s->serialized, len);
    json_free(s, s->serialized);
    s->serialized = NULL;
    yyjson_mut_doc_free(s->mut);
    s->mut = NULL;
    return 1;
}
static void set_function(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}
static int initialize_and_run(lua_State *L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    const char *removed[] = {"dofile",         "loadfile",     "load",
                             "collectgarbage", "pcall",        "xpcall",
                             "setmetatable",   "getmetatable", NULL};
    for (size_t i = 0; removed[i]; ++i) {
        lua_pushnil(L);
        lua_setglobal(L, removed[i]);
    }
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    lua_pushcfunction(L, code_print);
    lua_setglobal(L, "print");
    lua_newtable(L);
    set_function(L, "call", code_call);
    set_function(L, "list", code_list);
    set_function(L, "describe", code_describe);
    lua_setglobal(L, "tools");
    luaL_newmetatable(L, "tny.json.array");
    lua_pop(L, 1);
    lua_newtable(L);
    set_function(L, "encode", code_encode);
    set_function(L, "decode", code_decode);
    lua_pushlightuserdata(L, NULL);
    lua_setfield(L, -2, "null");
    lua_setglobal(L, "json");
    code_state *s = state(L);
    if (luaL_loadbufferx(L, s->code, strlen(s->code), "code", "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    int results = lua_gettop(L);
    if (results) code_print(L);
    check_budget(L);
    return 0;
}
char *tny_code_run_with_deadline(const char *code, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata) {
    if (!code || strlen(code) > TNY_CODE_SOURCE_BYTES)
        return xstrdup("error: code: source limit exceeded");
    if (!deadline) return xstrdup("error: code: missing host deadline");
    code_state s = {.code = code,
                    .catalog = catalog_json ? catalog_json : "[]",
                    .call = call,
                    .userdata = userdata,
                    .deadline = deadline};
    s.allocator =
        (yyjson_alc){.malloc = json_alloc, .realloc = code_alloc, .free = json_free, .ctx = &s};
    s.output = malloc(TNY_CODE_OUTPUT_BYTES + 1);
    if (!s.output) return NULL;
    s.output[0] = 0;
    lua_State *L = lua_newstate(code_alloc, &s);
    if (!L) {
        free(s.output);
        return xstrdup("error: code: memory limit exceeded");
    }
    code_state *state_ptr = &s;
    memcpy(lua_getextraspace(L), &state_ptr, sizeof(code_state *));
    lua_sethook(L, instruction_hook, LUA_MASKCOUNT, 1000);
    lua_pushcfunction(L, initialize_and_run);
    int status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *error = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        if (!error) error = "runtime failure";
        size_t len = strlen(error);
        if (len > TNY_CODE_OUTPUT_BYTES - 13) len = TNY_CODE_OUTPUT_BYTES - 13;
        memcpy(s.output, "error: code: ", 13);
        memcpy(s.output + 13, error, len);
        s.output[13 + len] = 0;
    }
    /* Finalizers cannot acquire host capabilities; close under the same bound. */
    lua_close(L);
    yyjson_doc_free(s.doc);
    yyjson_mut_doc_free(s.mut);
    free(s.pending);
    json_free(&s, s.serialized);
    return s.output;
}

char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata) {
    if (timeout_ms <= 0) timeout_ms = TNY_CODE_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > TNY_CODE_MAX_TIMEOUT_MS) timeout_ms = TNY_CODE_MAX_TIMEOUT_MS;
    int64_t deadline = monotonic_ms() + timeout_ms;
    return tny_code_run_with_deadline(code, &deadline, catalog_json, call, userdata);
}
