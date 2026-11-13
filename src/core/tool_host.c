#include "core/tool_host.h"

#include "core/code_python.h"
#include "tny_python_pkg.h"

#include "json/json.h"
#include "util/util.h"

#include <stdio.h>
#include <string.h>

enum { PACKAGE_MODULES = sizeof tny_python_package / sizeof tny_python_package[0] };

const char *tny_python_module_source(const char *module) {
    for (size_t i = 0; module && i < PACKAGE_MODULES; i++)
        if (strcmp(tny_python_package[i].name, module) == 0)
            return (const char *)tny_python_package[i].source;
    return NULL;
}

char *tny_tool_host_package_json(void) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{");
    for (size_t i = 0; i < PACKAGE_MODULES; i++) {
        if (i) buf_appends(&b, ",");
        jescape(&b, tny_python_package[i].name);
        buf_appendf(&b, ":[%s,", tny_python_package[i].is_package ? "true" : "false");
        jescape(&b, (const char *)tny_python_package[i].source);
        buf_appends(&b, "]");
    }
    buf_appends(&b, "}\n");
    if (buf_oom(&b)) {
        buf_free(&b);
        return NULL;
    }
    return buf_detach(&b);
}

uint64_t tny_tool_host_package_hash(void) {
    uint64_t h = 0;
    for (size_t i = 0; i < PACKAGE_MODULES; i++) {
        const char *src = (const char *)tny_python_package[i].source;
        h = h * 1099511628211ULL ^ fnv1a(src, strlen(src));
    }
    return h;
}
