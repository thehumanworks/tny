/* Strict, owned representation of a purposeful swarm definition. */
#ifndef TNY_SWARM_MANIFEST_H
#define TNY_SWARM_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#define TNY_SWARM_MANIFEST_VERSION_V1            1u
#define TNY_SWARM_MANIFEST_VERSION_V2            2u
#define TNY_SWARM_MANIFEST_VERSION               TNY_SWARM_MANIFEST_VERSION_V2
#define TNY_SWARM_MANIFEST_MAX_BYTES             (64u * 1024u)
#define TNY_SWARM_MANIFEST_MAX_NAME_BYTES        64u
#define TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES     4096u
#define TNY_SWARM_MANIFEST_MAX_DELIVERABLE_BYTES 4096u
#define TNY_SWARM_MANIFEST_MAX_ACCEPTANCE        16u
#define TNY_SWARM_MANIFEST_MAX_ACCEPTANCE_BYTES  1024u
#define TNY_SWARM_MANIFEST_MAX_DEPENDENCIES      15u
#define TNY_SWARM_MANIFEST_MAX_WORKSPACE_BASE    256u
/* The root is depth one. At most three nested group levels follow it. */
#define TNY_SWARM_MANIFEST_MAX_DEPTH        4u
#define TNY_SWARM_MANIFEST_MAX_PARTICIPANTS 16u
#define TNY_SWARM_MANIFEST_MAX_GROUPS       (TNY_SWARM_MANIFEST_MAX_PARTICIPANTS + 1u)

typedef enum {
    TNY_SWARM_WORKSPACE_SHARED_WRITABLE = 0,
    TNY_SWARM_WORKSPACE_SHARED_READ_ONLY,
    TNY_SWARM_WORKSPACE_ISOLATED,
} tny_swarm_manifest_workspace_policy;

typedef struct {
    char *deliverable;
    size_t acceptance_count;
    char *acceptance[TNY_SWARM_MANIFEST_MAX_ACCEPTANCE];
    size_t dependency_count;
    char *dependency_names[TNY_SWARM_MANIFEST_MAX_DEPENDENCIES];
    size_t dependencies[TNY_SWARM_MANIFEST_MAX_DEPENDENCIES];
    tny_swarm_manifest_workspace_policy workspace_policy;
    char *workspace_base;
    bool acceptance_declared;
    bool dependencies_declared;
    bool workspace_declared;
} tny_swarm_manifest_contract;

typedef struct {
    char *name;
    char *purpose;
    size_t group;
    bool coordinator;
    tny_swarm_manifest_contract contract;
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
    /* The root may declare deliverable/acceptance. Nested values mirror the
     * launched coordinator participant and remain owned independently. */
    tny_swarm_manifest_contract coordinator_contract;
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
const char *tny_swarm_manifest_workspace_name(tny_swarm_manifest_workspace_policy policy);

#endif
