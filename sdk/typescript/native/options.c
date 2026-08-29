#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int get_named(napi_env env, napi_value object, const char *name, napi_value *out) {
    bool has = false;
    napi_status status;
    *out = NULL;
    status = napi_has_named_property(env, object, name, &has);
    if (status != napi_ok) return -1;
    if (!has) return 0;
    status = napi_get_named_property(env, object, name, out);
    if (status != napi_ok) {
        *out = NULL;
        return -1;
    }
    return 1;
}

static int read_string_limited(napi_env env, napi_value value, size_t max_utf8_bytes,
                               const char *limit_message, sdk_owned_bytes *out) {
    napi_valuetype type = napi_undefined;
    size_t utf8_len = 0u, utf16_len = 0u, written = 0u, index;
    char16_t *units;
    char *copy;
    memset(out, 0, sizeof(*out));
    if (napi_typeof(env, value, &type) != napi_ok) return 0;
    if (type != napi_string) {
        (void)napi_throw_type_error(env, NULL, "expected string");
        return 0;
    }
    if (napi_get_value_string_utf8(env, value, NULL, 0u, &utf8_len) != napi_ok) return 0;
    if (utf8_len > max_utf8_bytes) {
        (void)napi_throw_range_error(env, NULL, limit_message);
        return 0;
    }
    if (napi_get_value_string_utf16(env, value, NULL, 0u, &utf16_len) != napi_ok) return 0;
    if (utf16_len > SIZE_MAX / sizeof(*units) - 1u) {
        (void)napi_throw_range_error(env, NULL, "string is too large");
        return 0;
    }
    units = (char16_t *)malloc((utf16_len + 1u) * sizeof(*units));
    if (!units) {
        (void)napi_throw_error(env, NULL, "out of memory");
        return 0;
    }
    if (napi_get_value_string_utf16(env, value, units, utf16_len + 1u, &written) != napi_ok ||
        written != utf16_len) {
        free(units);
        return 0;
    }
    for (index = 0u; index < utf16_len; index++) {
        uint16_t unit = (uint16_t)units[index];
        if (unit == 0u || (unit >= 0xdc00u && unit <= 0xdfffu) ||
            (unit >= 0xd800u && unit <= 0xdbffu &&
             (index + 1u >= utf16_len || (uint16_t)units[++index] < 0xdc00u ||
              (uint16_t)units[index] > 0xdfffu))) {
            free(units);
            (void)napi_throw_type_error(env, NULL,
                                        "string must be valid UTF-8 without embedded NUL");
            return 0;
        }
    }
    free(units);
    copy = (char *)malloc(utf8_len + 1u);
    if (!copy) {
        (void)napi_throw_error(env, NULL, "out of memory");
        return 0;
    }
    if (napi_get_value_string_utf8(env, value, copy, utf8_len + 1u, &written) != napi_ok ||
        written != utf8_len || memchr(copy, '\0', utf8_len) != NULL) {
        free(copy);
        (void)napi_throw_type_error(env, NULL, "string must be valid UTF-8 without embedded NUL");
        return 0;
    }
    out->ptr = copy;
    out->len = (uint64_t)utf8_len;
    return 1;
}

static int read_string(napi_env env, napi_value value, sdk_owned_bytes *out) {
    return read_string_limited(env, value, SIZE_MAX, "string is too large", out);
}

static int get_string(napi_env env, napi_value object, const char *name, int required,
                      sdk_owned_bytes *out) {
    napi_value value;
    int found = get_named(env, object, name, &value);
    memset(out, 0, sizeof(*out));
    if (found < 0) return 0;
    if (found == 0) {
        if (required) {
            (void)napi_throw_type_error(env, NULL, "required string option is missing");
            return 0;
        }
        memset(out, 0, sizeof(*out));
        return 1;
    }
    return read_string(env, value, out);
}

static int get_string_limited(napi_env env, napi_value object, const char *name, int required,
                              size_t max_utf8_bytes, const char *limit_message,
                              sdk_owned_bytes *out) {
    napi_value value;
    int found = get_named(env, object, name, &value);
    memset(out, 0, sizeof(*out));
    if (found < 0) return 0;
    if (found == 0) {
        if (required) {
            (void)napi_throw_type_error(env, NULL, "required string option is missing");
            return 0;
        }
        return 1;
    }
    return read_string_limited(env, value, max_utf8_bytes, limit_message, out);
}

static int get_uint32(napi_env env, napi_value object, const char *name, uint32_t fallback,
                      uint32_t *out) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    double number = 0.0;
    int found = get_named(env, object, name, &value);
    *out = 0u;
    if (found < 0) return 0;
    if (found == 0) {
        *out = fallback;
        return 1;
    }
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_number ||
        napi_get_value_double(env, value, &number) != napi_ok || !isfinite(number) ||
        number < 0.0 || number > (double)UINT32_MAX || floor(number) != number) {
        (void)napi_throw_type_error(env, NULL, "expected unsigned integer option");
        return 0;
    }
    *out = (uint32_t)number;
    return 1;
}

