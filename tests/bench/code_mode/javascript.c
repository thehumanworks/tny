/* Experimental embedding probe, not a production replacement. No quickjs-libc. */
#include "bench.h"
#include "quickjs.h"
#include <stdlib.h>
#include <string.h>
static int interrupted(JSRuntime *rt, void *opaque) {
    (void)rt;
    return bench_clock_ns() >= ((bench_state *)opaque)->deadline;
}
static JSValue call(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "tools.call requires name and JSON argument strings");
    size_t nlen, alen;
    const char *name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    const char *args = JS_ToCStringLen(ctx, &alen, argv[1]);
    if (!name || !args) { JS_FreeCString(ctx, name); JS_FreeCString(ctx, args); return JS_EXCEPTION; }
    char *out = NULL;
    if (nlen && nlen <= 256 && alen <= 262144 && strlen(name) == nlen && strlen(args) == alen)
        out = bench_call(JS_GetContextOpaque(ctx), name, args);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, args);
    if (!out) return JS_ThrowTypeError(ctx, "invalid call arguments");
    JSValue value = JS_NewString(ctx, out);
    free(out);
    return value;
}
static JSValue list(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return JS_NewString(ctx, ((bench_state *)JS_GetContextOpaque(ctx))->catalog);
}
static JSValue describe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc != 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "tool name required");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char *out = bench_describe(JS_GetContextOpaque(ctx), name);
    JS_FreeCString(ctx, name);
    JSValue value = out ? JS_NewString(ctx, out) : JS_NULL;
    free(out);
    return value;
}
static JSValue output(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    bench_state *s = JS_GetContextOpaque(ctx);
    for (int i = 0; i < argc; ++i) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str) return JS_EXCEPTION;
        bool ok = (!i || bench_append(s, "\t", 1)) && bench_append(s, str, len);
        JS_FreeCString(ctx, str);
        if (!ok) return JS_ThrowInternalError(ctx, "output limit exceeded");
    }
    if (!bench_append(s, "\n", 1)) return JS_ThrowInternalError(ctx, "output limit exceeded");
    return JS_UNDEFINED;
}
bool bench_execute(bench_state *s, const char *code) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return false;
    JS_SetMemoryLimit(rt, 16 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 1024 * 1024);
    JS_SetInterruptHandler(rt, interrupted, s);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return false; }
    JS_SetContextOpaque(ctx, s);
    JSValue global = JS_GetGlobalObject(ctx), tools = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tools, "call", JS_NewCFunction(ctx, call, "call", 2));
    JS_SetPropertyStr(ctx, tools, "list", JS_NewCFunction(ctx, list, "list", 0));
    JS_SetPropertyStr(ctx, tools, "describe", JS_NewCFunction(ctx, describe, "describe", 1));
    JS_SetPropertyStr(ctx, global, "tools", tools);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, output, "print", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, output, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "eval", JS_UNDEFINED);
    JS_FreeValue(ctx, global);
    JSValue result = JS_Eval(ctx, code, strlen(code), "code.js", JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(result);
    if (!ok) {
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        if (message) {
            (void)bench_append(s, "error: ", 7);
            (void)bench_append(s, message, strlen(message));
            JS_FreeCString(ctx, message);
        }
        JS_FreeValue(ctx, exception);
    }
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ok;
}
