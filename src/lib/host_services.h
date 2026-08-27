#ifndef TNY_LIB_HOST_SERVICES_H
#define TNY_LIB_HOST_SERVICES_H

#include "tny/tny.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tny_host_services_state tny_host_services_state;

int32_t tny_host_services_copy(const tny_host_services_v1 *source, tny_host_services_state **out);
void tny_host_services_free(tny_host_services_state *state);
void tny_host_services_close(tny_host_services_state *state);
bool tny_host_services_in_callback(const tny_host_services_state *state);

void tny_host_services_diagnostic(tny_host_services_state *state, uint32_t level,
                                  const char *message);
int32_t tny_host_services_monotonic(tny_host_services_state *state, int64_t *out_ms);
int64_t tny_host_services_monotonic_or_native(tny_host_services_state *state);
int32_t tny_host_services_random(tny_host_services_state *state, void *buffer, uint64_t size);
int32_t tny_host_services_storage_load(tny_host_services_state *state, tny_bytes key,
                                       uint64_t *out_revision, void *buffer, uint64_t capacity,
                                       uint64_t *out_size);
int32_t tny_host_services_storage_store(tny_host_services_state *state, tny_bytes key,
                                        uint64_t expected_revision, const void *data, uint64_t size,
                                        uint64_t *out_revision);
int32_t tny_host_services_open_url(tny_host_services_state *state, tny_bytes url);
int32_t tny_host_services_notify(tny_host_services_state *state);
void tny_host_services_notify_advisory(tny_host_services_state *state);

#endif
