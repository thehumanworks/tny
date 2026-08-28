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

static int read_string(napi_env env, napi_value value, sdk_owned_bytes *out) {
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
    if (napi_get_value_string_utf16(env, value, NULL, 0u, &utf16_len) != napi_ok) return 0;
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
    if (napi_get_value_string_utf8(env, value, NULL, 0u, &utf8_len) != napi_ok) return 0;
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

int sdk_parse_create_options(napi_env env, napi_value object, create_options *options) {
    int persistence;
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
    return 1;
}
