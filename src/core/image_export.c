/* image_export.c — explicit local exports and contact sheets (ADR 0094).
 *
 * Nothing in here runs unless a caller explicitly asked for an export or a
 * contact sheet. Provider generation and editing never reach this file.
 *
 * The converter is told only fixed tokens, validated numbers and tny's own
 * generated stage paths; approved source bytes are read once, hashed once and
 * copied into a private staging directory under generated ASCII names. The
 * output is accepted only after the producing invocation succeeds, a second
 * full decode of the produced bytes succeeds, and the bytes themselves report
 * exactly the requested MIME and canvas.
 */
#include "core/image_export.h"
#include "core/image.h"
#include "json/json.h"
#include "util/image_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One cell's staged input, its label, and the produced sheet. */
#define STAGE_OUTPUT "out"

typedef struct {
    char *path;            /* canonical resolved input */
    char *source_manifest; /* the record an --artifact input came from */
    char source_operation[TNY_IMAGE_OPERATION_ID_MAX];
    char expected[TNY_IMAGE_SHA256_HEX]; /* hash the record pinned, if any */
    char sha256[TNY_IMAGE_SHA256_HEX];   /* hash of the bytes actually read */
    tny_image_io_id id;
    buf_t data;
    const char *mime;
    uint32_t width, height;
} export_source;

struct tny_image_export_plan {
    tny_image_export_request request; /* only scalar flags/callback used after resolution */
    tny_image_export_settings settings;
    export_source sources[TNY_IMAGE_EXPORT_SOURCES_MAX];
    size_t count;
    char *destination;
    bool captured;
};

static bool stopped(const tny_image_export_request *r) {
    return r->cancelled && r->cancelled(r->userdata);
}

const char *tny_image_export_operation(const tny_image_export_request *r) {
    return r && r->sheet ? "contact_sheet" : "export";
}

bool tny_image_export_supported(void) {
#ifdef __EMSCRIPTEN__
    return false;
#else
    return true;
#endif
}

/* ---- option grammar ------------------------------------------------------ */

static const char *const POLICY_NAMES[] = {"fit", "crop", "pad"};
static const char *const GRAVITY_NAMES[] = {
    "center", "north", "south", "east", "west", "northeast", "northwest", "southeast", "southwest"};
/* ImageMagick's own spelling of the same nine directions. */
static const char *const GRAVITY_TOKENS[] = {
    "center", "north", "south", "east", "west", "northeast", "northwest", "southeast", "southwest"};
static const char *const FORMAT_NAMES[] = {"png", "jpeg", "webp"};
static const char *const LABEL_NAMES[] = {"none", "numbers"};

static int name_index(const char *value, const char *const *names, size_t count) {
    for (size_t i = 0; value && i < count; i++)
        if (strcmp(value, names[i]) == 0) return (int)i;
    return -1;
}

int tny_image_export_options(int argc, char **argv, tny_image_export_request *r, bool *json) {
    if (argc < 1) return 1;
    r->sheet = strcmp(argv[0], "contact-sheet") == 0;
    if (!r->sheet && strcmp(argv[0], "export") != 0) return 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) return -1;
        if (strcmp(a, "--json") == 0) {
            if (json) *json = true;
            continue;
        }
        if (strcmp(a, "--overwrite") == 0) {
            r->overwrite = true;
            continue;
        }
        if (strcmp(a, "--preview") == 0) {
            r->preview = true;
            continue;
        }
        if (strcmp(a, "--no-manifest") == 0) {
            r->no_manifest = true;
            continue;
        }
        const char **slot = strcmp(a, "--output-file") == 0  ? &r->output_file
                            : strcmp(a, "--size") == 0       ? &r->size
                            : strcmp(a, "--fit") == 0        ? &r->policy
                            : strcmp(a, "--gravity") == 0    ? &r->gravity
                            : strcmp(a, "--background") == 0 ? &r->background
                            : strcmp(a, "--format") == 0     ? &r->format
                            : strcmp(a, "--columns") == 0    ? &r->columns
                            : strcmp(a, "--labels") == 0     ? &r->labels
                                                             : NULL;
        /* --image and --artifact share one ordered list, so the command's own
         * order is the sheet's order. */
        if ((strcmp(a, "--image") == 0 || strcmp(a, "--artifact") == 0) &&
            r->source_count < TNY_IMAGE_EXPORT_SOURCES_MAX) {
            r->source_is_artifact[r->source_count] = strcmp(a, "--artifact") == 0;
            slot = &r->sources[r->source_count++];
        }
        if (!slot || *slot || i + 1 >= argc || !*argv[i + 1]) return 1;
        *slot = argv[++i];
    }
    /* Grid options belong to a sheet; a single-image export refuses them
     * rather than accepting a setting it would silently ignore. */
    if (!r->sheet && (r->columns || r->labels)) return 1;
    if (!r->source_count || !r->output_file) return 1;
    if (!r->sheet && r->source_count != 1) return 1;
    return 0;
}

/* "#RRGGBB", "#RRGGBBAA" or "transparent", normalized to lowercase. Nothing
 * else reaches the converter, so no color name, expression or coder-looking
 * string can enter its argument vector. */
static bool parse_background(const char *value, char out[TNY_IMAGE_EXPORT_COLOR_MAX]) {
    if (!value || !*value || strcmp(value, "transparent") == 0) {
        snprintf(out, TNY_IMAGE_EXPORT_COLOR_MAX, "%s", "transparent");
        return true;
    }
    size_t n = strlen(value);
    if (value[0] != '#' || (n != 7 && n != 9)) return false;
    out[0] = '#';
    for (size_t i = 1; i < n; i++) {
        char c = value[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        out[i] = c;
    }
    out[n] = 0;
    return true;
}

/* Exact decimal, no sign, no leading zero, within the grid bound. */
static bool parse_count(const char *value, uint32_t max, uint32_t *out) {
    if (!value || !*value || (value[0] == '0' && value[1])) return false;
    uint32_t n = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        if (n > (max - (uint32_t)(*p - '0')) / 10) return false;
        n = n * 10 + (uint32_t)(*p - '0');
    }
    if (!n || n > max) return false;
    *out = n;
    return true;
}

static uint32_t isqrt_ceil(uint32_t n) {
    uint32_t root = 1;
    while (root * root < n) root++;
    return root;
}

