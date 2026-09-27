/* Pure gates; see code_policy.h. Formally checked from this source. */
#include "core/code_policy.h"
#include "core/code_runtime.h"

bool tny_code_timeout_admit(int64_t timeout_ms) {
    return timeout_ms >= 1 && timeout_ms <= TNY_CODE_MAX_TIMEOUT_MS;
}

bool tny_code_source_admit(uint64_t bytes) { return bytes <= TNY_CODE_SOURCE_BYTES; }

bool tny_code_call_admit(uint64_t calls_done, uint64_t name_bytes, bool recursive,
                         uint64_t argument_bytes, bool argument_is_object) {
    return calls_done < TNY_CODE_TOOL_CALLS && name_bytes >= 1 &&
           name_bytes <= TNY_CODE_NAME_BYTES && !recursive &&
           argument_bytes <= TNY_CODE_ARGUMENT_BYTES && argument_is_object;
}

bool tny_code_result_admit(uint64_t result_bytes) {
    return result_bytes <= TNY_CODE_TOOL_RESULT_BYTES;
}

bool tny_code_output_admit(uint64_t used, uint64_t add) {
    return used <= TNY_CODE_OUTPUT_BYTES && add <= TNY_CODE_OUTPUT_BYTES - used;
}

bool tny_code_memory_admit(uint64_t used, uint64_t request, uint64_t header, uint64_t limit) {
    return used <= limit && header <= limit - used && request <= limit - used - header;
}

bool tny_code_frame_admit(int phase, int type, uint64_t payload_bytes, uint64_t calls_done) {
    return phase == TNY_CODE_PHASE_RUNNING && payload_bytes >= 1 &&
           ((type == TNY_CODE_FRAME_CALL && calls_done < TNY_CODE_TOOL_CALLS &&
             payload_bytes <= TNY_CODE_NAME_BYTES + 2 + TNY_CODE_ARGUMENT_BYTES) ||
            (type == TNY_CODE_FRAME_DONE && payload_bytes <= TNY_CODE_RESULT_TEXT_BYTES + 1));
}

int tny_code_json_kind(bool is_none, bool is_bool, bool is_int, bool is_float, bool is_str,
                       bool is_list_or_tuple, bool is_dict) {
    return is_none             ? TNY_CODE_JSON_NULL
           : is_bool           ? TNY_CODE_JSON_BOOL
           : is_int            ? TNY_CODE_JSON_INT
           : is_float          ? TNY_CODE_JSON_FLOAT
           : is_str            ? TNY_CODE_JSON_STRING
           : is_list_or_tuple  ? TNY_CODE_JSON_ARRAY
           : is_dict           ? TNY_CODE_JSON_OBJECT
                               : TNY_CODE_JSON_DEFAULT;
}
