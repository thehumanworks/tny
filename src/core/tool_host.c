#include "core/tool_host.h"

#include "core/code_python.h"
#include "tny_python_pkg.h"

#include "util/util.h"

#include <stdio.h>
#include <string.h>

int tny_tool_host_main(int argc, char **argv) {
    enum { N = sizeof tny_python_package / sizeof tny_python_package[0] };
    tny_python_module modules[N];
    for (size_t i = 0; i < N; i++) {
        modules[i].name = tny_python_package[i].name;
        modules[i].is_package = tny_python_package[i].is_package != 0;
        modules[i].source = (const char *)tny_python_package[i].source;
    }
    int rc = tny_code_python_host_main(modules, N, argc, argv);
    fflush(stdout);
    return rc;
}

uint64_t tny_tool_host_package_hash(void) {
    uint64_t h = 0;
    for (size_t i = 0; i < sizeof tny_python_package / sizeof tny_python_package[0]; i++) {
        const char *src = (const char *)tny_python_package[i].source;
        h = h * 1099511628211ULL ^ fnv1a(src, strlen(src));
    }
    return h;
}