/* 5x7 numerals, scaled by an integer factor: a fixed bitmap, so a label never
 * depends on an installed font and never carries caller text. */
#define GLYPH_W 5
#define GLYPH_H 7
static const uint8_t DIGITS[10][GLYPH_H] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static uint32_t label_digits(size_t count) {
    uint32_t digits = 1;
    for (size_t n = count; n >= 10; n /= 10) digits++;
    return digits;
}

int tny_image_export_settings_resolve(const tny_image_export_request *r,
                                      tny_image_export_settings *s, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    s->sheet = r->sheet;
    uint32_t width = 0, height = 0;
    int policy = r->policy ? name_index(r->policy, POLICY_NAMES, 3) : TNY_IMAGE_POLICY_FIT;
    int gravity = r->gravity ? name_index(r->gravity, GRAVITY_NAMES, 9) : TNY_IMAGE_GRAVITY_CENTER;
    int format = r->format ? name_index(r->format, FORMAT_NAMES, 3) : TNY_IMAGE_FORMAT_PNG;
    int labels = r->labels ? name_index(r->labels, LABEL_NAMES, 2) : TNY_IMAGE_LABELS_NONE;
    if (!r->source_count || r->source_count > TNY_IMAGE_EXPORT_SOURCES_MAX ||
        (!r->sheet && r->source_count != 1)) {
        snprintf(err, errlen, "an export takes one image; a contact sheet takes 1-%u, in order",
                 (unsigned)TNY_IMAGE_EXPORT_SOURCES_MAX);
        return 1;
    }
    for (size_t i = 0; i < r->source_count; i++)
        if (!r->sources[i] || !*r->sources[i] ||
            !utf8_valid_bytes(r->sources[i], strlen(r->sources[i])) ||
            strlen(r->sources[i]) > TNY_IMAGE_PATH_MAX) {
            snprintf(err, errlen, "invalid image source path");
            return 1;
        }
    if (!r->output_file || !*r->output_file || strlen(r->output_file) > TNY_IMAGE_PATH_MAX ||
        !utf8_valid_bytes(r->output_file, strlen(r->output_file))) {
        snprintf(err, errlen, "an export needs an output file");
        return 1;
    }
    if (!tny_image_size_parse(r->size, &width, &height) || width > TNY_IMAGE_EXPORT_DIM_MAX ||
        height > TNY_IMAGE_EXPORT_DIM_MAX ||
        (uint64_t)width * height > TNY_IMAGE_EXPORT_PIXELS_MAX) {
        snprintf(err, errlen,
                 "an export needs an exact --size WIDTHxHEIGHT, each edge 1-%u and at most %u "
                 "pixels in total",
                 TNY_IMAGE_EXPORT_DIM_MAX, TNY_IMAGE_EXPORT_PIXELS_MAX);
        return 1;
    }
    if (policy < 0 || gravity < 0 || format < 0 || labels < 0) {
        snprintf(err, errlen,
                 "--fit is fit|crop|pad, --gravity one of the nine directions, --format "
                 "png|jpeg|webp, --labels none|numbers");
        return 1;
    }
    if (!parse_background(r->background, s->background)) {
        snprintf(err, errlen, "--background is transparent, #RRGGBB or #RRGGBBAA");
        return 1;
    }
    /* JPEG has no alpha: the documented default is black rather than a
     * silently opaque "transparent". */
    if (format == TNY_IMAGE_FORMAT_JPEG && strcmp(s->background, "transparent") == 0)
        snprintf(s->background, sizeof s->background, "%s", "#000000");
    s->width = width;
    s->height = height;
    s->policy = (tny_image_policy)policy;
    s->gravity = (tny_image_gravity)gravity;
    s->format = (tny_image_format)format;
    s->labels = (tny_image_labels)labels;
    if (!r->sheet) return 0;

    uint32_t columns = isqrt_ceil((uint32_t)r->source_count);
    if (r->columns && !parse_count(r->columns, (uint32_t)r->source_count, &columns)) {
        snprintf(err, errlen, "--columns is 1..the number of sources (%zu)", r->source_count);
        return 1;
    }
    s->columns = columns;
    s->rows = ((uint32_t)r->source_count + columns - 1) / columns;
    s->cell_width = width / columns;
    s->cell_height = height / s->rows;
    if (!s->cell_width || !s->cell_height) {
        snprintf(err, errlen,
                 "the canvas %ux%u cannot hold a %ux%u grid; use a larger --size or fewer columns",
                 width, height, columns, s->rows);
        return 1;
    }
    if (s->labels == TNY_IMAGE_LABELS_NUMBERS) {
        uint32_t smaller = s->cell_width < s->cell_height ? s->cell_width : s->cell_height;
        uint32_t scale = smaller / 64;
        s->label_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        s->label_width = s->label_scale * ((GLYPH_W + 1) * label_digits(r->source_count) + 1);
        s->label_height = s->label_scale * (GLYPH_H + 2);
        if (s->label_width > s->cell_width || s->label_height > s->cell_height) {
            snprintf(err, errlen,
                     "cells of %ux%u are too small for the %ux%u numeric label; use --labels none "
                     "or a larger --size",
                     s->cell_width, s->cell_height, s->label_width, s->label_height);
            return 1;
        }
    }
    return 0;
}

/* ---- sources ------------------------------------------------------------- */

static void sources_free(export_source *sources, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(sources[i].path);
        free(sources[i].source_manifest);
        buf_free(&sources[i].data);
    }
}

/* Records only: no source is opened here, so resolving what an export would
 * read never reads it. */
static int sources_resolve(const tny_image_export_request *r, export_source *sources, size_t *count,
                           char *err, size_t errlen) {
    *count = 0;
    for (size_t i = 0; i < r->source_count; i++) {
        export_source *s = &sources[(*count)++];
        buf_init(&s->data);
        if (!r->source_is_artifact[i]) {
            s->path = path_abs(r->sources[i]);
            if (!s->path) {
                snprintf(err, errlen, "cannot resolve the image source path");
                return 1;
            }
            continue;
        }
        tny_image_manifest *m = tny_image_manifest_load(r->sources[i], err, errlen);
        if (!m) return 1;
        const char *status = tny_image_manifest_observed_status(m);
        if (strcmp(status, "succeeded") != 0 || !m->committed || !m->artifact_path) {
            snprintf(err, errlen, "image manifest records no usable artifact (status %s)", status);
            tny_image_manifest_free(m);
            return 1;
        }
        s->path = tny_image_manifest_resolve(m, m->artifact_path);
        s->source_manifest = xstrdup(m->path);
        snprintf(s->source_operation, sizeof s->source_operation, "%s", m->operation_id);
        memcpy(s->expected, m->artifact_sha256, sizeof s->expected);
        tny_image_manifest_free(m);
        if (!s->path || !s->source_manifest) {
            snprintf(err, errlen, "out of memory resolving an image source");
            return 1;
        }
    }
    return 0;
}

