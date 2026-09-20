/* Strict, owned representation of a purposeful swarm definition. */
#ifndef TNY_SWARM_MANIFEST_H
#define TNY_SWARM_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#define TNY_SWARM_MANIFEST_VERSION           1u
#define TNY_SWARM_MANIFEST_MAX_BYTES         (64u * 1024u)
#define TNY_SWARM_MANIFEST_MAX_NAME_BYTES    64u
#define TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES 4096u
/* The root is depth one. At most three nested group levels follow it. */
#define TNY_SWARM_MANIFEST_MAX_DEPTH        4u
#define TNY_SWARM_MANIFEST_MAX_PARTICIPANTS 16u
#define TNY_SWARM_MANIFEST_MAX_GROUPS       (TNY_SWARM_MANIFEST_MAX_PARTICIPANTS + 1u)

typedef struct {
    char *name;
    char *purpose;
    size_t group;
    bool coordinator;
} tny_swarm_manifest_participant;

typedef struct {
    char *purpose;
    char *coordinator_name;
    char *coordinator_purpose;
    /* SIZE_MAX denotes the root's absent parent/current-session coordinator. */
    size_t parent;
    /* SIZE_MAX for the root coordinator, which is the current lead. */
    size_t coordinator_participant;
    unsigned depth;
} tny_swarm_manifest_group;

typedef struct {
    unsigned version;
    size_t group_count;
    size_t participant_count;
    tny_swarm_manifest_group groups[TNY_SWARM_MANIFEST_MAX_GROUPS];
    tny_swarm_manifest_participant participants[TNY_SWARM_MANIFEST_MAX_PARTICIPANTS];
    /* Stable compact JSON, regenerated in version/purpose/coordinator/agents/swarms order. */
    char *canonical_json;
    size_t canonical_len;
} tny_swarm_manifest;

/* capacity is the shared global collaborator capacity, excluding the current
 * root lead. It must be 1..16. Parsing and all semantic checks finish before
 * a manifest is returned; callers can therefore validate before any job or
 * provider effect. `out` is left NULL on failure. */
int tny_swarm_manifest_parse(const char *bytes, size_t len, size_t capacity,
                             tny_swarm_manifest **out, char *err, size_t errlen);
int tny_swarm_manifest_parse_file(const char *path, size_t capacity, tny_swarm_manifest **out,
                                  char *err, size_t errlen);
void tny_swarm_manifest_free(tny_swarm_manifest *manifest);

#endif
