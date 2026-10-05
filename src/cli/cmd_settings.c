/* UI settings are usable without a conversation provider or credentials. */
#include "cli/cli.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_settings(const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *args[3] = {0};
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (n < 3) args[n++] = argv[i];
        else n = 4;
    }
    bool get = n == 2 && strcmp(args[0], "get") == 0;
    bool set = n == 3 && strcmp(args[0], "set") == 0;
    if (n && !get && !set) {
        fputs("tny: settings: usage: tny settings [get KEY | set KEY VALUE] [--json]\n", stderr);
        return 1;
    }
    const char *key = n ? args[1] : NULL;
    bool mode = key && strcmp(key, "ui.mode") == 0;
    if (key && !mode && strcmp(key, "ui.alternate_screen") != 0) {
        fputs("tny: settings: key must be ui.mode or ui.alternate_screen\n", stderr);
        return 1;
    }
    if (set && (mode ? strcmp(args[2], "inline") != 0 && strcmp(args[2], "fullscreen") != 0
                     : strcmp(args[2], "true") != 0 && strcmp(args[2], "false") != 0)) {
        fputs(mode ? "tny: settings: ui.mode must be inline or fullscreen\n"
                   : "tny: settings: ui.alternate_screen must be true or false\n",
              stderr);
        return 1;
    }

    tny_ctx ctx = {0};
    ctx.tny_dir = path_tny_dir();
    ctx.settings_path = ctx.tny_dir ? path_join(ctx.tny_dir, "settings.json") : NULL;
    int rc = 1;
    if (!ctx.settings_path) goto done;
    ctx.settings = jparse_file(ctx.settings_path);
    if ((!ctx.settings && file_exists(ctx.settings_path)) ||
        (ctx.settings && !yyjson_is_obj(yyjson_doc_get_root(ctx.settings)))) {
        fputs("tny: settings: settings.json must contain a JSON object\n", stderr);
        goto done;
    }
    if (set && tny_settings_set_ui(&ctx, mode ? "mode" : "alternate_screen", args[2]) != 0) {
        fputs("tny: settings: could not save UI settings\n", stderr);
        goto done;
    }
    const char *ui_mode = tny_settings_ui_mode(&ctx);
    const char *alternate = tny_settings_ui_alternate_screen(&ctx) ? "true" : "false";
    if (!key) {
        if (json)
            printf("{\"kind\":\"settings\",\"ui\":{\"mode\":\"%s\",\"alternate_screen\":%s}}\n",
                   ui_mode, alternate);
        else printf("ui.mode = %s\nui.alternate_screen = %s\n", ui_mode, alternate);
    } else if (json) {
        printf("{\"kind\":\"setting\",\"key\":\"%s\",\"value\":", key);
        if (mode) printf("\"%s\"", ui_mode);
        else fputs(alternate, stdout);
        fputs("}\n", stdout);
    } else printf("%s\n", mode ? ui_mode : alternate);
    rc = ferror(stdout) ? 1 : 0;
done:
    yyjson_doc_free(ctx.settings);
    free(ctx.settings_path);
    free(ctx.tny_dir);
    return rc;
}