/* Read each source exactly once and hash those very bytes. A record's pinned
 * hash is checked against what was read, never against a separate preflight. */
static int sources_load(const tny_image_export_request *r, export_source *sources, size_t count,
                        char *err, size_t errlen) {
    for (size_t i = 0; i < count; i++) {
        export_source *s = &sources[i];
        if (stopped(r)) return 130;
        if (tny_image_io_read_input(s->path, TNY_IMAGE_INPUT_MAX, &s->data, &s->id, err, errlen))
            return 1;
        s->mime = image_mime((const uint8_t *)s->data.data, s->data.len);
        if (!s->mime || strcmp(s->mime, "image/gif") == 0) {
            snprintf(err, errlen, "image sources must be PNG, JPEG or WebP");
            return 1;
        }
        if (!tny_image_io_sha256_hex(s->data.data, s->data.len, s->sha256)) {
            snprintf(err, errlen, "cannot hash an image source");
            return 1;
        }
        if (*s->expected && strcmp(s->expected, s->sha256) != 0) {
            snprintf(err, errlen,
                     "recorded source no longer matches its hash; supply the image explicitly if "
                     "this replacement is intended");
            return 1;
        }
        /* Every input bound is explicit before the converter starts. */
        if (tny_image_dimensions((const uint8_t *)s->data.data, s->data.len, &s->width,
                                 &s->height) != TNY_IMAGE_DIM_OK) {
            snprintf(err, errlen, "cannot read the dimensions of an image source");
            return 1;
        }
        if ((uint64_t)s->width * s->height > TNY_IMAGE_EXPORT_PIXELS_MAX) {
            snprintf(err, errlen, "an image source exceeds the %u pixel limit",
                     TNY_IMAGE_EXPORT_PIXELS_MAX);
            return 1;
        }
    }
    return 0;
}

int tny_image_export_inputs(const tny_image_export_request *r,
                            char *paths[TNY_IMAGE_EXPORT_SOURCES_MAX], size_t *count, char *err,
                            size_t errlen) {
    export_source sources[TNY_IMAGE_EXPORT_SOURCES_MAX] = {0};
    size_t resolved = 0;
    *count = 0;
    int rc = sources_resolve(r, sources, &resolved, err, errlen);
    for (size_t i = 0; !rc && i < resolved; i++) {
        paths[i] = sources[i].path;
        sources[i].path = NULL;
        (*count)++;
    }
    sources_free(sources, resolved);
    return rc;
}

/* ---- permission identity -------------------------------------------------- */

static void settings_json(const tny_image_export_request *r, const tny_image_export_settings *s,
                          buf_t *b) {
    buf_appendf(b, ",\"width\":%u,\"height\":%u,\"policy\":", s->width, s->height);
    jescape(b, POLICY_NAMES[s->policy]);
    buf_appends(b, ",\"gravity\":");
    jescape(b, GRAVITY_NAMES[s->gravity]);
    buf_appends(b, ",\"background\":");
    jescape(b, s->background);
    buf_appends(b, ",\"format\":");
    jescape(b, FORMAT_NAMES[s->format]);
    buf_appendf(b, ",\"overwrite\":%s,\"persist_manifest\":%s", r->overwrite ? "true" : "false",
                r->no_manifest ? "false" : "true");
    if (s->sheet) {
        buf_appendf(b, ",\"columns\":%u,\"rows\":%u,\"labels\":", s->columns, s->rows);
        jescape(b, LABEL_NAMES[s->labels]);
    } else buf_appends(b, ",\"columns\":null,\"rows\":null,\"labels\":null");
}

void tny_image_export_plan_free(tny_image_export_plan *plan) {
    if (!plan) return;
    sources_free(plan->sources, plan->count);
    free(plan->destination);
    free(plan);
}

tny_image_export_plan *tny_image_export_plan_new(const tny_image_export_request *r, char *err,
                                                 size_t errlen) {
    tny_image_export_plan *plan = calloc(1, sizeof *plan);
    if (!plan) {
        snprintf(err, errlen, "cannot allocate an export plan");
        return NULL;
    }
    if (tny_image_export_settings_resolve(r, &plan->settings, err, errlen) ||
        sources_resolve(r, plan->sources, &plan->count, err, errlen))
        goto fail;
    plan->destination = tny_image_io_canonical(r->output_file, err, errlen);
    if (!plan->destination) goto fail;
    /* No borrowed option or source strings survive resolution. */
    plan->request.sheet = r->sheet;
    plan->request.overwrite = r->overwrite;
    plan->request.no_manifest = r->no_manifest;
    plan->request.cancelled = r->cancelled;
    plan->request.userdata = r->userdata;
    return plan;
fail:
    tny_image_export_plan_free(plan);
    return NULL;
}

const char *tny_image_export_plan_input(const tny_image_export_plan *plan, size_t i,
                                        bool *artifact) {
    if (!plan || i >= plan->count) return NULL;
    *artifact = plan->sources[i].source_manifest != NULL;
    return plan->sources[i].path;
}

int tny_image_export_plan_capture(tny_image_export_plan *plan, char *err, size_t errlen) {
    if (!plan) return 1;
    if (plan->captured) return 0;
    int rc = sources_load(&plan->request, plan->sources, plan->count, err, errlen);
    if (!rc) plan->captured = true;
    if (rc == 130) snprintf(err, errlen, "the export was interrupted");
    return rc;
}

