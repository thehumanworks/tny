/* ACP process seam: WebAssembly cannot launch external agents. */
#include "backends/acp/acp_client.h"
#include <stdio.h>

bool ac_platform_supported(void) { return false; }
bool ac_on_path(const char *bin) {
    (void)bin;
    return false;
}
int ac_spawn_agent(ac_impl *o, char *errbuf, size_t errlen) {
    (void)o;
    snprintf(errbuf, errlen, "acp: external agent processes are unsupported on WebAssembly");
    return -1;
}
int ac_reap_agent(ac_impl *o) {
    (void)o;
    return -1;
}
int ac_pump_reads(ac_impl *o) {
    (void)o;
    return -1;
}
int ac_transport_pollfds(ac_impl *o, struct pollfd *fds, int max) {
    (void)o;
    (void)fds;
    (void)max;
    return 0;
}
int ac_tx_request(ac_impl *o, int64_t id, const char *method, const char *params) {
    (void)o;
    (void)id;
    (void)method;
    (void)params;
    return -1;
}
int ac_tx_notify(ac_impl *o, const char *method, const char *params) {
    (void)o;
    (void)method;
    (void)params;
    return -1;
}
int ac_tx_result(ac_impl *o, const char *id, const char *result) {
    (void)o;
    (void)id;
    (void)result;
    return -1;
}
int ac_tx_error(ac_impl *o, const char *id, int code, const char *message) {
    (void)o;
    (void)id;
    (void)code;
    (void)message;
    return -1;
}
yyjson_doc *ac_rpc(ac_impl *o, const char *method, const char *params, char *errbuf,
                   size_t errlen) {
    (void)o;
    (void)method;
    (void)params;
    snprintf(errbuf, errlen, "acp: external agent processes are unsupported on WebAssembly");
    return NULL;
}
