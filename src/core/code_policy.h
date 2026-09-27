/* code_policy.h — pure, loop-free Python code-cell gates (docs/adr/0179).
 * tests/formal/check_code_policy.py translates these exact definitions through
 * the Clang AST into Lean and proves their properties; keep them integer and
 * Boolean only (no enums, pointers, loops or calls) so the translator can
 * accept them. The interpreter, parent and child enforce limits through these
 * functions rather than private copies. */
#ifndef TNY_CODE_POLICY_H
#define TNY_CODE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Parent-side cell protocol frame types (first payload byte). */
#define TNY_CODE_FRAME_START  83 /* 'S' parent -> child: catalog length, catalog, code */
#define TNY_CODE_FRAME_CALL   67 /* 'C' child -> parent: name '\n' JSON object arguments */
#define TNY_CODE_FRAME_RESULT 82 /* 'R' parent -> child: tool result text */
#define TNY_CODE_FRAME_FAIL   70 /* 'F' parent -> child: terminal failure, no more calls */
#define TNY_CODE_FRAME_DONE   68 /* 'D' child -> parent: final cell output */

/* Parent protocol phases. */
#define TNY_CODE_PHASE_RUNNING  1 /* waiting for the child's next CALL or DONE */
#define TNY_CODE_PHASE_FINISHED 2 /* DONE received: only EOF may follow */
#define TNY_CODE_PHASE_FAILED   3 /* terminal: nothing further is admitted */

/* JSON encoding kinds, in the facade's classification order. */
#define TNY_CODE_JSON_NULL    0
#define TNY_CODE_JSON_BOOL    1
#define TNY_CODE_JSON_INT     2
#define TNY_CODE_JSON_FLOAT   3
#define TNY_CODE_JSON_STRING  4
#define TNY_CODE_JSON_ARRAY   5
#define TNY_CODE_JSON_OBJECT  6
#define TNY_CODE_JSON_DEFAULT 7 /* unsupported: default= hook or TypeError */

bool tny_code_timeout_admit(int64_t timeout_ms);
bool tny_code_source_admit(uint64_t bytes);
/* A nested call is admitted only before the call budget is spent, with a
 * 1..256-byte name that is not run_code and a bounded JSON-object argument. */
bool tny_code_call_admit(uint64_t calls_done, uint64_t name_bytes, bool recursive,
                         uint64_t argument_bytes, bool argument_is_object);
bool tny_code_result_admit(uint64_t result_bytes);
/* Appending `add` printed bytes to `used` stays within the output limit. */
bool tny_code_output_admit(uint64_t used, uint64_t add);
/* Allocation accounting without overflow: `request` plus a header fits the
 * remaining heap budget below `limit`. */
bool tny_code_memory_admit(uint64_t used, uint64_t request, uint64_t header, uint64_t limit);
/* Parent admission of a child frame. FAILED and FINISHED admit nothing. */
bool tny_code_frame_admit(int phase, int type, uint64_t payload_bytes, uint64_t calls_done);
/* Python values map to JSON kinds with bool checked before int (bool is an
 * int subclass) and None before everything: false never becomes 0. */
int tny_code_json_kind(bool is_none, bool is_bool, bool is_int, bool is_float, bool is_str,
                       bool is_list_or_tuple, bool is_dict);

#endif