int tny_image_export_plan_detail(const tny_image_export_plan *plan, buf_t *out) {
    if (!plan || !plan->captured) return 1;
    {
        buf_appends(out, "{\"operation\":");
        jescape(out, tny_image_export_operation(&plan->request));
        buf_appends(out, ",\"output_file\":");
        jescape(out, plan->destination);
        buf_appends(out, ",\"sources\":[");
        for (size_t i = 0; i < plan->count; i++) {
            if (i) buf_appends(out, ",");
            buf_appends(out, "{\"path\":");
            jescape(out, plan->sources[i].path);
            buf_appends(out, ",\"sha256\":");
            jescape(out, plan->sources[i].sha256);
            buf_appends(out, ",\"manifest\":");
            if (plan->sources[i].source_manifest) jescape(out, plan->sources[i].source_manifest);
            else buf_appends(out, "null");
            buf_appends(out, ",\"source_operation\":");
            if (*plan->sources[i].source_operation) jescape(out, plan->sources[i].source_operation);
            else buf_appends(out, "null");
            buf_appends(out, "}");
        }
        buf_appends(out, "]");
        settings_json(&plan->request, &plan->settings, out);
        buf_appends(out, "}");
    }
    return out->oom ? 1 : 0;
}

int tny_image_export_detail(const tny_image_export_request *r, buf_t *out, char *err,
                            size_t errlen) {
    tny_image_export_plan *plan = tny_image_export_plan_new(r, err, errlen);
    int rc = plan ? tny_image_export_plan_capture(plan, err, errlen) : 1;
    if (!rc) {
        rc = tny_image_export_plan_detail(plan, out);
        if (rc) snprintf(err, errlen, "cannot format export permission identity");
    }
    tny_image_export_plan_free(plan);
    return rc;
}

/* ---- staging ------------------------------------------------------------- */

static const char *coder_for(const char *mime) {
    if (!mime) return NULL;
    if (strcmp(mime, "image/png") == 0) return "png";
    if (strcmp(mime, "image/jpeg") == 0) return "jpeg";
    if (strcmp(mime, "image/webp") == 0) return "webp";
    return NULL;
}

static const char *extension_for(const char *mime) {
    const char *coder = coder_for(mime);
    return !coder ? NULL : strcmp(coder, "jpeg") == 0 ? "jpg" : coder;
}

/* The forced output coder. PNG is written as 8-bit RGBA so a padded canvas is
 * unambiguous and independently checkable, and every format is chosen by the
 * requested --format, never guessed from the destination's extension. */
static const char *output_coder(tny_image_format format) {
    return format == TNY_IMAGE_FORMAT_PNG    ? "png32"
           : format == TNY_IMAGE_FORMAT_JPEG ? "jpeg"
                                             : "webp";
}

static const char *output_mime(tny_image_format format) {
    return format == TNY_IMAGE_FORMAT_PNG    ? "image/png"
           : format == TNY_IMAGE_FORMAT_JPEG ? "image/jpeg"
                                             : "image/webp";
}

static const char *output_extension(tny_image_format format) {
    return format == TNY_IMAGE_FORMAT_PNG    ? "png"
           : format == TNY_IMAGE_FORMAT_JPEG ? "jpg"
                                             : "webp";
}

/* A monochrome PBM holding one right-aligned decimal number, drawn from the
 * fixed glyph table: white box, black numerals, no font and no caller text. */
static bool label_bitmap(uint32_t number, uint32_t digits, uint32_t scale, buf_t *out) {
    uint32_t width = scale * ((GLYPH_W + 1) * digits + 1), height = scale * (GLYPH_H + 2);
    size_t stride = (width + 7) / 8;
    uint8_t *rows = calloc(stride, height);
    if (!rows) return false;
    for (uint32_t d = 0; d < digits; d++) {
        uint32_t place = 1;
        for (uint32_t k = 0; k + 1 < digits - d; k++) place *= 10;
        uint32_t value = (number / place) % 10;
        for (uint32_t gy = 0; gy < GLYPH_H; gy++)
            for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
                if (!(DIGITS[value][gy] & (1u << (GLYPH_W - 1 - gx)))) continue;
                for (uint32_t sy = 0; sy < scale; sy++)
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        uint32_t x = scale * (1 + d * (GLYPH_W + 1) + gx) + sx;
                        uint32_t y = scale * (1 + gy) + sy;
                        rows[(size_t)y * stride + x / 8] |= (uint8_t)(0x80u >> (x % 8));
                    }
            }
    }
    buf_appendf(out, "P4\n%u %u\n", width, height);
    buf_append(out, (const char *)rows, stride * height);
    free(rows);
    return !out->oom;
}

/* ---- argument vector ------------------------------------------------------ */

typedef struct {
    char **argv;
    size_t count, cap;
    bool failed;
} argv_builder;

static void argv_init(argv_builder *b, size_t cap) {
    b->argv = calloc(cap + 1, sizeof *b->argv);
    b->count = 0;
    b->cap = cap;
    b->failed = !b->argv;
}

static void argv_free(argv_builder *b) {
    for (size_t i = 0; b->argv && i < b->count; i++) free(b->argv[i]);
    free(b->argv);
    b->argv = NULL;
    b->count = 0;
}

/* Every token is built here and nowhere else: a fixed string, a validated
 * number, or one of tny's own stage paths with its forced coder prefix. */
static void argv_push(argv_builder *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void argv_push(argv_builder *b, const char *fmt, ...) {
    if (b->failed || b->count >= b->cap) {
        b->failed = true;
        return;
    }
    char text[TNY_IMAGE_TOOL_PATH_MAX + 128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof text) {
        b->failed = true;
        return;
    }
    b->argv[b->count] = xstrdup(text);
    if (!b->argv[b->count]) b->failed = true;
    else b->count++;
}

/* The tool's own documented cache and time ceilings, plus the parent deadline
 * in util/image_transform.c. These are resource limits, not an OS sandbox. */
static void argv_limits(argv_builder *b) {
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "memory");
    argv_push(b, "%s", "128MiB");
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "map");
    argv_push(b, "%s", "256MiB");
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "disk");
    argv_push(b, "%s", "512MiB");
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "thread");
    argv_push(b, "%s", "1");
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "time");
    argv_push(b, "%s", "60");
    argv_push(b, "%s", "-limit");
    argv_push(b, "%s", "area");
    argv_push(b, "%s", "64MP");
}

/* fit contains and pads, crop covers and crops, pad shrinks only. The suffix
 * is ImageMagick's geometry flag, and the numbers are tny's validated ones. */
