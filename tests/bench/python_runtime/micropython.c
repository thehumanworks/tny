/* Benchmark-only MicroPython adapter. Runtime json module; outer watchdog only. */
#include "bench.h"
#include "port/micropython_embed.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bench_state *active;
static bool overflow;
static char heap[16 * 1024 * 1024];
void tny_mp_out(const char *text, size_t len) {
    if (!bench_append(active, text, len)) overflow = true;
}
static mp_obj_t tools_call(mp_obj_t name, mp_obj_t args) {
    char *result = bench_call(active, mp_obj_str_get_str(name), mp_obj_str_get_str(args));
    if (!result) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("tool callback failed"));
    mp_obj_t value = mp_obj_new_str(result, strlen(result));
    free(result);
    return value;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tools_call_obj, tools_call);
static mp_obj_t tools_list(void) {
    return mp_obj_new_str(active->catalog, strlen(active->catalog));
}
static MP_DEFINE_CONST_FUN_OBJ_0(tools_list_obj, tools_list);
static mp_obj_t tools_describe(mp_obj_t name) {
    char *result = bench_describe(active, mp_obj_str_get_str(name));
    if (!result) return mp_const_none;
    mp_obj_t value = mp_obj_new_str(result, strlen(result));
    free(result);
    return value;
}
static MP_DEFINE_CONST_FUN_OBJ_1(tools_describe_obj, tools_describe);
static void exec_str(const char *src) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str("code"), src, strlen(src), 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_call_function_0(mp_compile(&tree, source_name, false));
}
static void err_strn(void *env, const char *str, size_t len) {
    (void)env;
    (void)bench_append(active, str, len);
}
bool bench_execute(bench_state *s, const char *code) {
    int stack_top;
    active = s;
    mp_embed_init(heap, sizeof(heap), &stack_top);
    mp_cstack_init_with_top(&stack_top, 1024 * 1024);
    bool ok = true;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t tools = mp_obj_new_module(qstr_from_str("tools"));
        mp_store_attr(tools, qstr_from_str("call"), MP_OBJ_FROM_PTR(&tools_call_obj));
        mp_store_attr(tools, qstr_from_str("list"), MP_OBJ_FROM_PTR(&tools_list_obj));
        mp_store_attr(tools, qstr_from_str("describe"), MP_OBJ_FROM_PTR(&tools_describe_obj));
        mp_store_global(qstr_from_str("tools"), tools);
        exec_str("import json\n");
        exec_str(code);
        nlr_pop();
    } else {
        mp_print_t print = {NULL, err_strn};
        (void)bench_append(s, "error: ", 7);
        mp_obj_print_exception(&print, (mp_obj_t)nlr.ret_val);
        ok = false;
    }
    mp_embed_deinit();
    active = NULL;
    return ok && !overflow;
}
