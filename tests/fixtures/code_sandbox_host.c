/* Native OS enforcement probe. Execute as a fresh native process: Valgrind
 * emulates syscalls and cannot establish the behavior of kernel confinement.
 * This mandatory gate accompanies, not replaces, the instrumented unit suite. */
#include "util/code_sandbox.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int ordinary = open("/dev/null", O_RDONLY);
    if (ordinary < 0) return 61;
    close(ordinary);
    ordinary = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ordinary < 0) return 62;
    close(ordinary);
    pid_t control = fork();
    if (control < 0) return 63;
    if (!control) _exit(0);
    int status = 0;
    pid_t got;
    do { got = waitpid(control, &status, 0); } while (got < 0 && errno == EINTR);
    if (got != control || !WIFEXITED(status) || WEXITSTATUS(status)) return 64;

    if (tny_code_sandbox_limits(3) || tny_code_sandbox_enter()) return 71;
    int denied_file = open("/dev/null", O_RDONLY);
    if (denied_file >= 0) {
        close(denied_file);
        return 72;
    }
    int denied_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (denied_socket >= 0) {
        close(denied_socket);
        return 73;
    }
    pid_t denied_process = fork();
    if (!denied_process) _exit(74);
    if (denied_process > 0) {
        do { got = waitpid(denied_process, &status, 0); } while (got < 0 && errno == EINTR);
        return 75;
    }
    return 0;
}