static void argv_scale(argv_builder *b, const tny_image_export_settings *s, uint32_t width,
                       uint32_t height) {
    const char *suffix = s->policy == TNY_IMAGE_POLICY_CROP  ? "^"
                         : s->policy == TNY_IMAGE_POLICY_PAD ? ">"
                                                             : "";
    argv_push(b, "%s", "-strip");
    argv_push(b, "%s", "-background");
    argv_push(b, "%s", s->background);
    argv_push(b, "%s", "-gravity");
    argv_push(b, "%s", GRAVITY_TOKENS[s->gravity]);
    argv_push(b, "%s", "-resize");
    argv_push(b, "%ux%u%s", width, height, suffix);
    argv_push(b, "%s", "-extent");
    argv_push(b, "%ux%u", width, height);
    argv_push(b, "%s", "+repage");
}

static void argv_finish(argv_builder *b, const tny_image_export_settings *s, const char *out_path) {
    /* JPEG cannot carry alpha: flatten onto the resolved background instead of
     * letting the encoder invent one. */
    if (s->format == TNY_IMAGE_FORMAT_JPEG) {
        argv_push(b, "%s", "-alpha");
        argv_push(b, "%s", "remove");
        argv_push(b, "%s", "-alpha");
        argv_push(b, "%s", "off");
    }
    /* Canvas creation and flattening can introduce metadata after source
     * stripping; apply the same policy to the final encoded image. */
    argv_push(b, "%s", "-strip");
    argv_push(b, "%s:%s", output_coder(s->format), out_path);
}

/* ---- the operation -------------------------------------------------------- */

typedef struct {
    char *canonical;
    char *manifest;
    char *workspace;
    char *started, *finished;
    char *stage; /* private staging directory */
    tny_image_guard *guard;
    tny_image_commit *commit;
    bool intent;
} export_operation;

static void operation_free(export_operation *op) {
    tny_image_io_commit_close(op->commit);
    tny_image_io_guard_release(op->guard);
    tny_image_stage_remove(op->stage);
    free(op->stage);
    free(op->canonical);
    free(op->manifest);
    free(op->workspace);
    free(op->started);
    free(op->finished);
    memset(op, 0, sizeof *op);
}

static int persist(const export_operation *op, tny_image_record *record, const char *status,
                   bool initial) {
    buf_t out;
    buf_init(&out);
    record->status = status;
    record->finished = initial ? NULL : op->finished;
    tny_image_manifest_serialize(record, &out);
    int rc = out.oom   ? -1
             : initial ? tny_image_io_write_new(op->manifest, out.data, out.len)
                       : tny_image_io_replace(op->manifest, out.data, out.len);
    buf_free(&out);
    return rc;
}

/* Only locally decided text is recorded: a converter diagnostic never becomes
 * record content. */
static void safe_failure(int rc, tny_image_record *record) {
    record->error_code = rc == 130 ? TNY_IMAGE_CODE_EXPORT_CANCEL : TNY_IMAGE_CODE_EXPORT_FAILED;
    record->error_message = rc == 130 ? "the export was interrupted before an image was written"
                                      : "the export failed before an image was written";
}

/* The destination may not be a record tny writes, and may not be any source —
 * by name or by inode, with or without --overwrite. An export that consumed
 * its own destination could not preserve the original bytes it read. */
static int check_destination(const export_source *sources, size_t count, const char *canonical,
                             const tny_image_commit *commit, char *err, size_t errlen) {
    if (tny_image_manifest_reserved_name(canonical)) {
        snprintf(err, errlen, "an export may not write a reserved tny manifest file name");
        return 1;
    }
    tny_image_io_id target = tny_image_io_commit_target(commit);
    for (size_t i = 0; i < count; i++) {
        bool alias = strcmp(canonical, sources[i].path) == 0 ||
                     (target.present && sources[i].id.present && target.dev == sources[i].id.dev &&
                      target.ino == sources[i].id.ino) ||
                     tny_image_io_same_file(canonical, sources[i].path);
        if (alias) {
            snprintf(err, errlen,
                     "an export may not write to one of its own sources, even with --overwrite; "
                     "choose a new path");
            return 1;
        }
    }
    return 0;
}

static int stage_inputs(const tny_image_export_request *r, const tny_image_export_settings *s,
                        const export_operation *op, export_source *sources, size_t count, char *err,
                        size_t errlen) {
    for (size_t i = 0; i < count; i++) {
        if (stopped(r)) return 130;
        char name[TNY_IMAGE_STAGE_NAME_MAX];
        snprintf(name, sizeof name, "src%02zu.%s", i, extension_for(sources[i].mime));
        if (tny_image_stage_write(op->stage, name, sources[i].data.data, sources[i].data.len, err,
                                  errlen))
            return 1;
    }
    if (!s->sheet || s->labels != TNY_IMAGE_LABELS_NUMBERS) return 0;
    uint32_t digits = label_digits(count);
    for (size_t i = 0; i < count; i++) {
        buf_t bitmap;
        buf_init(&bitmap);
        char name[TNY_IMAGE_STAGE_NAME_MAX];
        snprintf(name, sizeof name, "lab%02zu.pbm", i);
        bool built = label_bitmap((uint32_t)i + 1, digits, s->label_scale, &bitmap);
        int rc = built
                     ? tny_image_stage_write(op->stage, name, bitmap.data, bitmap.len, err, errlen)
                     : -1;
        buf_free(&bitmap);
        if (rc) {
            if (!built) snprintf(err, errlen, "cannot build a contact-sheet label");
            return 1;
        }
    }
    return 0;
}