static int get_uint64(napi_env env, napi_value object, const char *name, uint64_t fallback,
                      uint64_t *out) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    int found = get_named(env, object, name, &value);
    *out = 0u;
    if (found < 0) return 0;
    if (found == 0) {
        *out = fallback;
        return 1;
    }
    if (napi_typeof(env, value, &type) != napi_ok) return 0;
    if (type == napi_bigint) {
        uint64_t result;
        bool lossless;
        if (napi_get_value_bigint_uint64(env, value, &result, &lossless) == napi_ok && lossless) {
            *out = result;
            return 1;
        }
    } else if (type == napi_number) {
        double number;
        if (napi_get_value_double(env, value, &number) == napi_ok && isfinite(number) &&
            number >= 0.0 && number <= 9007199254740991.0 && floor(number) == number) {
            *out = (uint64_t)number;
            return 1;
        }
    }
    (void)napi_throw_type_error(env, NULL, "expected non-negative exact bigint or number option");
    return 0;
}

static int get_bool(napi_env env, napi_value object, const char *name, int fallback, int *out) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    bool result;
    int found = get_named(env, object, name, &value);
    *out = 0;
    if (found < 0) return 0;
    if (found == 0) {
        *out = fallback;
        return 1;
    }
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_boolean ||
        napi_get_value_bool(env, value, &result) != napi_ok) {
        (void)napi_throw_type_error(env, NULL, "expected boolean option");
        return 0;
    }
    *out = result ? 1 : 0;
    return 1;
}

int sdk_arg_uint32(napi_env env, napi_value value, uint32_t *out) {
    napi_valuetype type = napi_undefined;
    double number = 0.0;
    *out = 0u;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_number ||
        napi_get_value_double(env, value, &number) != napi_ok || !isfinite(number) ||
        number < 0.0 || number > (double)UINT32_MAX || floor(number) != number) {
        (void)napi_throw_type_error(env, NULL, "expected native handle id");
        return 0;
    }
    *out = (uint32_t)number;
    return 1;
}

int sdk_arg_string(napi_env env, napi_value value, sdk_owned_bytes *out) {
    return read_string(env, value, out);
}

static int task_name_valid(const sdk_owned_bytes *name) {
    if (!name->ptr || name->len == 0u || name->len > 63u || name->ptr[0] == '.') return 0;
    for (uint64_t i = 0u; i < name->len; i++) {
        unsigned char c = (unsigned char)name->ptr[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return 0;
        if (c == '.' && i + 1u < name->len && name->ptr[i + 1u] == '.') return 0;
    }
    return 1;
}

int sdk_parse_create_options(napi_env env, napi_value object, create_options *options) {
    int persistence;
    napi_value task;
    napi_valuetype task_type = napi_undefined;
    if (!get_string(env, object, "workspace", 1, &options->workspace) ||
        !get_string(env, object, "stateDir", 0, &options->state_dir) ||
        !get_string(env, object, "provider", 0, &options->provider) ||
        !get_string(env, object, "model", 0, &options->model) ||
        !get_string(env, object, "baseUrl", 0, &options->base_url) ||
        !get_string(env, object, "apiKey", 0, &options->api_key) ||
        !get_string(env, object, "wireApi", 0, &options->wire_api) ||
        !get_uint32(env, object, "permissionMode", TNY_PERMISSION_ASK, &options->permission_mode) ||
        !get_bool(env, object, "persistence", 0, &persistence) ||
        !get_uint32(env, object, "maxSteps", 0u, &options->max_steps) ||
        !get_uint64(env, object, "maxToolResultBytes", 32768u, &options->max_tool_result_bytes))
        return 0;
    if (options->max_steps > INT_MAX) {
        (void)napi_throw_range_error(env, NULL, "maxSteps must be at most INT_MAX");
        return 0;
    }
    options->persistence = persistence ? 1u : 0u;
    int task_found = get_named(env, object, "taskPreset", &task);
    if (task_found < 0) return 0;
    if (task_found > 0) {
        if (napi_typeof(env, task, &task_type) != napi_ok) return 0;
        if (task_type == napi_string) {
            if (!read_string_limited(
                    env, task, 63u,
                    "taskPreset name must match [A-Za-z0-9_.-]{1,63} without a leading dot or '..'",
                    &options->task_name))
                return 0;
        } else if (task_type == napi_object) {
            if (!get_string_limited(
                    env, task, "name", 1, 63u,
                    "taskPreset name must match [A-Za-z0-9_.-]{1,63} without a leading dot or '..'",
                    &options->task_name) ||
                !get_string_limited(env, task, "instructions", 0, 262144u,
                                    "taskPreset instructions must be at most 262144 UTF-8 bytes",
                                    &options->task_instructions))
                return 0;
        } else {
            (void)napi_throw_type_error(
                env, NULL, "taskPreset must be a preset name or { name, instructions }");
            return 0;
        }
        if (!task_name_valid(&options->task_name)) {
            (void)napi_throw_type_error(
                env, NULL,
                "taskPreset name must match [A-Za-z0-9_.-]{1,63} without a leading dot or '..'");
            return 0;
        }
        if (options->task_instructions.len > 262144u) {
            (void)napi_throw_range_error(
                env, NULL, "taskPreset instructions must be at most 262144 UTF-8 bytes");
            return 0;
        }
        options->task_set = 1;
    }
    return 1;
}
