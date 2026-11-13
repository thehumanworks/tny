/* Private tool worker, not a CLI: inference and sessions stay in libtny. */
#include "core/code_runtime.h"
#include "core/execution.h"
#include "core/execution_parallel.h"
#include "util/execution_command.h"
#include "util/execution_host.h"
#include "util/process.h"
#include "util/util.h"
#include "tny_version.h"
#include <string.h>

int main(int argc, char **argv) {
    if (tny_process_scope_admit() != 0 || argc != 2) return 2;
    if (strcmp(argv[1], "--exec-server") == 0) {
        int fd = tny_exec_host_accept();
        if (fd < 0 ||
            tny_exec_host_send(fd, "{\"tny_sdk_executor\":1,\"version\":\"" TNY_VERSION "\"}",
                               monotonic_ms() + 5000, NULL, NULL))
            return 2;
        return tny_execution_embedded_server_main();
    }
    if (strcmp(argv[1], "--exec-tool") == 0) return tny_execution_tool_main();
    if (strcmp(argv[1], "--exec-command") == 0) return tny_exec_command_main();
    return 2;
}
