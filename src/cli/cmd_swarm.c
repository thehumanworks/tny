#include "cli/cli.h"
#include "core/swarm_manifest.h"
#include "util/image_io.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_swarm(const cli_globals *g, int argc, char **argv) {
    if (argc < 2 || strcmp(argv[0], "validate") != 0) {
        fputs("Usage: tny swarm validate FILE [--json]\n", stderr);
        return 1;
    }
    const char *path = argv[1];
    bool json = g && g->json;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0 && !json) json = true;
        else {
            fputs("Usage: tny swarm validate FILE [--json]\n", stderr);
            return 1;
        }
    }
    char *source = path_abs(path);
    tny_swarm_manifest *manifest = NULL;
    char err[256];
    if (!source || tny_swarm_manifest_parse_file(source, TNY_SWARM_MANIFEST_MAX_PARTICIPANTS,
                                                 &manifest, err, sizeof err) != 0) {
        fprintf(stderr, "tny: invalid swarm file: %s\n", source ? err : "invalid path");
        free(source);
        return 1;
    }
    char digest[65];
    if (!tny_image_io_sha256_hex(manifest->canonical_json, manifest->canonical_len, digest)) {
        fputs("tny: could not hash swarm definition\n", stderr);
        tny_swarm_manifest_free(manifest);
        free(source);
        return 1;
    }
    if (json) {
        unsigned max_depth = 0;
        for (size_t i = 0; i < manifest->group_count; ++i)
            if (manifest->groups[i].depth > max_depth) max_depth = manifest->groups[i].depth;
        buf_t out = {0};
        buf_appendf(&out,
                    "{\"kind\":\"swarm_definition\",\"valid\":true,\"version\":%u,"
                    "\"source\":",
                    manifest->version);
        jescape(&out, source);
        buf_appends(&out, ",\"sha256\":");
        jescape(&out, digest);
        buf_appendf(&out,
                    ",\"participants\":%zu,\"groups\":%zu,\"max_depth\":%u,"
                    "\"purpose\":",
                    manifest->participant_count, manifest->group_count, max_depth);
        jescape(&out, manifest->groups[0].purpose);
        buf_appends(&out, ",\"coordinator\":");
        jescape(&out, manifest->groups[0].coordinator_name);
        buf_appends(&out, "}\n");
        if (out.data) fputs(out.data, stdout);
        buf_free(&out);
    } else {
        printf("valid swarm definition: %zu participant%s, %zu group%s, sha256 %s\n",
               manifest->participant_count, manifest->participant_count == 1 ? "" : "s",
               manifest->group_count, manifest->group_count == 1 ? "" : "s", digest);
    }
    tny_swarm_manifest_free(manifest);
    free(source);
    return 0;
}