static int build_argv(const tny_image_tool *tool, const tny_image_export_settings *s,
                      const export_operation *op, const export_source *sources, size_t count,
                      argv_builder *b, char *err, size_t errlen) {
    char *out_path = NULL;
    char name[TNY_IMAGE_STAGE_NAME_MAX];
    snprintf(name, sizeof name, "%s.%s", STAGE_OUTPUT, output_extension(s->format));
    out_path = tny_image_stage_path(op->stage, name);
    argv_init(b, 32 + count * 24);
    argv_push(b, "%s", tool->path);
    argv_limits(b);
    if (!s->sheet) {
        char source_name[TNY_IMAGE_STAGE_NAME_MAX];
        snprintf(source_name, sizeof source_name, "src00.%s", extension_for(sources[0].mime));
        char *source_path = tny_image_stage_path(op->stage, source_name);
        if (source_path) argv_push(b, "%s:%s", coder_for(sources[0].mime), source_path);
        else b->failed = true;
        free(source_path);
        argv_scale(b, s, s->width, s->height);
    } else {
        argv_push(b, "%s", "-size");
        argv_push(b, "%ux%u", s->width, s->height);
        argv_push(b, "canvas:%s", s->background);
        for (size_t i = 0; i < count; i++) {
            char source_name[TNY_IMAGE_STAGE_NAME_MAX];
            snprintf(source_name, sizeof source_name, "src%02zu.%s", i,
                     extension_for(sources[i].mime));
            char *source_path = tny_image_stage_path(op->stage, source_name);
            uint32_t column = (uint32_t)i % s->columns, row = (uint32_t)i / s->columns;
            argv_push(b, "%s", "(");
            if (source_path) argv_push(b, "%s:%s", coder_for(sources[i].mime), source_path);
            else b->failed = true;
            free(source_path);
            argv_scale(b, s, s->cell_width, s->cell_height);
            argv_push(b, "%s", "-repage");
            argv_push(b, "+%u+%u", column * s->cell_width, row * s->cell_height);
            argv_push(b, "%s", ")");
        }
        for (size_t i = 0; s->labels == TNY_IMAGE_LABELS_NUMBERS && i < count; i++) {
            char label_name[TNY_IMAGE_STAGE_NAME_MAX];
            snprintf(label_name, sizeof label_name, "lab%02zu.pbm", i);
            char *label_path = tny_image_stage_path(op->stage, label_name);
            uint32_t column = (uint32_t)i % s->columns, row = (uint32_t)i / s->columns;
            argv_push(b, "%s", "(");
            if (label_path) argv_push(b, "pbm:%s", label_path);
            else b->failed = true;
            free(label_path);
            argv_push(b, "%s", "-repage");
            argv_push(b, "+%u+%u", column * s->cell_width, row * s->cell_height);
            argv_push(b, "%s", ")");
        }
        argv_push(b, "%s", "-background");
        argv_push(b, "%s", s->background);
        argv_push(b, "%s", "-flatten");
        argv_push(b, "%s", "+repage");
    }
    if (out_path) argv_finish(b, s, out_path);
    else b->failed = true;
    free(out_path);
    if (b->failed) {
        snprintf(err, errlen, "cannot build the image conversion command");
        return 1;
    }
    return 0;
}

/* The produced bytes must decode completely a second time through the same
 * executable, with warnings treated as failures, before anything is committed.
 * A zero exit status from the producing run proves nothing on its own. */
static int verify_output(const tny_image_tool *tool, const tny_image_export_request *r,
                         const tny_image_export_settings *s, const export_operation *op, char *err,
                         size_t errlen) {
    char name[TNY_IMAGE_STAGE_NAME_MAX];
    snprintf(name, sizeof name, "%s.%s", STAGE_OUTPUT, output_extension(s->format));
    char *path = tny_image_stage_path(op->stage, name);
    argv_builder b;
    argv_init(&b, 24);
    argv_push(&b, "%s", tool->path);
    argv_limits(&b);
    argv_push(&b, "%s", "-regard-warnings");
    if (path) argv_push(&b, "%s:%s", output_coder(s->format), path);
    else b.failed = true;
    argv_push(&b, "%s", "null:");
    free(path);
    int rc = b.failed ? 1
                      : tny_image_tool_run(tool, b.argv, op->stage, r->cancelled, r->userdata, NULL,
                                           err, errlen);
    if (b.failed) snprintf(err, errlen, "cannot build the image verification command");
    else if (rc && rc != 130)
        snprintf(err, errlen, "the exported image did not decode completely; nothing was written");
    argv_free(&b);
    return rc;
}

