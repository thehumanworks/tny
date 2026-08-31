/* Controlled benchmark entry point for the otherwise internal broker.
 * tt_broker_run writes one readiness byte to stdout; the Python harness owns
 * this process and terminates it after each isolated measurement. */
#include "broker/broker.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: bench-broker SOCKET\n");
        return 2;
    }
    return tt_broker_run(argv[1], STDOUT_FILENO);
}