int tny_image_export_plan_run(const tny_ctx *ctx, tny_image_export_plan *plan,
                              tny_image_export_result *result, char *err, size_t errlen) {
    *err = 0;
    memset(result, 0, sizeof *result);
    if (!plan || !plan->captured) {
        snprintf(err, errlen, "export plan has no captured inputs");
        result->code = TNY_IMAGE_CODE_EXPORT_INVALID;
        return 1;
    }
    const tny_image_export_request *r = &plan->request;
    export_source *sources = plan->sources;
    result->settings = plan->settings;
    export_operation op = {0};
    tny_image_export_settings *s = &result->settings;
    tny_image_record record = {0};
    tny_image_transform transform = {0};
    tny_image_reference lineage[TNY_IMAGE_EXPORT_SOURCES_MAX] = {0};
    tny_image_tool tool = {0};
    argv_builder argv = {0};
    buf_t produced;
    buf_init(&produced);
    size_t count = plan->count;
    int rc = 1;
    bool terminal = false;
    /* The optional dependency is settled before a destination is touched, and
     * long before anything is staged or written. */
    rc = tny_image_tool_resolve(&tool, r->cancelled, r->userdata, err, errlen);
    if (rc) {
        if (rc != 130) result->code = TNY_IMAGE_TOOL_CODE_UNAVAILABLE;
        goto done;
    }
    rc = 1;
    snprintf(result->tool_version, sizeof result->tool_version, "%s", tool.version);
    if (stopped(r)) {
        rc = 130;
        goto done;
    }
    op.canonical = xstrdup(plan->destination);
    if (!op.canonical) goto done;
    snprintf(result->output, sizeof result->output, "%s", op.canonical);
    char *id = gen_id();
    if (!id) {
        snprintf(err, errlen, "cannot start an export");
        goto done;
    }
    snprintf(result->operation_id, sizeof result->operation_id, "%s", id);
    free(id);
    /* Own the destination before staging, so cooperating exports cannot
     * interleave their staging and commits. Inputs are already captured. */
    op.guard = tny_image_io_guard_acquire(op.canonical, result->operation_id, err, errlen);
    if (!op.guard) goto done;
    op.commit = tny_image_io_commit_open(op.canonical, r->overwrite, err, errlen);
    if (!op.commit) goto done;
    result->source_count = count;
    if (check_destination(sources, count, op.canonical, op.commit, err, errlen)) goto done;

    op.workspace = path_abs(ctx && ctx->cwd && *ctx->cwd == '/' ? ctx->cwd : ".");
    op.started = now_iso8601();
    op.manifest =
        r->no_manifest ? NULL : tny_image_manifest_path(op.canonical, result->operation_id);
    if (!op.workspace || !op.started || (!r->no_manifest && !op.manifest)) {
        snprintf(err, errlen, "cannot start an export");
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        result->source_dimensions[i] =
            (tny_image_source_dimensions){sources[i].width, sources[i].height};
        lineage[i].path = sources[i].path;
        lineage[i].source_manifest = sources[i].source_manifest;
        memcpy(lineage[i].sha256, sources[i].sha256, sizeof lineage[i].sha256);
        snprintf(lineage[i].source_operation, sizeof lineage[i].source_operation, "%s",
                 sources[i].source_operation);
    }
    char canvas[TNY_IMAGE_SIZE_MAX];
    snprintf(canvas, sizeof canvas, "%ux%u", s->width, s->height);
    transform = (tny_image_transform){.operation = tny_image_export_operation(r),
                                      .policy = POLICY_NAMES[s->policy],
                                      .gravity = GRAVITY_NAMES[s->gravity],
                                      .background = s->background,
                                      .format = FORMAT_NAMES[s->format],
                                      .labels = s->sheet ? LABEL_NAMES[s->labels] : NULL,
                                      .source_dimensions = result->source_dimensions,
                                      .width = s->width,
                                      .height = s->height,
                                      .columns = s->columns,
                                      .rows = s->rows,
                                      .cell_width = s->cell_width,
                                      .cell_height = s->cell_height,
                                      .tool = "imagemagick",
                                      .tool_version = tool.version,
                                      .sources = lineage,
                                      .source_count = count};
    record.operation_id = result->operation_id;
    record.workspace = op.workspace;
    record.started = op.started;
    record.output = op.canonical;
    record.requested_provider = "local";
    record.requested_size = canvas;
    record.effective_provider = "local";
    record.effective_size = canvas;
    record.transform = &transform;
    if (op.manifest) {
        if (persist(&op, &record, "running", true) != 0) {
            snprintf(err, errlen,
                     "cannot create the image manifest beside the output; nothing was converted "
                     "(use --no-manifest to skip provenance)");
            goto done;
        }
        op.intent = true;
        snprintf(result->manifest_path, sizeof result->manifest_path, "%s", op.manifest);
    }
    op.stage = tny_image_stage_dir(err, errlen);
    if (!op.stage) goto done;
    rc = stage_inputs(r, s, &op, sources, count, err, errlen);
    if (rc) goto done;
    rc = build_argv(&tool, s, &op, sources, count, &argv, err, errlen);
    if (rc) goto done;
    rc = tny_image_tool_run(&tool, argv.argv, op.stage, r->cancelled, r->userdata, NULL, err,
                            errlen);
    if (rc) goto done;
    rc = verify_output(&tool, r, s, &op, err, errlen);
    if (rc) goto done;
    rc = 1;
    char produced_name[TNY_IMAGE_STAGE_NAME_MAX];
    snprintf(produced_name, sizeof produced_name, "%s.%s", STAGE_OUTPUT,
             output_extension(s->format));
    if (tny_image_stage_read(op.stage, produced_name, TNY_IMAGE_OUTPUT_MAX, &produced, err, errlen))
        goto done;
    /* The bytes themselves must be the requested encoding at the exact
     * requested canvas; the converter's exit status is not evidence. */
    const char *mime = image_mime((const uint8_t *)produced.data, produced.len);
    uint32_t width = 0, height = 0;
    tny_image_dim_status dimensions =
        tny_image_dimensions((const uint8_t *)produced.data, produced.len, &width, &height);
    if (!mime || strcmp(mime, output_mime(s->format)) != 0 || dimensions != TNY_IMAGE_DIM_OK ||
        width != s->width || height != s->height) {
        snprintf(err, errlen,
                 "the converter produced %s at %ux%u instead of %s at %ux%u; nothing was written",
                 mime ? mime : "an unreadable image", width, height, output_mime(s->format),
                 s->width, s->height);
        goto done;
    }
    result->mime = output_mime(s->format);
    result->width = width;
    result->height = height;
    result->bytes = produced.len;
    if (stopped(r)) {
        rc = 130;
        goto done;
    }
    if (tny_image_io_commit_stage(op.commit, produced.data, produced.len, err, errlen)) goto done;
    if (stopped(r)) {
        rc = 130;
        goto done;
    }
    /* Hashing allocates. Finish it before the irreversible destination commit. */
    if (!tny_image_io_sha256_hex(produced.data, produced.len, result->sha256)) {
        snprintf(err, errlen, "cannot hash the exported image");
        goto done;
    }
    if (tny_image_io_commit_finish(op.commit, err, errlen)) goto done;
    rc = 0;
    result->committed = true;
    record.committed = true;
    record.mime = result->mime;
    record.width = width;
    record.height = height;
    record.bytes = produced.len;
    record.size_status = "match";
done:
    if (op.manifest) {
        op.finished = now_iso8601();
        record.output_sha256 = result->committed ? result->sha256 : NULL;
        if (rc) safe_failure(rc, &record);
        const char *status = !rc ? "succeeded" : rc == 130 ? "cancelled" : "failed";
        bool wrote = op.finished && (!record.committed || *result->sha256) &&
                     persist(&op, &record, status, false) == 0;
        terminal = wrote;
        if (!wrote && !rc) {
            /* The derived artifact is in place and paid for in real work: say
             * exactly that, at every surface, instead of reporting a failure
             * that wrote nothing. The unfinished intent stays behind so a
             * later reader sees an incomplete operation, not a fake success. */
            result->code = TNY_IMAGE_CODE_MANIFEST;
            snprintf(err, errlen,
                     "the image was exported to the requested output and kept, but its manifest "
                     "could not be finalized");
            rc = 1;
        }
    }
    if (op.intent && !terminal && !result->committed) {
        if (!op.finished) op.finished = now_iso8601();
        safe_failure(rc, &record);
        (void)persist(&op, &record, rc == 130 ? "cancelled" : "failed", false);
    }
    if (rc && rc != 130 && !result->code) result->code = TNY_IMAGE_CODE_EXPORT_FAILED;
    if (rc == 130 && !result->code) result->code = TNY_IMAGE_CODE_EXPORT_CANCEL;
    argv_free(&argv);
    buf_free(&produced);
    operation_free(&op);
    if (rc == 130) snprintf(err, errlen, "the export was interrupted");
    else if (rc && !*err) snprintf(err, errlen, "the export failed");
    return rc;
}

int tny_image_export_run(const tny_ctx *ctx, const tny_image_export_request *r,
                         tny_image_export_result *result, char *err, size_t errlen) {
    memset(result, 0, sizeof *result);
    if (!tny_image_export_supported()) {
        result->code = TNY_IMAGE_TOOL_CODE_UNAVAILABLE;
        snprintf(err, errlen, "local image transforms are unavailable on this platform");
        return 1;
    }
    tny_image_export_plan *plan = tny_image_export_plan_new(r, err, errlen);
    int rc = plan ? tny_image_export_plan_capture(plan, err, errlen) : 1;
    if (!rc) rc = tny_image_export_plan_run(ctx, plan, result, err, errlen);
    else result->code = rc == 130 ? TNY_IMAGE_CODE_EXPORT_CANCEL : TNY_IMAGE_CODE_EXPORT_INVALID;
    tny_image_export_plan_free(plan);
    return rc;
}

/* ---- results -------------------------------------------------------------- */

bool tny_image_export_retained(const tny_image_export_result *result) {
    return result && result->committed;
}

static void transform_json(const tny_image_export_request *r, const tny_image_export_result *result,
                           buf_t *out) {
    const tny_image_export_settings *s = &result->settings;
    buf_appends(out, ",\"native\":false,\"transform\":{\"operation\":");
    jescape(out, tny_image_export_operation(r));
    buf_appends(out, ",\"policy\":");
    jescape(out, POLICY_NAMES[s->policy]);
    buf_appends(out, ",\"gravity\":");
    jescape(out, GRAVITY_NAMES[s->gravity]);
    buf_appends(out, ",\"background\":");
    jescape(out, s->background);
    buf_appends(out, ",\"format\":");
    jescape(out, FORMAT_NAMES[s->format]);
    buf_appendf(out, ",\"width\":%u,\"height\":%u,\"sources\":%zu", s->width, s->height,
                result->source_count);
    buf_appends(out, ",\"source_dimensions\":[");
    for (size_t i = 0; i < result->source_count; i++) {
        if (i) buf_appends(out, ",");
        buf_appendf(out, "{\"width\":%u,\"height\":%u}", result->source_dimensions[i].width,
                    result->source_dimensions[i].height);
    }
    buf_appends(out, "]");
    if (s->sheet) {
        buf_appendf(out,
                    ",\"grid\":{\"columns\":%u,\"rows\":%u,\"cell_width\":%u,\"cell_height\":%u}",
                    s->columns, s->rows, s->cell_width, s->cell_height);
        buf_appends(out, ",\"labels\":");
        jescape(out, LABEL_NAMES[s->labels]);
    } else buf_appends(out, ",\"grid\":null,\"labels\":null");
    buf_appends(out, ",\"tool\":\"imagemagick\",\"tool_version\":");
    if (*result->tool_version) jescape(out, result->tool_version);
    else buf_appends(out, "null");
    buf_appends(out, "}");
}

static void provenance_json(const tny_image_export_result *result, buf_t *out) {
    buf_appends(out, ",\"operation_id\":");
    if (*result->operation_id) jescape(out, result->operation_id);
    else buf_appends(out, "null");
    buf_appends(out, ",\"manifest_path\":");
    if (*result->manifest_path) jescape(out, result->manifest_path);
    else buf_appends(out, "null");
    /* A local transform makes no provider request: these are null rather than
     * a local identifier wearing a provider's name. */
    buf_appends(out, ",\"seed\":null,\"request_id\":null");
}

static void committed_json(const tny_image_export_request *r, const tny_image_export_result *result,
                           buf_t *out) {
    buf_appends(out, ",\"path\":");
    jescape(out, *result->output ? result->output : r->output_file);
    buf_appends(out, ",\"sha256\":");
    if (*result->sha256) jescape(out, result->sha256);
    else buf_appends(out, "null");
    buf_appends(out, ",\"mime_type\":");
    if (result->mime) jescape(out, result->mime);
    else buf_appends(out, "null");
    buf_appendf(out, ",\"bytes\":%zu,\"width\":%u,\"height\":%u", result->bytes, result->width,
                result->height);
}

void tny_image_export_result_json(const tny_image_export_request *r,
                                  const tny_image_export_result *result, buf_t *out) {
    buf_appends(out, "{\"kind\":\"image\",\"ok\":true,\"operation\":");
    jescape(out, tny_image_export_operation(r));
    committed_json(r, result, out);
    transform_json(r, result, out);
    buf_appends(out, ",\"committed\":true");
    provenance_json(result, out);
    buf_appends(out, "}\n");
}

void tny_image_export_error_json(const tny_image_export_request *r,
                                 const tny_image_export_result *result, const char *message,
                                 buf_t *out) {
    if (tny_image_export_retained(result)) {
        tny_image_export_retained_json(r, result, message, out);
        return;
    }
    buf_appends(out, "{\"kind\":\"image\",\"ok\":false,\"operation\":");
    jescape(out, tny_image_export_operation(r));
    buf_appends(out, ",\"code\":");
    jescape(out, result->code ? result->code : TNY_IMAGE_CODE_EXPORT_FAILED);
    buf_appends(out, ",\"error\":");
    jescape(out, message ? message : "the export failed");
    /* Nothing was committed, so no path and no hash may be reported. */
    buf_appends(out, ",\"path\":null,\"sha256\":null,\"committed\":false}\n");
}

void tny_image_export_retained_json(const tny_image_export_request *r,
                                    const tny_image_export_result *result, const char *message,
                                    buf_t *out) {
    buf_appends(out, "{\"kind\":\"image\",\"ok\":false,\"operation\":");
    jescape(out, tny_image_export_operation(r));
    buf_appends(out, ",\"code\":");
    jescape(out, result->code ? result->code : TNY_IMAGE_CODE_EXPORT_FAILED);
    buf_appends(out, ",\"error\":");
    jescape(out, message ? message : "the export manifest could not be finalized");
    committed_json(r, result, out);
    transform_json(r, result, out);
    buf_appends(out, ",\"committed\":true");
    provenance_json(result, out);
    buf_appends(out, "}\n");
}
