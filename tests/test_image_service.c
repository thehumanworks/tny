#include "greatest.h"
#include "core/image_export.h"
#include "core/jobs.h"
#include "core/image_provider.h"
#include "core/image_manifest.h"
#include "core/intercept.h"
#include "core/tools.h"
#include "core/tools_image.h"
#include "util/image_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static tny_ctx ctx;
static char root[256];
static bool cancelled(void *ud) {
    (void)ud;
    return true;
}

TEST image_capability_and_validation(void) {
    char err[256];
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_token = "fixture-image-token";
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_account_id = "fixture-account";
    ASSERT(tny_image_available(&ctx, NULL, false, err, sizeof err));
    ASSERT(tny_image_available(&ctx, "codex", true, err, sizeof err));
    buf_t names;
    buf_init(&names);
    ASSERT(tny_image_capabilities(&ctx, true, &names));
    ASSERT_STR_EQ("codex", names.data);
    buf_free(&names);
    ASSERT(!tny_image_available(&ctx, "grok", false, err, sizeof err));
    ctx.chatgpt_token = "bad\r\nheader";
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_token = "fixture-image-token";
    tny_image_request r = {.prompt = "hello", .output_file = "unused", .cancelled = cancelled};
    tny_image_result result;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    const char *bad[] = {NULL, "", " \t\n", "\xff"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r.prompt = bad[i];
        ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    }
    char *prompt = malloc(TNY_IMAGE_PROMPT_MAX + 2);
    ASSERT(prompt);
    memset(prompt, 'x', TNY_IMAGE_PROMPT_MAX + 1);
    prompt[TNY_IMAGE_PROMPT_MAX + 1] = 0;
    r.prompt = prompt;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    prompt[TNY_IMAGE_PROMPT_MAX] = 0;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    free(prompt);
    r.prompt = "hello";
    r.edit = true;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.images[0] = "input.png";
    r.image_count = 1;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.image_count = 5;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.image_count = 6;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.edit = false;
    r.image_count = 0;
    r.quality = "bogus";
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    const char *qualities[] = {"auto", "low", "medium", "high", "xhigh", "max"};
    for (size_t i = 0; i < sizeof qualities / sizeof qualities[0]; i++) {
        r.quality = qualities[i];
        ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    }
    PASS();
}

TEST image_base64_is_strict_and_magic_checked(void) {
    char err[256];
    const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13};
    buf_t b;
    buf_init(&b);
    b64_encode(png, sizeof png, &b);
    buf_t out;
    buf_init(&out);
    ASSERT_EQ(0, tny_image_decode(b.data, b.len, &out, err, sizeof err));
    ASSERT_EQ(sizeof png, out.len);
    ASSERT_MEM_EQ(png, out.data, sizeof png);
    buf_free(&out);
    buf_free(&b);
    const char *bad[] = {NULL,
                         "",
                         "!!!!",
                         "AAA",
                         "AAAA=AAA",
                         "iVBORw0KGgoAAAAN====",
                         "iVBORw0KGgoAAAAN\n",
                         "R0lGODlhAAAAAAAB",
                         "YWJjZGVmZ2hpamts",
                         "AB==",
                         "AAB="};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        buf_init(&out);
        ASSERT_EQ(1, tny_image_decode(bad[i], bad[i] ? strlen(bad[i]) : 0, &out, err, sizeof err));
        ASSERT_EQ(0, out.len);
        buf_free(&out);
    }
    PASS();
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define PNG_HEADER_LEN 33
static uint32_t fixture_png_crc(const uint8_t *data, size_t len) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        uint32_t byte = data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t carry = (crc ^ byte) & 1;
            crc >>= 1;
            if (carry) crc ^= UINT32_C(0xEDB88320);
            byte >>= 1;
        }
    }
    return ~crc;
}

static void make_png(uint8_t *out, uint32_t w, uint32_t h, uint8_t depth, uint8_t colour) {
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    store_be32(out + 8, 13);
    memcpy(out + 12, "IHDR", 4);
    store_be32(out + 16, w);
    store_be32(out + 20, h);
    out[24] = depth;
    out[25] = colour;
    out[26] = out[27] = out[28] = 0;
    store_be32(out + 29, fixture_png_crc(out + 12, 17));
}

/* SOI, an APP0 segment, a fill byte, one frame header and the scan marker. */
static size_t make_jpeg(uint8_t *out, uint8_t frame, uint32_t w, uint32_t h) {
    size_t i = 0;
    out[i++] = 0xFF;
    out[i++] = 0xD8;
    out[i++] = 0xFF;
    out[i++] = 0xE0;
    out[i++] = 0;
    out[i++] = 16;
    memcpy(out + i, "JFIF\0\1\1\0\0\1\0\1\0\0", 14);
    i += 14;
    out[i++] = 0xFF;
    out[i++] = 0xFF; /* legal fill byte before the frame marker */
    out[i++] = frame;
    out[i++] = 0;
    out[i++] = 17; /* 8 + 3 components */
    out[i++] = 8;
    out[i++] = (uint8_t)(h >> 8);
    out[i++] = (uint8_t)h;
    out[i++] = (uint8_t)(w >> 8);
    out[i++] = (uint8_t)w;
    out[i++] = 3;
    for (uint8_t c = 1; c <= 3; c++) {
        out[i++] = c;
        out[i++] = 0x11;
        out[i++] = 0;
    }
    out[i++] = 0xFF;
    out[i++] = 0xDA;
    return i;
}

static size_t make_webp(uint8_t *out, const char *codec, uint32_t w, uint32_t h) {
    uint32_t payload = strcmp(codec, "VP8L") == 0 ? 5 : 10;
    memcpy(out, "RIFF", 4);
    store_le32(out + 4, 4 + 8 + payload + (payload & 1));
    memcpy(out + 8, "WEBP", 4);
    memcpy(out + 12, codec, 4);
    store_le32(out + 16, payload);
    uint8_t *p = out + 20;
    memset(p, 0, payload);
    if (strcmp(codec, "VP8 ") == 0) {
        p[0] = 0x10; /* key frame: low bit clear */
        p[3] = 0x9D;
        p[4] = 0x01;
        p[5] = 0x2A;
        p[6] = (uint8_t)w;
        p[7] = (uint8_t)((w >> 8) & 0x3F);
        p[8] = (uint8_t)h;
        p[9] = (uint8_t)((h >> 8) & 0x3F);
    } else if (strcmp(codec, "VP8L") == 0) {
        p[0] = 0x2F;
        store_le32(p + 1, (w - 1) | ((h - 1) << 14));
    } else {
        store_be32(p, 0); /* VP8X flags */
        p[4] = (uint8_t)(w - 1);
        p[5] = (uint8_t)((w - 1) >> 8);
        p[6] = (uint8_t)((w - 1) >> 16);
        p[7] = (uint8_t)(h - 1);
        p[8] = (uint8_t)((h - 1) >> 8);
        p[9] = (uint8_t)((h - 1) >> 16);
    }
    if (payload & 1) out[20 + payload] = 0;
    return 20 + payload + (payload & 1);
}

/* Every truncation is either the same answer or no answer at all, and the copy
 * is exactly as long as the prefix so any overread is a real out-of-bounds read.
 * A JPEG frame header completes before the scan, so some prefixes do answer. */
static int prefixes_never_lie(const uint8_t *data, size_t n, uint32_t width, uint32_t height) {
    for (size_t cut = 0; cut < n; cut++) {
        uint8_t *copy = malloc(cut ? cut : 1);
        uint32_t w = 1, h = 1;
        if (!copy) return 0;
        memcpy(copy, data, cut);
        tny_image_dim_status status = tny_image_dimensions(copy, cut, &w, &h);
        free(copy);
        if (status == TNY_IMAGE_DIM_OK ? w != width || h != height : w || h) return 0;
    }
    return 1;
}

TEST image_dimensions_from_real_headers(void) {
    uint8_t png[PNG_HEADER_LEN], jpeg[64], webp[64];
    uint32_t w = 0, h = 0;
    make_png(png, 3440, 1440, 8, 6);
    ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(png, sizeof png, &w, &h));
    ASSERT_EQ(3440u, w);
    ASSERT_EQ(1440u, h);
    ASSERT(prefixes_never_lie(png, sizeof png, 3440, 1440));
    /* The header chunk is complete only with its length and CRC fields. */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png - 1, &w, &h));
    const uint8_t depths[] = {1, 2, 4, 8, 16}, colours[] = {0, 2, 3, 4, 6};
    for (size_t i = 0; i < sizeof depths / sizeof depths[0]; i++)
        for (size_t j = 0; j < sizeof colours / sizeof colours[0]; j++) {
            make_png(png, 7, 9, depths[i], colours[j]);
            bool legal = colours[j] == 0 || (colours[j] == 3 && depths[i] != 16) ||
                         (colours[j] != 3 && depths[i] >= 8);
            ASSERT_EQ(legal ? TNY_IMAGE_DIM_OK : TNY_IMAGE_DIM_UNVERIFIABLE,
                      tny_image_dimensions(png, sizeof png, &w, &h));
        }
    /* Baseline, extended, progressive and arithmetic frames all carry sizes. */
    const uint8_t frames[] = {0xC0, 0xC1, 0xC2, 0xC9, 0xCA};
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        size_t n = make_jpeg(jpeg, frames[i], 1935, 811);
        ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(jpeg, n, &w, &h));
        ASSERT_EQ(1935u, w);
        ASSERT_EQ(811u, h);
        ASSERT(prefixes_never_lie(jpeg, n, 1935, 811));
    }
    const char *codecs[] = {"VP8 ", "VP8L", "VP8X"};
    for (size_t i = 0; i < sizeof codecs / sizeof codecs[0]; i++) {
        size_t n = make_webp(webp, codecs[i], 2048, 1024);
        ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(webp, n, &w, &h));
        ASSERT_EQ(2048u, w);
        ASSERT_EQ(1024u, h);
        ASSERT(prefixes_never_lie(webp, n, 2048, 1024));
        ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n - 1, &w, &h));
    }
    /* A skipped, odd-length chunk still leaves the walk on a chunk boundary. */
    uint8_t padded[80];
    size_t n = make_webp(padded, "VP8L", 64, 32);
    memmove(padded + 12 + 8 + 4, padded + 12, n - 12);
    memcpy(padded + 12, "ICCP", 4);
    store_le32(padded + 16, 3);
    memset(padded + 20, 0, 4); /* three payload bytes plus one pad byte */
    n += 12;
    store_le32(padded + 4, (uint32_t)n - 8);
    ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(padded, n, &w, &h));
    ASSERT_EQ(64u, w);
    ASSERT_EQ(32u, h);
    PASS();
}

TEST image_dimensions_reject_impossible_headers(void) {
    uint8_t png[PNG_HEADER_LEN], jpeg[64], webp[64];
    uint32_t w = 1, h = 1;
    const uint32_t bad_edges[] = {0, TNY_IMAGE_DIMENSION_MAX + 1, 0x80000000u, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof bad_edges / sizeof bad_edges[0]; i++) {
        make_png(png, bad_edges[i], 8, 8, 6);
        ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
        make_png(png, 8, bad_edges[i], 8, 6);
        ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
        ASSERT_EQ(0u, w);
        ASSERT_EQ(0u, h);
    }
    make_png(png, 8, 8, 8, 6);
    png[11] = 12; /* IHDR length that does not describe an IHDR */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
    make_png(png, 8, 8, 8, 6);
    memcpy(png + 12, "IDAT", 4); /* first chunk must be the header chunk */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
    make_png(png, 8, 8, 8, 6);
    png[28] = 2; /* unknown interlace method */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
    make_png(png, 8, 8, 8, 6);
    png[26] = 1; /* unknown compression method */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(png, sizeof png, &w, &h));
    /* Lossless, differential and hierarchical JPEG frames are not read. */
    const uint8_t unsupported[] = {0xC3, 0xC5, 0xC6, 0xC7, 0xCB, 0xCD, 0xCE, 0xCF};
    for (size_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; i++) {
        size_t n = make_jpeg(jpeg, unsupported[i], 100, 50);
        ASSERT_EQ(TNY_IMAGE_DIM_UNSUPPORTED, tny_image_dimensions(jpeg, n, &w, &h));
        ASSERT_EQ(0u, w);
    }
    size_t n = make_jpeg(jpeg, 0xC0, 0, 50); /* zero edge in the frame header */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    n = make_jpeg(jpeg, 0xC0, 100, 50);
    jpeg[5] = 200; /* APP0 length beyond the data */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    n = make_jpeg(jpeg, 0xC0, 100, 50);
    jpeg[5] = 1; /* segment length below its own two length bytes */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    n = make_jpeg(jpeg, 0xC0, 100, 50);
    jpeg[22] = 18; /* frame length inconsistent with its component count */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    const uint8_t no_frame[] = {0xFF, 0xD8, 0xFF, 0xDA, 0, 4, 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(no_frame, sizeof no_frame, &w, &h));
    n = make_webp(webp, "VP8 ", 200, 100);
    webp[20] = 0x11; /* interframe, not a key frame */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8 ", 200, 100);
    webp[23] = 0x9C; /* corrupt start code */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8L", 200, 100);
    webp[20] = 0x30; /* wrong lossless signature */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8X", 200, 100);
    store_le32(webp + 16, 9); /* extended header of the wrong length */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8L", 200, 100);
    store_le32(webp + 16, 64); /* chunk longer than the RIFF payload */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8L", 200, 100);
    store_le32(webp + 4, 4096); /* RIFF size beyond the actual bytes */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8L", 200, 100);
    memcpy(webp + 12, "XXXX", 4); /* a container with no known canvas chunk */
    ASSERT_EQ(TNY_IMAGE_DIM_UNSUPPORTED, tny_image_dimensions(webp, n, &w, &h));
    /* The CRC bytes here were generated independently with Python zlib.crc32,
     * not with the fixture or production checksum implementation. */
    static const uint8_t known_png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00,
                                        0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
                                        0x00, 0x11, 0x00, 0x00, 0x00, 0x0B, 0x08, 0x06, 0x00,
                                        0x00, 0x00, 0x99, 0x20, 0x66, 0x07};
    uint8_t corrupt[sizeof known_png];
    ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(known_png, sizeof known_png, &w, &h));
    memcpy(corrupt, known_png, sizeof corrupt);
    corrupt[32] ^= 1;
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(corrupt, sizeof corrupt, &w, &h));
    n = make_webp(webp, "VP8L", 17, 11);
    store_le32(webp + 4, (uint32_t)n - 9); /* omit the required odd-payload pad */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n - 1, &w, &h));
    n = make_webp(webp, "VP8L", 17, 11);
    webp[n - 1] = 1; /* RIFF pad must be zero */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8L", 17, 11);
    webp[24] |= 0x20; /* unsupported lossless stream version */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_webp(webp, "VP8X", 17, 11);
    webp[20] = 0xC1;
    webp[21] = webp[22] = webp[23] = 0xFF;
    ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(webp, n, &w, &h));
    ASSERT_EQ(17u, w);
    ASSERT_EQ(11u, h); /* reader MUST ignore the reserved fields */
    n = make_webp(webp, "VP8X", 65536, 65536);
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, n, &w, &h));
    n = make_jpeg(jpeg, 0xC0, 17, 11);
    jpeg[25] = 12; /* baseline precision must be 8 */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    n = make_jpeg(jpeg, 0xC2, 17, 11);
    jpeg[25] = 12;
    ASSERT_EQ(TNY_IMAGE_DIM_OK, tny_image_dimensions(jpeg, n, &w, &h));
    jpeg[25] = 16; /* supported DCT frame precision is 8 or 12 */
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(jpeg, n, &w, &h));
    /* The RIFF end declares two bytes of an incomplete next chunk header. */
    memcpy(webp, "RIFF", 4);
    store_le32(webp + 4, 6);
    memcpy(webp + 8, "WEBPxx", 6);
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(webp, 14, &w, &h));
    const uint8_t gif[] = "GIF89a\1\0\1\0\0\0\0";
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(gif, sizeof gif - 1, &w, &h));
    ASSERT_EQ(TNY_IMAGE_DIM_UNVERIFIABLE, tny_image_dimensions(NULL, 0, &w, &h));
    ASSERT_EQ(0u, w);
    ASSERT_EQ(0u, h);
    PASS();
}

TEST image_size_requests_and_status(void) {
    uint32_t w = 9, h = 9;
    ASSERT(tny_image_size_parse("3440x1440", &w, &h));
    ASSERT_EQ(3440u, w);
    ASSERT_EQ(1440u, h);
    const char *opaque[] = {NULL,        "",           "auto",      "1024",      "1024x",   "x1024",
                            "0x1024",    "1024x0",     " 1024x16",  "1024x16 ",  "1024X16", "+1x1",
                            "01024x768", "1024x768x2", "1048577x8", "12345678x8"};
    for (size_t i = 0; i < sizeof opaque / sizeof opaque[0]; i++) {
        w = h = 9;
        ASSERT(!tny_image_size_parse(opaque[i], &w, &h));
        ASSERT_EQ(0u, w);
        ASSERT_EQ(0u, h);
    }
    /* Omitted or auto requests never become a match, a mismatch or an error. */
    ASSERT_EQ(TNY_IMAGE_SIZE_AUTO, tny_image_size_compare(NULL, TNY_IMAGE_DIM_OK, 1, 1));
    ASSERT_EQ(TNY_IMAGE_SIZE_AUTO, tny_image_size_compare("auto", TNY_IMAGE_DIM_OK, 1, 1));
    ASSERT_EQ(TNY_IMAGE_SIZE_AUTO,
              tny_image_size_compare("auto", TNY_IMAGE_DIM_UNVERIFIABLE, 0, 0));
    ASSERT_EQ(TNY_IMAGE_SIZE_MATCH, tny_image_size_compare("2x3", TNY_IMAGE_DIM_OK, 2, 3));
    ASSERT_EQ(TNY_IMAGE_SIZE_MISMATCH, tny_image_size_compare("2x3", TNY_IMAGE_DIM_OK, 2, 4));
    ASSERT_EQ(TNY_IMAGE_SIZE_MISMATCH, tny_image_size_compare("2x3", TNY_IMAGE_DIM_OK, 3, 3));
    /* Aspect ratio alone is not a match: 2:1 requested, 2:1 returned, smaller. */
    ASSERT_EQ(TNY_IMAGE_SIZE_MISMATCH,
              tny_image_size_compare("3440x1720", TNY_IMAGE_DIM_OK, 1720, 860));
    ASSERT_EQ(TNY_IMAGE_SIZE_UNVERIFIABLE,
              tny_image_size_compare("2x3", TNY_IMAGE_DIM_UNVERIFIABLE, 0, 0));
    /* An opaque provider token stays unverifiable even with readable bytes. */
    ASSERT_EQ(TNY_IMAGE_SIZE_UNVERIFIABLE,
              tny_image_size_compare("portrait", TNY_IMAGE_DIM_OK, 1024, 1536));
    ASSERT_EQ(TNY_IMAGE_SIZE_UNSUPPORTED,
              tny_image_size_compare("2x3", TNY_IMAGE_DIM_UNSUPPORTED, 0, 0));
    ASSERT_STR_EQ("mismatch", tny_image_size_status_name(TNY_IMAGE_SIZE_MISMATCH));
    ASSERT_STR_EQ("unverifiable", tny_image_size_status_name(TNY_IMAGE_SIZE_UNVERIFIABLE));
    ASSERT_STR_EQ("unsupported", tny_image_size_status_name(TNY_IMAGE_SIZE_UNSUPPORTED));
    ASSERT_STR_EQ("auto", tny_image_size_status_name(TNY_IMAGE_SIZE_AUTO));
    ASSERT_STR_EQ("match", tny_image_size_status_name(TNY_IMAGE_SIZE_MATCH));
    char warning[320];
    ASSERT(!tny_image_size_warning("auto", "auto", 0, 0, warning, sizeof warning));
    ASSERT(!tny_image_size_warning("2x3", "match", 2, 3, warning, sizeof warning));
    ASSERT(!tny_image_size_warning("portrait", "unverifiable", 8, 8, warning, sizeof warning));
    ASSERT(tny_image_size_warning("3440x1440", "mismatch", 1935, 811, warning, sizeof warning));
    ASSERT(strstr(warning, "3440x1440"));
    ASSERT(strstr(warning, "1935x811"));
    ASSERT(tny_image_size_warning("3440x1440", "unverifiable", 0, 0, warning, sizeof warning));
    ASSERT(strstr(warning, "could not be read"));
    PASS();
}

TEST image_strict_size_is_settled_before_any_request(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    tny_image_request r = {
        .prompt = "hello", .output_file = "unused", .strict_size = true, .cancelled = cancelled};
    tny_image_result result;
    char err[256];
    /* cancelled() is always true, so reaching any later stage would return 130. */
    const char *invalid[] = {NULL, "auto", "portrait", "1024", "0x0"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        r.size = invalid[i];
        ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
        ASSERT_STR_EQ(TNY_IMAGE_CODE_STRICT_INVALID, result.code);
        ASSERT(strstr(err, TNY_IMAGE_CODE_STRICT_INVALID));
        ASSERT_EQ(0u, result.width);
        ASSERT_EQ(0u, result.height);
        buf_t json;
        buf_init(&json);
        tny_image_error_json(&r, &result, err, &json);
        ASSERT(strstr(json.data, "\"ok\":false"));
        ASSERT(strstr(json.data, "\"code\":\"" TNY_IMAGE_CODE_STRICT_INVALID "\""));
        ASSERT(strstr(json.data, "\"path\":null"));
        ASSERT(strstr(json.data, "\"committed\":false"));
        ASSERT(strstr(json.data, "\"width\":null,\"height\":null"));
        buf_free(&json);
    }
    r.size = "1024x1024"; /* a concrete request reaches the ordinary path */
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    ASSERT(!result.code);
    r.strict_size = false;
    r.size = "auto";
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    buf_t json;
    buf_init(&json);
    result.provider = "codex";
    snprintf(result.model, sizeof result.model, "fixture-image-model");
    result.mime = "image/png";
    result.bytes = 68;
    result.effective_size[0] = 0;
    result.edit = true; /* serializers use the resolved operation, not the request */
    tny_image_result_json(&r, &result, &json);
    ASSERT(strstr(json.data, "\"operation\":\"edit\""));
    ASSERT(strstr(json.data, "\"requested_size\":\"auto\""));
    ASSERT(strstr(json.data, "\"effective_size\":null"));
    ASSERT(strstr(json.data, "\"width\":null,\"height\":null"));
    ASSERT(strstr(json.data, "\"size_status\":\"auto\""));
    ASSERT(strstr(json.data, "\"native\":true,\"transform\":null"));
    buf_free(&json);
    PASS();
}

TEST image_prepared_quality_is_checked_before_provider_availability(void) {
    tny_ctx offline = {.cwd = root};
    tny_image_request r = {.from_manifest = "not-reopened.json", .output_file = "unused"};
    tny_image_plan plan = {.prompt = xstrdup("approved prompt"),
                           .quality = xstrdup("not-a-quality")};
    tny_image_result result;
    char err[256];
    ASSERT_EQ(1, tny_image_run_prepared(&offline, &r, &plan, &result, err, sizeof err));
    ASSERT(strstr(err, "valid options"));
    ASSERT(!result.committed);
    ASSERT(!*result.operation_id);
    ASSERT(!*result.manifest_path);
    tny_image_plan_free(&plan);
    PASS();
}

TEST image_tool_schema_permission_and_interception(void) {
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    const char *args = "{\"prompt\":\"hello\",\"output_file\":\"out.png\",\"images\":[\"first."
                       "png\",\"second.png\"]}";
    char *schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(strstr(schema, "image_generate"));
    ASSERT(strstr(schema, "image_edit"));
    free(schema);
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args, &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    ASSERT(strstr(call.summary, "out.png"));
    ASSERT(strstr(call.summary, "first.png"));
    ASSERT(strstr(call.summary, "second.png"));
    ASSERT(strstr(call.summary, "codex"));
    tools_call_grant(&env, &call);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args, &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0,
              tools_call_prepare(
                  &env, "image_edit",
                  "{\"prompt\":\"hello\",\"output_file\":\"out.png\",\"images\":[\"other.png\"]}",
                  &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);
    const char *bad[] = {
        "{\"prompt\":3,\"output_file\":\"out.png\"}",
        "{\"prompt\":\"x\\u0000y\",\"output_file\":\"out.png\"}",
        "{\"prompt\":\"x\",\"output_file\":\"out.png\",\"images\":[3]}",
        "{\"prompt\":\"x\",\"output_file\":\"out.png\",\"images\":[\"x\\u0000y\"]}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", bad[i], &call));
        tools_call_free(&call);
    }
    ctx.tool_profile = TNY_TOOLS_TERMINAL;
    schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(!strstr(schema, "image_generate"));
    free(schema);
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "terminal",
                                    "{\"command\":\"printf 'hello' | tny image edit --image "
                                    "first.png --image second.png --output-file out.png --json\"}",
                                    &call));
    ASSERT(call.intercept);
    ASSERT_EQ(TNY_INTERCEPT_IMAGE_RENDER, call.intercept->kind);
    ASSERT_STR_EQ("image_edit", call.permission_tool);
    ASSERT_EQ(PERM_ALLOW, call.verdict); /* same grant as the typed call */
    tools_call_free(&call);
    ctx.tool_profile = TNY_TOOLS_ALL;
    ctx.library_mode = true;
    schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(!strstr(schema, "image_edit"));
    free(schema);
    ctx.library_mode = false;
    ctx.ssh_host = "fixture";
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    ctx.ssh_host = NULL;
    ctx.chatgpt_token = NULL;
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    perm_free(env.perm);
    PASS();
}

TEST image_decode_exact_output_limit(void) {
    size_t max = TNY_IMAGE_OUTPUT_MAX;
    unsigned char *data = calloc(1, max + 1);
    ASSERT(data);
    memcpy(data, "\x89PNG\r\n\x1a\n", 8);
    char err[256];
    buf_t encoded, out;
    buf_init(&encoded);
    buf_init(&out);
    b64_encode(data, max, &encoded);
    ASSERT_EQ(0, tny_image_decode(encoded.data, encoded.len, &out, err, sizeof err));
    ASSERT_EQ(max, out.len);
    buf_free(&encoded);
    buf_free(&out);
    buf_init(&encoded);
    buf_init(&out);
    b64_encode(data, max + 1, &encoded);
    ASSERT_EQ(1, tny_image_decode(encoded.data, encoded.len, &out, err, sizeof err));
    ASSERT_EQ(0, out.len);
    buf_free(&encoded);
    buf_free(&out);
    free(data);
    PASS();
}

TEST image_invalid_later_reference_is_an_error(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    char *input = path_join(root, "input.png");
    ASSERT(input);
    const char bytes[] = "\x89PNG\r\n\x1a\nABCD";
    ASSERT_EQ(0, file_write_atomic(input, bytes, sizeof bytes - 1));
    tny_image_request r = {.edit = true,
                           .prompt = "edit",
                           .output_file = "unused",
                           .images = {input, "\xff"},
                           .image_count = 2};
    tny_image_result result;
    char err[256];
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    ASSERT(strstr(err, "reference path"));
    ASSERT_EQ(0, result.bytes);
    unlink(input);
    free(input);
    PASS();
}

/* ---- manifests, guards and destinations (#127) ---- */

static char *under_root(const char *name) {
    char *path = path_join(root, name);
    if (!path) abort();
    return path;
}

/* ---- explicit local exports and contact sheets (#125) ---- */

/* A complete 2x2 PNG, so an export's own bounds and hashes have real bytes to
 * read. No converter runs in these unit tests. */
static const unsigned char TINY_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a,
    0x73, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x08, 0x1d, 0x63, 0x60, 0x60, 0xf8, 0xcf,
    0xc0, 0xc0, 0xf0, 0x9f, 0x01, 0x09, 0x0c, 0x00, 0x29, 0x0d, 0x03, 0xf9, 0x1f, 0x9d, 0x7e, 0xdf,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

static int export_options(const char *line, tny_image_export_request *r, bool *json) {
    static char storage[16][128];
    static char *argv[16];
    int argc = 0;
    const char *p = line;
    while (*p && argc < 16) {
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof storage[0]) abort();
        memcpy(storage[argc], p, n);
        storage[argc][n] = 0;
        argv[argc] = storage[argc];
        argc++;
        p = end ? end + 1 : p + n;
    }
    return tny_image_export_options(argc, argv, r, json);
}

TEST image_export_grammar_and_settings(void) {
    tny_image_export_request r = {0};
    tny_image_export_settings s;
    char err[256];
    bool json = false;
    /* Defaults: fit, center, transparent, png, exact canvas, no grid. */
    ASSERT_EQ(0,
              export_options("export --image a.png --output-file out.png --size 64x48", &r, &json));
    ASSERT(!json);
    ASSERT_EQ(0, tny_image_export_settings_resolve(&r, &s, err, sizeof err));
    ASSERT_EQ(TNY_IMAGE_POLICY_FIT, s.policy);
    ASSERT_EQ(TNY_IMAGE_GRAVITY_CENTER, s.gravity);
    ASSERT_EQ(TNY_IMAGE_FORMAT_PNG, s.format);
    ASSERT_STR_EQ("transparent", s.background);
    ASSERT_EQ(64u, s.width);
    ASSERT_EQ(48u, s.height);
    ASSERT_EQ(0u, s.columns);
    ASSERT_STR_EQ("export", tny_image_export_operation(&r));

    /* JPEG cannot carry alpha, so its documented default is black. */
    r = (tny_image_export_request){0};
    ASSERT_EQ(0, export_options("export --image a.png --output-file o.jpg --size 8x8 --format jpeg",
                                &r, &json));
    ASSERT_EQ(0, tny_image_export_settings_resolve(&r, &s, err, sizeof err));
    ASSERT_STR_EQ("#000000", s.background);
    r = (tny_image_export_request){0};
    ASSERT_EQ(0, export_options("export --image a.png --output-file o.jpg --size 8x8 --format jpeg "
                                "--background #AABBCC",
                                &r, &json));
    ASSERT_EQ(0, tny_image_export_settings_resolve(&r, &s, err, sizeof err));
    ASSERT_STR_EQ("#aabbcc", s.background);

    /* A sheet keeps the order given and derives its grid; the remainder of a
     * canvas that does not divide evenly stays background. */
    r = (tny_image_export_request){0};
    ASSERT_EQ(0, export_options("contact-sheet --image a.png --artifact rec.json --image c.png "
                                "--output-file s.png --size 101x61 --labels numbers",
                                &r, &json));
    ASSERT_EQ(3u, r.source_count);
    ASSERT(!r.source_is_artifact[0] && r.source_is_artifact[1] && !r.source_is_artifact[2]);
    ASSERT_STR_EQ("contact_sheet", tny_image_export_operation(&r));
    ASSERT_EQ(0, tny_image_export_settings_resolve(&r, &s, err, sizeof err));
    ASSERT_EQ(2u, s.columns); /* ceil(sqrt(3)) */
    ASSERT_EQ(2u, s.rows);
    ASSERT_EQ(50u, s.cell_width);
    ASSERT_EQ(30u, s.cell_height);
    ASSERT_EQ(1u, s.label_scale);
    ASSERT_EQ(7u, s.label_width); /* one digit: scale * (6 * digits + 1) */
    ASSERT_EQ(9u, s.label_height);

    /* Cells too small for the fixed label are a validation error, not a
     * silently dropped label. */
    r = (tny_image_export_request){0};
    ASSERT_EQ(0, export_options("contact-sheet --image a.png --image b.png --output-file s.png "
                                "--size 10x8 --labels numbers",
                                &r, &json));
    ASSERT_EQ(1, tny_image_export_settings_resolve(&r, &s, err, sizeof err));
    ASSERT(strstr(err, "too small"));

    const char *invalid[] = {
        "export --image a.png --output-file out.png",                       /* no size */
        "export --image a.png --output-file out.png --size 64",             /* not WxH */
        "export --image a.png --output-file out.png --size 0x10",           /* zero edge */
        "export --image a.png --output-file out.png --size 20000x10",       /* past the edge max */
        "export --image a.png --output-file out.png --size 16384x16384",    /* past 64M pixels */
        "export --image a.png --output-file out.png --size 8x8 --fit fill", /* unknown policy */
        "export --image a.png --output-file out.png --size 8x8 --gravity up",
        "export --image a.png --output-file out.png --size 8x8 --format gif",
        "export --image a.png --output-file out.png --size 8x8 --background red",
        "export --image a.png --output-file out.png --size 8x8 --background #ABC",
        "contact-sheet --image a.png --output-file s.png --size 8x8 --labels loud",
        "contact-sheet --image a.png --image b.png --output-file s.png --size 8x8 --columns 3",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        r = (tny_image_export_request){0};
        int parsed = export_options(invalid[i], &r, &json);
        if (!parsed) parsed = tny_image_export_settings_resolve(&r, &s, err, sizeof err);
        ASSERT_EQ(1, parsed);
    }
    /* Grid options belong to a sheet, and every option needs its value. */
    const char *rejected[] = {
        "export --image a.png --output-file out.png --size 8x8 --columns 2",
        "export --image a.png --output-file out.png --size 8x8 --labels numbers",
        "export --image a.png --image b.png --output-file out.png --size 8x8",
        "export --output-file out.png --size 8x8",
        "export --image a.png --size 8x8",
        "export --image a.png --output-file out.png --size",
        "resize --image a.png --output-file out.png --size 8x8",
    };
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        r = (tny_image_export_request){0};
        ASSERT_EQ(1, export_options(rejected[i], &r, &json));
    }
    r = (tny_image_export_request){0};
    ASSERT_EQ(-1, export_options("export --help", &r, &json));
    r = (tny_image_export_request){0};
    ASSERT_EQ(0, export_options("export --image a.png --output-file out.png --size 8x8 --json", &r,
                                &json));
    ASSERT(json);
    PASS();
}

TEST image_export_tools_carry_the_whole_operation(void) {
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    char *source = under_root("export-src.png");
    ASSERT_EQ(0, file_write_atomic(source, (const char *)TINY_PNG, sizeof TINY_PNG));
    const char *args =
        "{\"sources\":[{\"image\":\"export-src.png\"}],\"output_file\":\"export-out.png\","
        "\"size\":\"32x32\",\"fit\":\"crop\",\"gravity\":\"north\"}";
    char *schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(strstr(schema, "image_export"));
    ASSERT(strstr(schema, "image_contact_sheet"));
    free(schema);
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(&env, "image_export", args, &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    /* The grant names the operation, the exact bytes and every setting. */
    ASSERT(strstr(call.detail, "\"operation\":\"export\""));
    ASSERT(strstr(call.detail, "export-src.png"));
    ASSERT(strstr(call.detail, "export-out.png"));
    ASSERT(strstr(call.detail, "\"policy\":\"crop\""));
    ASSERT(strstr(call.detail, "\"gravity\":\"north\""));
    ASSERT(strstr(call.detail, "\"width\":32,\"height\":32"));
    ASSERT(strstr(call.detail, "\"overwrite\":false"));
    ASSERT(strstr(call.detail, "\"persist_manifest\":true"));
    char *hashed = strstr(call.detail, "\"sha256\":\"");
    ASSERT(hashed);
    tools_call_grant(&env, &call);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "image_export", args, &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    /* A changed source is a different operation: the old grant does not
     * cover the new bytes. */
    unsigned char changed[sizeof TINY_PNG];
    memcpy(changed, TINY_PNG, sizeof TINY_PNG);
    changed[sizeof TINY_PNG - 12] ^= 0x01;
    ASSERT_EQ(0, file_write_atomic(source, (const char *)changed, sizeof changed));
    ASSERT_EQ(0, tools_call_prepare(&env, "image_export", args, &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0, file_write_atomic(source, (const char *)TINY_PNG, sizeof TINY_PNG));
    /* A different setting is a different operation too. */
    ASSERT_EQ(0, tools_call_prepare(
                     &env, "image_export",
                     "{\"sources\":[{\"image\":\"export-src.png\"}],\"output_file\":\"export-out."
                     "png\",\"size\":\"32x32\",\"fit\":\"crop\",\"gravity\":\"south\"}",
                     &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);
    const char *bad[] = {
        "{\"sources\":[],\"output_file\":\"o.png\",\"size\":\"8x8\"}",
        "{\"sources\":[{\"image\":\"export-src.png\",\"artifact\":\"r.json\"}],\"output_file\":\"o."
        "png\",\"size\":\"8x8\"}",
        "{\"sources\":[\"export-src.png\"],\"output_file\":\"o.png\",\"size\":\"8x8\"}",
        "{\"sources\":[{\"image\":\"missing.png\"}],\"output_file\":\"o.png\",\"size\":\"8x8\"}",
        "{\"sources\":[{\"image\":\"export-src.png\"}],\"output_file\":\"o.png\",\"size\":\"8\"}",
        "{\"sources\":[{\"image\":\"export-src.png\"}],\"output_file\":\"o.png\",\"size\":\"8x8\","
        "\"columns\":2}", /* grid option on a single export */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ASSERT_EQ(-1, tools_call_prepare(&env, "image_export", bad[i], &call));
        tools_call_free(&call);
    }
    /* The same command typed into the terminal tool is the same operation,
     * under the same grant. */
    ASSERT_EQ(0, tools_call_prepare(&env, "terminal",
                                    "{\"command\":\"tny image export --image export-src.png "
                                    "--output-file export-out.png --size 32x32 --fit crop "
                                    "--gravity north\"}",
                                    &call));
    ASSERT(call.intercept);
    ASSERT_EQ(TNY_INTERCEPT_IMAGE_EXPORT, call.intercept->kind);
    ASSERT_STR_EQ("image_export", call.permission_tool);
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "terminal",
                                    "{\"command\":\"tny image contact-sheet --image export-src.png "
                                    "--output-file sheet.png --size 32x32 --labels numbers\"}",
                                    &call));
    ASSERT(call.intercept);
    ASSERT_EQ(TNY_INTERCEPT_IMAGE_EXPORT, call.intercept->kind);
    ASSERT_STR_EQ("image_contact_sheet", call.permission_tool);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);
    /* Runtimes without a local process seam hide and refuse both tools. */
    ctx.library_mode = true;
    schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(!strstr(schema, "image_export"));
    ASSERT(!strstr(schema, "image_contact_sheet"));
    free(schema);
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_export", args, &call));
    tools_call_free(&call);
    ctx.library_mode = false;
    ctx.ssh_host = "fixture";
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_export", args, &call));
    tools_call_free(&call);
    ctx.ssh_host = NULL;
    unlink(source);
    free(source);
    perm_free(env.perm);
    PASS();
}

TEST image_export_records_are_derived_and_never_replayed(void) {
    char *output = under_root("derived.png");
    char *artifact_source = under_root("sheet-src.png");
    tny_image_reference sources[2] = {{.path = artifact_source}, {.path = artifact_source}};
    snprintf(sources[0].sha256, sizeof sources[0].sha256, "%s",
             "aa11bb22cc33dd44ee55ff6677889900aabbccddeeff00112233445566778899");
    snprintf(sources[1].sha256, sizeof sources[1].sha256, "%s",
             "0011223344556677889900aabbccddeeff00112233445566778899aabbccddee");
    sources[0].job = (tny_image_job){.id = "0123456789abcdef0123456789abcdef",
                                     .item_index = 2,
                                     .projection_attempt = 3,
                                     .item_attempt = 1,
                                     .carried_from_attempt = 1,
                                     .bytes = 42};
    tny_image_source_dimensions dimensions[2] = {{19, 7}, {3, 25}};
    tny_image_transform transform = {.operation = "contact_sheet",
                                     .source_dimensions = dimensions,
                                     .policy = "crop",
                                     .gravity = "northwest",
                                     .background = "#112233",
                                     .format = "webp",
                                     .labels = "numbers",
                                     .width = 64,
                                     .height = 32,
                                     .columns = 2,
                                     .rows = 1,
                                     .cell_width = 32,
                                     .cell_height = 32,
                                     .tool = "imagemagick",
                                     .tool_version = "7.1.2-31",
                                     .sources = sources,
                                     .source_count = 2};
    tny_image_record record = {.operation_id = "00112233445566aa",
                               .status = "succeeded",
                               .workspace = root,
                               .started = "2026-09-12T00:00:00Z",
                               .finished = "2026-09-12T00:00:01Z",
                               .output = output,
                               .committed = true,
                               .requested_provider = "local",
                               .requested_size = "64x32",
                               .effective_provider = "local",
                               .effective_size = "64x32",
                               .output_sha256 =
                                   "ffeeddccbbaa99887766554433221100ffeeddccbbaa998877665544332211",
                               .mime = "image/webp",
                               .width = 64,
                               .height = 32,
                               .bytes = 512,
                               .size_status = "match",
                               .transform = &transform};
    /* A real 64-hex digest for the artifact, so the record is loadable. */
    record.output_sha256 = "ffeeddccbbaa99887766554433221100ffeeddccbbaa9988776655443322110f";
    buf_t out;
    buf_init(&out);
    tny_image_manifest_serialize(&record, &out);
    ASSERT(!out.oom);
    ASSERT(strstr(out.data, "\"operation\":\"contact_sheet\""));
    ASSERT(strstr(out.data, "\"prompt\":null"));
    ASSERT(strstr(out.data, "\"provider\":\"local\""));
    ASSERT(strstr(out.data, "\"role\":\"derived\",\"native\":false"));
    ASSERT(strstr(out.data, "\"references\":[]"));
    ASSERT(strstr(out.data, "\"cell_width\":32"));
    ASSERT(strstr(out.data, "\"tool_version\":\"7.1.2-31\""));
    char *path = tny_image_manifest_path(output, record.operation_id);
    ASSERT(path);
    ASSERT_EQ(0, file_write_atomic(path, out.data, out.len));
    buf_free(&out);
    char err[256];
    tny_image_manifest *loaded = tny_image_manifest_load(path, err, sizeof err);
    ASSERT(loaded);
    ASSERT(tny_image_manifest_derived(loaded));
    ASSERT_STR_EQ("contact_sheet", loaded->operation);
    ASSERT_STR_EQ("crop", loaded->transform_policy);
    ASSERT_STR_EQ("numbers", loaded->transform_labels);
    ASSERT_EQ(2u, loaded->source_count);
    ASSERT_STR_EQ(sources[0].job.id, loaded->sources[0].job.id);
    ASSERT_EQ(3, loaded->sources[0].job.projection_attempt);
    ASSERT_EQ(1, loaded->sources[0].job.item_attempt);
    ASSERT_EQ(1, loaded->sources[0].job.carried_from_attempt);
    ASSERT_EQ(42u, loaded->sources[0].job.bytes);
    ASSERT(!*loaded->sources[1].job.id);
    ASSERT_EQ(19u, loaded->source_dimensions[0].width);
    ASSERT_EQ(7u, loaded->source_dimensions[0].height);
    ASSERT_EQ(3u, loaded->source_dimensions[1].width);
    ASSERT_EQ(25u, loaded->source_dimensions[1].height);
    /* Reader-to-writer roundtrip retains input dimensions independently of
     * the target and artifact dimensions. */
    transform.sources = loaded->sources;
    transform.source_dimensions = loaded->source_dimensions;
    buf_init(&out);
    tny_image_manifest_serialize(&record, &out);
    ASSERT(strstr(out.data, "\"width\":19,\"height\":7"));
    ASSERT(strstr(out.data, "\"width\":3,\"height\":25"));
    buf_free(&out);
    ASSERT_EQ(2u, loaded->grid_columns);
    ASSERT_EQ(32u, loaded->cell_height);
    ASSERT_EQ(0u, loaded->reference_count);
    /* The derived artifact is real and may be edited from; it is simply not a
     * provider operation that can be rerun. */
    ASSERT_STR_EQ("derived", loaded->artifact_role);
    ASSERT_STR_EQ(output, loaded->artifact_path);
    tny_image_manifest_free(loaded);

    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    buf_t args;
    buf_init(&args);
    buf_appendf(&args, "{\"prompt\":\"again\",\"output_file\":\"copy.png\",\"from_manifest\":");
    jescape(&args, path);
    buf_appends(&args, "}");
    tools_call call;
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_export", args.data, &call));
    tools_call_free(&call);
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_generate", args.data, &call));
    ASSERT(call.error);
    ASSERT(strstr(call.error, "local image export"));
    tools_call_free(&call);
    buf_free(&args);
    /* An edit reference, by contrast, resolves to the derived artifact. */
    tny_image_request r = {.edit = true, .prompt = "edit", .output_file = "copy.png"};
    r.images[0] = path;
    r.image_is_artifact[0] = true;
    r.image_count = 1;
    tny_image_plan plan = {0};
    ASSERT_EQ(0, tny_image_plan_resolve(&ctx, &r, &plan, err, sizeof err));
    ASSERT_EQ(1u, plan.reference_count);
    ASSERT_STR_EQ(output, plan.references[0].path);
    ASSERT_STR_EQ(record.output_sha256, plan.references[0].expected);
    tny_image_plan_free(&plan);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    perm_free(env.perm);
    unlink(path);
    free(path);
    free(output);
    free(artifact_source);
    PASS();
}

typedef struct {
    int calls;
    const char *change_source;
} export_approval_fixture;

static tny_perm_decision export_allow_once(const char *tool, const char *summary, void *ud) {
    (void)tool;
    (void)summary;
    export_approval_fixture *f = ud;
    f->calls++;
    if (f->change_source) {
        unsigned char changed[sizeof TINY_PNG];
        memcpy(changed, TINY_PNG, sizeof changed);
        changed[sizeof changed - 1] ^= 1;
        if (file_write_atomic(f->change_source, (const char *)changed, sizeof changed)) abort();
    }
    return TNY_PERM_DECISION_ALLOW;
}

TEST image_export_allow_once_is_exact_and_scoped(void) {
    char *source = under_root("once.png");
    char *tool = under_root("magick");
    char *marker = under_root("once-probed");
    char *saved_path = xstrdup(getenv("PATH"));
    buf_t script;
    buf_init(&script);
    buf_appendf(&script,
                "#!/bin/sh\nif [ \"$1\" = -version ]; then\n"
                "echo 'Version: ImageMagick 7.1.2-31 Q16'\n: > '%s'\nexit 0\nfi\nexit 1\n",
                marker);
    ASSERT_EQ(0, file_write_atomic(tool, script.data, script.len));
    buf_free(&script);
    ASSERT_EQ(0, chmod(tool, 0700));
    ASSERT_EQ(0, setenv("PATH", root, 1));
    export_approval_fixture fixture = {0};
    tools_env env = {.ctx = &ctx, .prompt = export_allow_once, .prompt_ud = &fixture};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    const char *args[] = {
        "{\"sources\":[{\"image\":\"once.png\"}],\"output_file\":\"once-out.png\","
        "\"size\":\"2x2\",\"persist_manifest\":false}",
        "{\"command\":\"tny image export --image once.png --output-file once-out.png "
        "--size 2x2 --no-manifest --json\"}",
        "{\"sources\":[{\"image\":\"once.png\"}],\"output_file\":\"once-out.png\","
        "\"size\":\"2x2\",\"persist_manifest\":false,\"preview\":true}",
        "{\"command\":\"tny image export --image once.png --output-file once-out.png "
        "--size 2x2 --no-manifest --preview --json\"}"};
    for (size_t i = 0; i < 4; i++) {
        for (int changed = 0; changed < 2; changed++) {
            ASSERT_EQ(0, file_write_atomic(source, (const char *)TINY_PNG, sizeof TINY_PNG));
            fixture.change_source = changed ? source : NULL;
            char *out = tools_execute(&env, i % 2 ? "terminal" : "image_export", args[i]);
            ASSERT(out);
            /* The controlled converter fails AFTER the permission boundary.
             * Exact ALLOW_ONCE must reach it; a changed snapshot must not. */
            if (!changed) {
                ASSERT_EQ(0, access(marker, F_OK));
                ASSERT_EQ(0, unlink(marker));
            } else ASSERT_EQ(-1, access(marker, F_OK));
            ASSERT_EQ(0, perm_grant_count(env.perm));
            free(out);
        }
    }
    ASSERT_EQ(8, fixture.calls);
    perm_free(env.perm);
    if (saved_path) ASSERT_EQ(0, setenv("PATH", saved_path, 1));
    else ASSERT_EQ(0, unsetenv("PATH"));
    free(saved_path);
    unlink(source);
    unlink(tool);
    free(source);
    free(tool);
    free(marker);
    PASS();
}

TEST image_export_snapshot_and_retained_result(void) {
    char *source = under_root("snapshot.png");
    char *output = under_root("snapshot-out.png");
    ASSERT_EQ(0, file_write_atomic(source, (const char *)TINY_PNG, sizeof TINY_PNG));
    tny_image_export_request r = {
        .sources = {source}, .source_count = 1, .output_file = output, .size = "1x1"};
    char err[256];
    tny_image_export_plan *plan = tny_image_export_plan_new(&r, err, sizeof err);
    ASSERT(plan);
    ASSERT_EQ(0, tny_image_export_plan_capture(plan, err, sizeof err));
    buf_t before, after;
    buf_init(&before);
    buf_init(&after);
    ASSERT_EQ(0, tny_image_export_plan_detail(plan, &before));
    /* No path read or borrowed request string is needed after capture. */
    ASSERT_EQ(0, unlink(source));
    free(source);
    free(output);
    ASSERT_EQ(0, tny_image_export_plan_capture(plan, err, sizeof err));
    ASSERT_EQ(0, tny_image_export_plan_detail(plan, &after));
    ASSERT_STR_EQ(before.data, after.data);
    tny_image_export_plan_free(plan);
    buf_free(&before);
    buf_free(&after);

    tny_image_export_result result = {.committed = true, .code = TNY_IMAGE_CODE_EXPORT_FAILED};
    snprintf(result.output, sizeof result.output, "%s", "/retained.png");
    ASSERT(tny_image_export_retained(&result));
    r = (tny_image_export_request){0};
    buf_init(&after);
    tny_image_export_error_json(&r, &result, "postcommit failure", &after);
    ASSERT(strstr(after.data, "\"committed\":true"));
    ASSERT(strstr(after.data, "\"path\":\"/retained.png\""));
    ASSERT(strstr(after.data, TNY_IMAGE_CODE_EXPORT_FAILED));
    buf_free(&after);
    result.committed = false;
    ASSERT(!tny_image_export_retained(&result));
    PASS();
}

TEST image_export_commit_never_follows_its_target(void) {
    char err[256];
    char *target = under_root("commit.png");
    char *other = under_root("commit-other.png");
    ASSERT_EQ(0, file_write_atomic(other, "original", 8));
    /* A fresh destination: the stage is created under the parent fd and
     * installed by link, so a competing creator loses atomically. */
    tny_image_commit *commit = tny_image_io_commit_open(target, false, err, sizeof err);
    ASSERT(commit);
    ASSERT(!tny_image_io_commit_target(commit).present);
    ASSERT_EQ(0, tny_image_io_commit_stage(commit, "exported", 8, err, sizeof err));
    ASSERT_EQ(0, file_write_atomic(target, "raced", 5));
    ASSERT_EQ(-1, tny_image_io_commit_finish(commit, err, sizeof err));
    ASSERT(strstr(err, "changed"));
    tny_image_io_commit_close(commit);
    buf_t contents;
    buf_init(&contents);
    ASSERT_EQ(0, tny_image_io_read_bounded(target, 64, &contents));
    ASSERT_EQ(5u, contents.len); /* the racing creator's file is untouched */
    buf_free(&contents);
    /* An existing destination needs the explicit flag. */
    ASSERT(!tny_image_io_commit_open(target, false, err, sizeof err));
    ASSERT(strstr(err, "already exists"));
    commit = tny_image_io_commit_open(target, true, err, sizeof err);
    ASSERT(commit);
    ASSERT(tny_image_io_commit_target(commit).present);
    ASSERT_EQ(0, tny_image_io_commit_stage(commit, "replaced", 8, err, sizeof err));
    ASSERT_EQ(0, tny_image_io_commit_finish(commit, err, sizeof err));
    tny_image_io_commit_close(commit);
    buf_init(&contents);
    ASSERT_EQ(0, tny_image_io_read_bounded(target, 64, &contents));
    ASSERT_EQ(8u, contents.len);
    ASSERT_EQ(0, memcmp(contents.data, "replaced", 8));
    buf_free(&contents);
    /* A symlink target is refused rather than written through, so the file it
     * points at keeps its bytes. */
    char *link = under_root("commit-link.png");
    unlink(link);
    ASSERT_EQ(0, symlink(other, link));
    ASSERT(!tny_image_io_commit_open(link, true, err, sizeof err));
    ASSERT(strstr(err, "symlink"));
    buf_init(&contents);
    ASSERT_EQ(0, tny_image_io_read_bounded(other, 64, &contents));
    ASSERT_EQ(8u, contents.len);
    ASSERT_EQ(0, memcmp(contents.data, "original", 8));
    buf_free(&contents);
    /* An abandoned stage leaves no debris and no partial artifact. */
    unlink(target);
    commit = tny_image_io_commit_open(target, false, err, sizeof err);
    ASSERT(commit);
    ASSERT_EQ(0, tny_image_io_commit_stage(commit, "abandoned", 9, err, sizeof err));
    tny_image_io_commit_close(commit);
    buf_init(&contents);
    ASSERT_EQ(-1, tny_image_io_read_bounded(target, 64, &contents));
    buf_free(&contents);
    /* Inputs are read once, with the identity of the descriptor they came
     * from, so an alias check cannot be defeated by a later rename. */
    tny_image_io_id id = {0};
    buf_init(&contents);
    ASSERT_EQ(0, tny_image_io_read_input(other, 64, &contents, &id, err, sizeof err));
    ASSERT(id.present);
    ASSERT_EQ(8u, contents.len);
    buf_free(&contents);
    buf_init(&contents);
    ASSERT_EQ(-1, tny_image_io_read_input(other, 4, &contents, &id, err, sizeof err));
    ASSERT(!id.present);
    buf_free(&contents);
    unlink(link);
    unlink(other);
    free(link);
    free(other);
    free(target);
    PASS();
}

static tny_image_reference fixture_reference(const char *path, const char *hash) {
    tny_image_reference ref = {.path = (char *)path};
    snprintf(ref.sha256, sizeof ref.sha256, "%s", hash);
    return ref;
}

TEST image_manifest_round_trip_and_strict_parsing(void) {
    const char sha_a[] = "aa11bb22cc33dd44ee55ff6677889900aabbccddeeff00112233445566778899";
    const char sha_b[] = "0011223344556677889900aabbccddeeff00112233445566778899aabbccddee";
    char *output = under_root("round.png");
    char *reference = under_root("ref.png");
    char *path = tny_image_manifest_path(output, "0123456789abcdef");
    ASSERT(path);
    ASSERT(strstr(path, ".tny-image-0123456789abcdef.json"));
    ASSERT(tny_image_manifest_reserved_name(path));
    ASSERT(!tny_image_manifest_reserved_name(output));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-.json"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-zz.json"));
    tny_image_reference refs[1] = {fixture_reference(reference, sha_a)};
    tny_image_record record = {.operation_id = "0123456789abcdef",
                               .edit = true,
                               .status = "succeeded",
                               .workspace = root,
                               .started = "2026-09-12T00:00:00Z",
                               .finished = "2026-09-12T00:00:05Z",
                               .prompt = "a recorded prompt",
                               .output = output,
                               .committed = true,
                               .references = refs,
                               .reference_count = 1,
                               .requested_provider = "codex",
                               .requested_size = "64x48",
                               .effective_provider = "codex",
                               .effective_model = "fixture-model",
                               .effective_size = "64x48",
                               .output_sha256 = sha_b,
                               .mime = "image/png",
                               .width = 64,
                               .height = 48,
                               .bytes = 4096,
                               .size_status = "match"};
    buf_t out;
    buf_init(&out);
    tny_image_manifest_serialize(&record, &out);
    ASSERT(!out.oom);
    ASSERT_EQ(0, tny_image_io_write_new(path, out.data, out.len));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0600, (int)(st.st_mode & 0777));
    /* Writing the same record twice can never overwrite the first. */
    ASSERT_EQ(-1, tny_image_io_write_new(path, out.data, out.len));
    char err[256];
    tny_image_manifest *parsed = tny_image_manifest_load(path, err, sizeof err);
    ASSERT(parsed);
    ASSERT_STR_EQ("0123456789abcdef", parsed->operation_id);
    ASSERT_STR_EQ("edit", parsed->operation);
    ASSERT_STR_EQ("succeeded", tny_image_manifest_observed_status(parsed));
    ASSERT_STR_EQ("a recorded prompt", parsed->prompt);
    ASSERT_EQ(1u, parsed->reference_count);
    ASSERT_STR_EQ(reference, parsed->references[0].path);
    ASSERT_STR_EQ(sha_a, parsed->references[0].sha256);
    ASSERT_STR_EQ(sha_a, parsed->references[0].expected);
    ASSERT(!parsed->references[0].source_manifest);
    ASSERT_STR_EQ(output, parsed->artifact_path);
    ASSERT_STR_EQ(sha_b, parsed->artifact_sha256);
    ASSERT_STR_EQ("native", parsed->artifact_role);
    ASSERT_EQ(64u, parsed->width);
    ASSERT_EQ(48u, parsed->height);
    ASSERT_EQ(4096u, (unsigned)parsed->bytes);
    ASSERT_STR_EQ("64x48", parsed->requested_size);
    ASSERT_STR_EQ("fixture-model", parsed->effective_model);
    /* Relative recorded paths resolve against the record, not this process. */
    char *resolved = tny_image_manifest_resolve(parsed, "nested/ref.png");
    ASSERT(resolved && strstr(resolved, root) == resolved);
    free(resolved);
    tny_image_manifest_free(parsed);
    unlink(path);
    free(path);
    free(output);
    free(reference);
    buf_free(&out);
    PASS();
}

/* The name tny appends is what must stay reserved, even when the caller's own
 * output already contains the marker (ADR 0095). */
TEST image_reserved_names_anchor_on_the_final_suffix(void) {
    char *nested = under_root("shot.tny-image-1.png");
    char *record = tny_image_manifest_path(nested, "0123456789abcdef");
    ASSERT(record);
    /* The real record written beside a marker-containing output. */
    ASSERT(tny_image_manifest_reserved_name(record));
    ASSERT(!tny_image_manifest_reserved_name(nested));
    ASSERT(tny_image_manifest_reserved_name("a.tny-image-1.png.tny-image-abcdef0123456789.json"));
    /* Only the exact final suffix: trailing text, a non-hex id and an empty
     * id are ordinary names, and so is a marker in a directory component. */
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-abcdef.json.bak"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-abcdef.jsonx"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-abcdef"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-.json"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-zz.json"));
    ASSERT(!tny_image_manifest_reserved_name("x.tny-image-ab cd.json"));
    ASSERT(!tny_image_manifest_reserved_name("/tmp/.tny-image-abcdef.json/out.png"));
    ASSERT(!tny_image_manifest_reserved_name("tny-image-abcdef.json"));
    ASSERT(!tny_image_manifest_reserved_name("robot.png"));
    ASSERT(!tny_image_manifest_reserved_name("notes.json"));
    ASSERT(!tny_image_manifest_reserved_name(NULL));
    free(record);
    free(nested);
    PASS();
}

TEST image_manifest_rejects_unusable_records(void) {
    char err[256];
    char *path = under_root("broken.json");
    static const char *const bodies[] = {
        "",
        "{",
        "[]",
        "{\"version\":1}",
        "{\"version\":2,\"kind\":\"image_manifest\"}",
        "{\"version\":\"1\",\"kind\":\"image_manifest\"}",
        "{\"version\":1,\"kind\":\"other\"}",
        /* every required field present but one type wrong or value invalid */
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"zz\",\"operation\":"
        "\"generate\",\"status\":\"succeeded\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "\"p\",\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},\"effective\":{}}",
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"render\",\"status\":\"succeeded\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "\"p\",\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},\"effective\":{}}",
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"generate\",\"status\":\"weird\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "\"p\",\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},\"effective\":{}}",
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"generate\",\"status\":\"succeeded\",\"workspace\":\"relative\",\"started\":\"t\","
        "\"prompt\":\"p\",\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},"
        "\"effective\":{}}",
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"generate\",\"status\":\"succeeded\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "17,\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},\"effective\":{}}",
        /* a committed record that names no hashed artifact */
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"generate\",\"status\":\"succeeded\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "\"p\",\"output\":\"/o\",\"committed\":true,\"requested\":{\"provider\":\"codex\"},"
        "\"effective\":{}}",
        /* six references exceeds the declared maximum */
        "{\"version\":1,\"kind\":\"image_manifest\",\"operation_id\":\"ab\",\"operation\":"
        "\"edit\",\"status\":\"succeeded\",\"workspace\":\"/w\",\"started\":\"t\",\"prompt\":"
        "\"p\",\"output\":\"/o\",\"requested\":{\"provider\":\"codex\"},\"effective\":{},"
        "\"references\":[{},{},{},{},{},{}]}",
    };
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        unlink(path);
        ASSERT_EQ(0, tny_image_io_write_new(path, bodies[i], strlen(bodies[i])));
        err[0] = 0;
        tny_image_manifest *parsed = tny_image_manifest_load(path, err, sizeof err);
        if (parsed) {
            tny_image_manifest_free(parsed);
            FAILm("an invalid manifest was accepted");
        }
        ASSERT(*err);
    }
    /* A future version says so, rather than failing as generic corruption. */
    unlink(path);
    const char future[] = "{\"version\":2,\"kind\":\"image_manifest\"}";
    ASSERT_EQ(0, tny_image_io_write_new(path, future, sizeof future - 1));
    ASSERT(!tny_image_manifest_load(path, err, sizeof err));
    ASSERT(strstr(err, "version"));
    unlink(path);
    ASSERT(!tny_image_manifest_load(root, err, sizeof err)); /* a directory */
    ASSERT(!tny_image_manifest_load(NULL, err, sizeof err));
    free(path);
    PASS();
}

TEST image_destinations_reject_aliases(void) {
    char err[256];
    char *fresh = under_root("fresh.png");
    char *canonical = tny_image_io_canonical(fresh, err, sizeof err);
    ASSERT(canonical);
    ASSERT(strstr(canonical, "fresh.png"));
    free(canonical);
    const char bytes[] = "\x89PNG\r\n\x1a\nABCD";
    char *real = under_root("real.png");
    char *soft = under_root("link.png");
    char *hard = under_root("hard.png");
    char *missing = under_root("absent-dir/x.png");
    ASSERT_EQ(0, file_write_atomic(real, bytes, sizeof bytes - 1));
    ASSERT_EQ(0, symlink(real, soft));
    ASSERT_EQ(0, link(real, hard));
    /* A symlink, a multiply linked name, a directory and a missing parent are
     * all refused before anything can be written through them. */
    const char *refused[] = {soft, hard, root, missing, ".", ""};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        err[0] = 0;
        char *resolved = tny_image_io_canonical(refused[i], err, sizeof err);
        if (resolved) {
            free(resolved);
            FAILm("an unsafe image destination was accepted");
        }
        ASSERT(*err);
    }
    ASSERT(tny_image_io_same_file(real, hard));
    ASSERT(tny_image_io_same_file(real, soft));
    ASSERT(!tny_image_io_same_file(real, fresh));
    unlink(hard);
    unlink(soft);
    unlink(real);
    free(fresh);
    free(real);
    free(soft);
    free(hard);
    free(missing);
    PASS();
}

TEST image_writer_guard_is_exclusive_and_identified(void) {
    char err[256];
    char *first = under_root("guarded.png");
    char *second = under_root("other.png");
    char owner[TNY_IMAGE_IO_ID_MAX];
    ASSERT(!tny_image_io_guard_owner(first, owner));
    ASSERT_STR_EQ("", owner);
    tny_image_guard *held = tny_image_io_guard_acquire(first, "1111222233334444", err, sizeof err);
    ASSERT(held);
    /* The same destination is busy, and the holder names itself. */
    err[0] = 0;
    ASSERT(!tny_image_io_guard_acquire(first, "5555666677778888", err, sizeof err));
    ASSERT(strstr(err, "already writing"));
    ASSERT(tny_image_io_guard_owner(first, owner));
    ASSERT_STR_EQ("1111222233334444", owner);
    /* An independent destination is unaffected. */
    tny_image_guard *other =
        tny_image_io_guard_acquire(second, "9999aaaabbbbcccc", err, sizeof err);
    ASSERT(other);
    ASSERT(tny_image_io_guard_owner(second, owner));
    ASSERT_STR_EQ("9999aaaabbbbcccc", owner);
    tny_image_io_guard_release(other);
    ASSERT(!tny_image_io_guard_owner(second, owner));
    tny_image_io_guard_release(held);
    ASSERT(!tny_image_io_guard_owner(first, owner));
    /* Releasing frees the name for the next writer. */
    held = tny_image_io_guard_acquire(first, "5555666677778888", err, sizeof err);
    ASSERT(held);
    tny_image_io_guard_release(held);
    tny_image_io_guard_release(NULL);
    ASSERT(!tny_image_io_guard_acquire(first, "", err, sizeof err));
    free(first);
    free(second);
    PASS();
}

TEST image_running_record_without_a_live_owner_is_interrupted(void) {
    char err[256];
    char *output = under_root("intent.png");
    char *path = tny_image_manifest_path(output, "abcdef0123456789");
    tny_image_record record = {.operation_id = "abcdef0123456789",
                               .status = "running",
                               .workspace = root,
                               .started = "2026-09-12T00:00:00Z",
                               .prompt = "an interrupted prompt",
                               .output = output,
                               .requested_provider = "codex",
                               .requested_size = "auto"};
    buf_t out;
    buf_init(&out);
    tny_image_manifest_serialize(&record, &out);
    ASSERT_EQ(0, tny_image_io_write_new(path, out.data, out.len));
    tny_image_manifest *parsed = tny_image_manifest_load(path, err, sizeof err);
    ASSERT(parsed);
    /* Nobody holds the destination, so the intent is interrupted, never a
     * success and never "still running". */
    ASSERT_STR_EQ("running", parsed->status);
    ASSERT_STR_EQ("interrupted", tny_image_manifest_observed_status(parsed));
    tny_image_guard *mine = tny_image_io_guard_acquire(output, "abcdef0123456789", err, sizeof err);
    ASSERT(mine);
    ASSERT_STR_EQ("running", tny_image_manifest_observed_status(parsed));
    tny_image_io_guard_release(mine);
    /* A later operation owning the same destination does not revive it. */
    tny_image_guard *later =
        tny_image_io_guard_acquire(output, "0f0f0f0f0f0f0f0f", err, sizeof err);
    ASSERT(later);
    ASSERT_STR_EQ("interrupted", tny_image_manifest_observed_status(parsed));
    tny_image_io_guard_release(later);
    tny_image_manifest_free(parsed);
    unlink(path);
    free(path);
    free(output);
    buf_free(&out);
    PASS();
}

/* ---- prepared plans (#127, ADR 0095) ---- */

static const char png_bytes[] = "\x89PNG\r\n\x1a\nABCD";

static char *write_png(const char *name, const char *tail) {
    char *path = under_root(name);
    buf_t bytes;
    buf_init(&bytes);
    buf_append(&bytes, png_bytes, sizeof png_bytes - 1);
    buf_appends(&bytes, tail);
    if (file_write_atomic(path, bytes.data, bytes.len) != 0) abort();
    buf_free(&bytes);
    return path;
}

static void hash_file(const char *path, char *hex) {
    buf_t bytes;
    buf_init(&bytes);
    if (tny_image_io_read_bounded(path, 4096, &bytes) != 0) abort();
    if (!tny_image_io_sha256_hex(bytes.data, bytes.len, hex)) abort();
    buf_free(&bytes);
}

/* A complete succeeded record of an earlier edit, exactly as a real operation
 * would have left it: one hashed reference and one committed artifact. */
static char *write_record(const char *name, const char *id, const char *provider,
                          const char *prompt, const char *output, const char *output_hash,
                          tny_image_reference *refs, size_t count) {
    tny_image_record record = {.operation_id = id,
                               .edit = count > 0,
                               .status = "succeeded",
                               .workspace = root,
                               .started = "2026-09-12T00:00:00Z",
                               .finished = "2026-09-12T00:00:05Z",
                               .prompt = prompt,
                               .output = output,
                               .committed = true,
                               .references = refs,
                               .reference_count = count,
                               .requested_provider = provider,
                               .requested_size = "auto",
                               .effective_provider = provider,
                               .effective_model = "fixture-model",
                               .output_sha256 = output_hash,
                               .mime = "image/png",
                               .width = 8,
                               .height = 8,
                               .bytes = 12,
                               .size_status = "auto"};
    buf_t out;
    buf_init(&out);
    tny_image_manifest_serialize(&record, &out);
    if (out.oom) abort();
    char *path = under_root(name);
    unlink(path);
    if (tny_image_io_write_new(path, out.data, out.len) != 0) abort();
    buf_free(&out);
    return path;
}

static int prompts;
static tny_perm_decision allow_once(const char *tool, const char *summary, void *ud) {
    (void)tool;
    (void)summary;
    (void)ud;
    prompts++;
    return TNY_PERM_DECISION_ALLOW; /* this call only: no remembered grant */
}

/* An approved call runs the plan it was approved for: no second permission
 * question (which would break a one-time approval) and no second record read. */
TEST image_preview_permission_detail_is_explicit_and_false_is_unchanged(void) {
    tools_env env = {.ctx = &ctx};
    const char *inputs[] = {
        "{\"prompt\":\"blue\",\"output_file\":\"preview-detail.png\"}",
        "{\"prompt\":\"blue\",\"output_file\":\"preview-detail.png\",\"preview\":false}",
        "{\"prompt\":\"blue\",\"output_file\":\"preview-detail.png\",\"preview\":true}",
        "{\"prompt\":\"blue\",\"output_file\":\"preview-detail.png\",\"preview\":\"true\"}"};
    char *details[4] = {0};
    for (int i = 0; i < 4; i++) {
        yyjson_doc *doc = jparse(inputs[i], strlen(inputs[i]));
        tny_image_plan *plan = NULL;
        char *error = NULL;
        details[i] = tool_image_detail(&env, yyjson_doc_get_root(doc), false, &plan, &error);
        if (i < 3) {
            ASSERT(plan);
            ASSERT_FALSE(error);
        } else {
            ASSERT(error);
            ASSERT_FALSE(plan);
        }
        free(error);
        tool_image_plan_free(plan);
        yyjson_doc_free(doc);
    }
    ASSERT_STR_EQ(details[0], details[1]);
    ASSERT(strstr(details[2], details[0]) == details[2]);
    ASSERT(strstr(details[2], "conversation_preview:"));
    for (int i = 0; i < 4; i++) free(details[i]);
    PASS();
}

TEST image_prepared_plan_runs_under_a_one_time_approval(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    /* A closed loopback port: nothing here should reach a provider, and a
     * regression that tries must fail at once instead of leaving the host. */
    ctx.codex_base_url = "http://127.0.0.1:9/backend-api/codex";
    char *reference = write_png("once-ref.png", "one");
    char *artifact = write_png("once-art.png", "two");
    char hash[TNY_IMAGE_SHA256_HEX], artifact_hash[TNY_IMAGE_SHA256_HEX];
    hash_file(reference, hash);
    hash_file(artifact, artifact_hash);
    tny_image_reference refs[1] = {fixture_reference(reference, hash)};
    char *record = write_record("once.json", "1234abcd1234abcd", "codex", "a recorded prompt",
                                artifact, artifact_hash, refs, 1);
    /* A directory destination fails inside the service, after permission and
     * after the plan is honoured, so this stays offline. */
    char *destination = under_root("once-dir");
    if (mkdir(destination, 0700) != 0) abort();
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"output_file\":");
    jescape(&args, destination);
    buf_appends(&args, ",\"from_manifest\":");
    jescape(&args, record);
    buf_appends(&args, "}");

    tools_env env = {.ctx = &ctx, .prompt = allow_once};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    prompts = 0;
    char *result = tools_execute(&env, "image_edit", args.data);
    ASSERT(result);
    ASSERT_EQ(1, prompts);
    ASSERT(str_starts(result, "error: "));
    /* Not a permission refusal, and not the old post-grant re-resolution. */
    ASSERT(!strstr(result, "permission"));
    ASSERT(!strstr(result, "changed after this call was approved"));
    ASSERT(strstr(result, "image output"));
    /* One-time approval remembers nothing: the next call asks again. */
    ASSERT_EQ(0, perm_grant_count(env.perm));
    free(result);
    result = tools_execute(&env, "image_edit", args.data);
    ASSERT(result);
    ASSERT_EQ(2, prompts);
    free(result);
    perm_free(env.perm);
    buf_free(&args);
    rmdir(destination);
    unlink(record);
    unlink(artifact);
    unlink(reference);
    free(destination);
    free(record);
    free(artifact);
    free(reference);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    ctx.codex_base_url = NULL;
    PASS();
}

/* A record rewritten after the approval cannot redirect the request: the
 * originally approved settings and reference paths are what run. */
TEST image_prepared_plan_ignores_a_record_edited_after_approval(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    /* A closed loopback port: nothing here should reach a provider, and a
     * regression that tries must fail at once instead of leaving the host. */
    ctx.codex_base_url = "http://127.0.0.1:9/backend-api/codex";
    char *approved = write_png("pinned-ref.png", "one");
    char *substitute = write_png("substitute-ref.png", "two");
    char *artifact = write_png("pinned-art.png", "three");
    char approved_hash[TNY_IMAGE_SHA256_HEX], substitute_hash[TNY_IMAGE_SHA256_HEX];
    char artifact_hash[TNY_IMAGE_SHA256_HEX];
    hash_file(approved, approved_hash);
    hash_file(substitute, substitute_hash);
    hash_file(artifact, artifact_hash);
    tny_image_reference refs[1] = {fixture_reference(approved, approved_hash)};
    char *record = write_record("pinned.json", "1234abcd1234abcd", "codex", "a recorded prompt",
                                artifact, artifact_hash, refs, 1);
    char *destination = under_root("pinned-out.png");
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"output_file\":");
    jescape(&args, destination);
    buf_appends(&args, ",\"from_manifest\":");
    jescape(&args, record);
    buf_appends(&args, "}");

    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args.data, &call));
    ASSERT(call.image_plan);
    ASSERT_EQ(1u, call.image_plan->reference_count);
    ASSERT_STR_EQ(approved, call.image_plan->references[0].path);
    ASSERT_STR_EQ("a recorded prompt", call.image_plan->prompt);
    ASSERT(strstr(call.detail, approved));

    /* Rewrite the record: another provider, another prompt, another reference.
     * Then remove the reference this call was actually approved for. */
    tny_image_reference swapped[1] = {fixture_reference(substitute, substitute_hash)};
    unlink(record);
    free(record);
    record = write_record("pinned.json", "1234abcd1234abcd", "nonesuch", "a hijacked prompt",
                          artifact, artifact_hash, swapped, 1);
    unlink(approved);

    char *result = tools_call_execute(&env, &call);
    ASSERT(result);
    /* The approved reference is gone, so the call fails on it. It never used
     * the substituted provider, which would have failed much earlier. */
    ASSERT(strstr(result, "cannot load reference image"));
    ASSERT(!strstr(result, "unknown image provider"));
    ASSERT_EQ(0, access(substitute, F_OK));
    free(result);
    tools_call_free(&call);
    ASSERT(!call.image_plan);
    perm_free(env.perm);
    buf_free(&args);
    unlink(record);
    unlink(artifact);
    unlink(substitute);
    unlink(destination);
    free(destination);
    free(record);
    free(artifact);
    free(substitute);
    free(approved);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    ctx.codex_base_url = NULL;
    PASS();
}

/* The exact bytes are pinned too: a reference whose content changed after the
 * approval is refused before the request, not silently uploaded. */
TEST image_prepared_plan_refuses_changed_reference_bytes(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    /* A closed loopback port: nothing here should reach a provider, and a
     * regression that tries must fail at once instead of leaving the host. */
    ctx.codex_base_url = "http://127.0.0.1:9/backend-api/codex";
    char *reference = write_png("bytes-ref.png", "one");
    char *artifact = write_png("bytes-art.png", "two");
    char hash[TNY_IMAGE_SHA256_HEX], artifact_hash[TNY_IMAGE_SHA256_HEX];
    hash_file(reference, hash);
    hash_file(artifact, artifact_hash);
    tny_image_reference refs[1] = {fixture_reference(reference, hash)};
    char *record = write_record("bytes.json", "1234abcd1234abcd", "codex", "a recorded prompt",
                                artifact, artifact_hash, refs, 1);
    char *destination = under_root("bytes-out.png");
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"output_file\":");
    jescape(&args, destination);
    buf_appends(&args, ",\"from_manifest\":");
    jescape(&args, record);
    buf_appends(&args, "}");
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args.data, &call));
    ASSERT(call.image_plan);
    /* Replace the approved bytes in place, keeping the same path. */
    free(write_png("bytes-ref.png", "changed"));
    char *result = tools_call_execute(&env, &call);
    ASSERT(result);
    ASSERT(strstr(result, "no longer matches its hash"));
    ASSERT_EQ(-1, access(destination, F_OK));
    free(result);
    tools_call_free(&call);
    perm_free(env.perm);
    buf_free(&args);
    unlink(record);
    unlink(artifact);
    unlink(reference);
    free(destination);
    free(record);
    free(artifact);
    free(reference);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    ctx.codex_base_url = NULL;
    PASS();
}

/* Every permission-gated route needs its prepared plan; nothing re-resolves
 * one at execution time. */
TEST image_execution_without_a_prepared_plan_refuses(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    /* A closed loopback port: nothing here should reach a provider, and a
     * regression that tries must fail at once instead of leaving the host. */
    ctx.codex_base_url = "http://127.0.0.1:9/backend-api/codex";
    char *destination = under_root("unprepared.png");
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"prompt\":\"a prompt\",\"output_file\":");
    jescape(&args, destination);
    buf_appends(&args, "}");
    yyjson_doc *doc = jparse(args.data, args.len);
    ASSERT(doc);
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    buf_t out;
    buf_init(&out);
    char err[256] = "";
    ASSERT_EQ(1,
              tool_image_run(&env, yyjson_doc_get_root(doc), false, NULL, &out, err, sizeof err));
    ASSERT(strstr(err, "not prepared"));
    ASSERT_EQ(0u, out.len);
    ASSERT_EQ(-1, access(destination, F_OK));
    /* The same refusal at the service boundary itself. */
    tny_image_request r = {.prompt = "a prompt", .output_file = destination};
    tny_image_result result;
    err[0] = 0;
    ASSERT_EQ(1, tny_image_run_prepared(&ctx, &r, NULL, &result, err, sizeof err));
    ASSERT(strstr(err, "not prepared"));
    ASSERT(!result.committed);
    /* And the name-only executor chain refuses instead of running unprepared. */
    bool handled = false;
    char *refused = tool_ext_execute(&env, "image_generate", yyjson_doc_get_root(doc), &handled);
    ASSERT(handled);
    ASSERT(refused && strstr(refused, "not prepared"));
    free(refused);
    buf_free(&out);
    yyjson_doc_free(doc);
    perm_free(env.perm);
    buf_free(&args);
    free(destination);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    ctx.codex_base_url = NULL;
    PASS();
}

/* The retained-artifact failure is selected by tny's own code plus a committed
 * output, and reports the file it deliberately kept. */
TEST image_retained_failure_is_distinct_from_a_strict_rejection(void) {
    tny_image_result retained = {.edit = true, /* replay's literal r.edit below is false */
                                 .code = TNY_IMAGE_CODE_MANIFEST,
                                 .committed = true,
                                 .bytes = 12,
                                 .mime = "image/png",
                                 .width = 8,
                                 .height = 8,
                                 .provider = "codex"};
    retained.have_seed = true;
    retained.seed = 4242;
    snprintf(retained.request_id, sizeof retained.request_id, "provider-retained-sentinel");
    snprintf(retained.requested_size, sizeof retained.requested_size, "auto");
    snprintf(retained.operation_id, sizeof retained.operation_id, "1234abcd1234abcd");
    snprintf(retained.manifest_path, sizeof retained.manifest_path, "/w/out.png.tny-image-x.json");
    ASSERT(tny_image_retained_failure(&retained));
    ASSERT(!tny_image_strict_failure(&retained));
    /* Uncommitted, or any other code, is not a retained artifact. */
    tny_image_result uncommitted = retained;
    uncommitted.committed = false;
    ASSERT(!tny_image_retained_failure(&uncommitted));
    tny_image_result strict = retained;
    strict.code = TNY_IMAGE_CODE_MISMATCH;
    ASSERT(!tny_image_retained_failure(&strict));
    ASSERT(tny_image_strict_failure(&strict));
    ASSERT(!tny_image_retained_failure(NULL));
    tny_image_request r = {.prompt = "p", .output_file = "/w/out.png"};
    buf_t out;
    buf_init(&out);
    tny_image_retained_json(&r, &retained, "kept the image", &out);
    ASSERT(!out.oom);
    yyjson_doc *doc = jparse(out.data, out.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    ASSERT(root);
    ASSERT_STR_EQ("edit", jget_str(root, "operation"));
    ASSERT_STR_EQ("IMAGE_MANIFEST_FINALIZE_FAILED", jget_str(root, "code"));
    ASSERT_EQ(false, jget_bool(root, "ok", true));
    ASSERT_EQ(true, jget_bool(root, "committed", false));
    ASSERT_STR_EQ("/w/out.png", jget_str(root, "path"));
    ASSERT_EQ(12, (int)jget_int(root, "bytes", 0));
    ASSERT_STR_EQ("1234abcd1234abcd", jget_str(root, "operation_id"));
    ASSERT_STR_EQ("/w/out.png.tny-image-x.json", jget_str(root, "manifest_path"));
    ASSERT(!yyjson_obj_get(root, "seed"));
    ASSERT(!yyjson_obj_get(root, "request_id"));
    ASSERT(!strstr(out.data, "provider-retained-sentinel"));
    /* The same actual metadata must still survive the success serializer. */
    buf_t success;
    buf_init(&success);
    tny_image_result_json(&r, &retained, &success);
    yyjson_doc *success_doc = jparse(success.data, success.len);
    ASSERT(success_doc);
    yyjson_val *success_root = yyjson_doc_get_root(success_doc);
    ASSERT_EQ(4242, (int)jget_int(success_root, "seed", 0));
    ASSERT_STR_EQ("provider-retained-sentinel", jget_str(success_root, "request_id"));
    yyjson_doc_free(success_doc);
    buf_free(&success);
    /* A strict rejection keeps its own shape: nothing committed, no path. */
    buf_t strict_json;
    buf_init(&strict_json);
    tny_image_error_json(&r, &strict, "rejected", &strict_json);
    yyjson_doc *sdoc = jparse(strict_json.data, strict_json.len);
    yyjson_val *sroot = sdoc ? yyjson_doc_get_root(sdoc) : NULL;
    ASSERT(sroot);
    ASSERT_STR_EQ("edit", jget_str(sroot, "operation"));
    ASSERT_EQ(false, jget_bool(sroot, "committed", true));
    ASSERT(!jget_str(sroot, "path"));
    yyjson_doc_free(sdoc);
    yyjson_doc_free(doc);
    buf_free(&strict_json);
    buf_free(&out);
    PASS();
}

TEST image_private_writes_are_atomic_and_bounded(void) {
    char *path = under_root("atomic.json");
    ASSERT_EQ(0, tny_image_io_write_new(path, "first", 5));
    ASSERT_EQ(-1, tny_image_io_write_new(path, "second", 6));
    ASSERT_EQ(0, tny_image_io_replace(path, "replaced", 8));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0600, (int)(st.st_mode & 0777));
    buf_t out;
    buf_init(&out);
    ASSERT_EQ(0, tny_image_io_read_bounded(path, 64, &out));
    ASSERT_EQ(8u, out.len);
    ASSERT(memcmp(out.data, "replaced", 8) == 0);
    buf_free(&out);
    buf_init(&out);
    /* Over the bound, a directory and a missing file all fail with nothing
     * partially read. */
    ASSERT_EQ(-1, tny_image_io_read_bounded(path, 4, &out));
    ASSERT_EQ(0u, out.len);
    ASSERT_EQ(-1, tny_image_io_read_bounded(root, 64, &out));
    ASSERT_EQ(-1, tny_image_io_read_bounded("/nonexistent/at/all", 64, &out));
    buf_free(&out);
    char digest[65];
    ASSERT(tny_image_io_sha256_hex("abc", 3, digest));
    ASSERT_STR_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest);
    unlink(path);
    free(path);
    PASS();
}

TEST image_publication_never_replaces_a_competing_entry(void) {
    char err[256];
    char *output = under_root("publish.png");
    char *stage = under_root("publish-stage");
    ASSERT_EQ(0, tny_image_io_no_replace_preflight(output, err, sizeof err));
    ASSERT_EQ(0, tny_image_io_write_new(stage, "validated", 9));
    ASSERT_EQ(0, tny_image_io_write_new(output, "winner", 6));
    ASSERT_EQ(-1, tny_image_io_no_replace_preflight(output, err, sizeof err));
    ASSERT_EQ(-1, tny_image_io_publish(stage, output, true));
    buf_t bytes;
    buf_init(&bytes);
    ASSERT_EQ(0, tny_image_io_read_bounded(output, 32, &bytes));
    ASSERT_STR_EQ("winner", bytes.data);
    ASSERT_EQ(0, unlink(output));
    ASSERT_EQ(0, symlink("absent", output));
    ASSERT_EQ(-1, tny_image_io_no_replace_preflight(output, err, sizeof err));
    ASSERT_EQ(-1, tny_image_io_publish(stage, output, true));
    ASSERT_EQ(0, unlink(output));
    ASSERT_EQ(0, tny_image_io_publish(stage, output, true));
    ASSERT_EQ(-1, access(stage, F_OK));
    ASSERT_EQ(0, tny_image_io_write_new(stage, "replacement", 11));
    ASSERT_EQ(0, tny_image_io_publish(stage, output, false));
    buf_free(&bytes);
    buf_init(&bytes);
    ASSERT_EQ(0, tny_image_io_read_bounded(output, 32, &bytes));
    ASSERT_STR_EQ("replacement", bytes.data);
    buf_free(&bytes);
    unlink(output);
    free(output);
    free(stage);
    PASS();
}

/* Select the actual internal job record. No artifact exists: selection must
 * be metadata-only and must not project a running job to interrupted. */
TEST image_job_selection_is_owned_strict_and_metadata_only(void) {
    char err[256] = "";
    const char *id = "0123456789abcdef0123456789abcdef";
    char *workspace = path_abs(ctx.cwd);
    char *tny = path_join(workspace, "job-selection-state");
    char *jobs = path_join(tny, "jobs");
    char *dir = path_join(jobs, id);
    char *record_path = path_join(dir, "job.json");
    char *output = path_join(workspace, "not-created-selection.png");
    char *manifest = path_join(workspace, "selection-manifest.json");
    ASSERT_EQ(0, mkdir_p(dir));
    tny_ctx local = ctx;
    local.tny_dir = tny;
    local.cwd = workspace;
    const char *hash = "111122223333444455556666777788889999aaaabbbbccccddddeeeeffff0000";
    const char *tuples[][3] = {{"1", "1", "0"},    {"3", "1", "1"},          {"3", "3", "0"},
                               {"3", "1", "0"},    {"3", "3", "3"},          {"1", "3", "3"},
                               {"0", "0", "0"},    {"1.0", "1", "0"},        {"1", "true", "0"},
                               {"1", "1", "null"}, {"2147483648", "1", "1"}, {"1", "1", "-1"}};
    for (size_t i = 0; i < sizeof tuples / sizeof tuples[0]; i++) {
        buf_t json;
        buf_init(&json);
        buf_appendf(&json,
                    "{\"version\":1,\"kind\":\"job\",\"job_kind\":\"image\",\"id\":\"%s\","
                    "\"state\":\"running\",\"attempt\":%s,\"items\":[{\"index\":0,"
                    "\"state\":\"succeeded\",\"attempt\":%s,\"carried_from_attempt\":%s,"
                    "\"output_path\":\"%s\",\"output_sha256\":\"%s\",\"output_bytes\":9,"
                    "\"manifest_path\":null,\"operation_id\":\"a123\"}]}",
                    id, tuples[i][0], tuples[i][1], tuples[i][2], output, hash);
        ASSERT_EQ(0, file_write_atomic(record_path, json.data, json.len));
        tny_job_artifact *a = tny_jobs_select_artifact(&local, id, 0, err, sizeof err);
        if (i < 3) {
            ASSERT(a);
            ASSERT_EQ(atoi(tuples[i][0]), a->projection_attempt);
            ASSERT_EQ(atoi(tuples[i][1]), a->item_attempt);
            ASSERT_EQ(atoi(tuples[i][2]), a->carried_from_attempt);
            ASSERT_STR_EQ(hash, a->sha256);
            ASSERT_STR_EQ(output, a->path);
            ASSERT_EQ(9, a->bytes);
            ASSERT(!a->manifest);
            ASSERT_EQ(-1, access(output, F_OK));
            char *after = file_slurp(record_path, NULL);
            ASSERT_STR_EQ(json.data, after);
            free(after);
            ASSERT_EQ(0, unlink(record_path));
            ASSERT_STR_EQ(hash, a->sha256);
            ASSERT_STR_EQ(id, a->job_id);
        } else ASSERT(!a);
        tny_jobs_artifact_free(a);
        buf_free(&json);
    }
    ASSERT(!tny_jobs_select_artifact(&local, "../foreign", 0, err, sizeof err));
    ASSERT(!tny_jobs_select_artifact(&local, id, -1, err, sizeof err));
    ASSERT(!tny_jobs_select_artifact(&local, id, 64, err, sizeof err));

    tny_image_record m = {.operation_id = "a123",
                          .status = "succeeded",
                          .workspace = workspace,
                          .started = "2026-09-12T00:00:00Z",
                          .finished = "2026-09-12T00:00:01Z",
                          .prompt = "fixture",
                          .output = output,
                          .committed = true,
                          .requested_provider = "codex",
                          .output_sha256 = hash,
                          .mime = "image/png",
                          .bytes = 9};
    buf_t metadata;
    buf_init(&metadata);
    tny_image_manifest_serialize(&m, &metadata);
    ASSERT_EQ(0, file_write_atomic(manifest, metadata.data, metadata.len));
    buf_t json;
    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":1,\"kind\":\"job\",\"job_kind\":\"image\",\"id\":\"%s\","
                "\"state\":\"running\",\"attempt\":3,\"items\":[{\"index\":0,"
                "\"state\":\"succeeded\",\"attempt\":1,\"carried_from_attempt\":1,"
                "\"output_path\":\"%s\",\"output_sha256\":\"%s\",\"output_bytes\":9,"
                "\"manifest_path\":\"%s\",\"operation_id\":\"a123\"}]}",
                id, output, hash, manifest);
    ASSERT_EQ(0, file_write_atomic(record_path, json.data, json.len));
    tny_job_artifact *a = tny_jobs_select_artifact(&local, id, 0, err, sizeof err);
    ASSERTm(err, a);
    ASSERT(a->manifest);
    ASSERT_EQ(0, unlink(manifest));
    ASSERT_STR_EQ(output, a->manifest->artifact_path);
    ASSERT(!tny_jobs_select_artifact(&local, id, 0, err, sizeof err));
    tny_jobs_artifact_free(a);
    m.bytes = 10;
    buf_free(&metadata);
    buf_init(&metadata);
    tny_image_manifest_serialize(&m, &metadata);
    ASSERT_EQ(0, file_write_atomic(manifest, metadata.data, metadata.len));
    ASSERT(!tny_jobs_select_artifact(&local, id, 0, err, sizeof err));
    unlink(manifest);
    unlink(record_path);
    rmdir(dir);
    rmdir(jobs);
    rmdir(tny);
    buf_free(&json);
    buf_free(&metadata);
    free(workspace);
    free(tny);
    free(jobs);
    free(dir);
    free(record_path);
    free(output);
    free(manifest);
    PASS();
}

SUITE(image_service_suite) {
    const char *vars[] = {"HOME", "CODEX_HOME", "CHATGPT_ACCESS_TOKEN", "CHATGPT_ACCOUNT_ID"};
    char *saved[4];
    for (size_t i = 0; i < 4; i++) {
        const char *v = getenv(vars[i]);
        saved[i] = v ? xstrdup(v) : NULL;
        unsetenv(vars[i]);
    }
    snprintf(root, sizeof root, "/tmp/tny-image-unit-XXXXXX");
    if (!mkdtemp(root)) abort();
    setenv("HOME", root, 1);
    setenv("CODEX_HOME", root, 1);
    ctx = (tny_ctx){.cwd = root, .perm_mode = TNY_MODE_ASK, .tool_profile = TNY_TOOLS_ALL};
    RUN_TEST(image_publication_never_replaces_a_competing_entry);
    RUN_TEST(image_job_selection_is_owned_strict_and_metadata_only);
    RUN_TEST(image_capability_and_validation);
    RUN_TEST(image_dimensions_from_real_headers);
    RUN_TEST(image_dimensions_reject_impossible_headers);
    RUN_TEST(image_size_requests_and_status);
    RUN_TEST(image_strict_size_is_settled_before_any_request);
    RUN_TEST(image_prepared_quality_is_checked_before_provider_availability);
    RUN_TEST(image_base64_is_strict_and_magic_checked);
    RUN_TEST(image_decode_exact_output_limit);
    RUN_TEST(image_invalid_later_reference_is_an_error);
    RUN_TEST(image_tool_schema_permission_and_interception);
    RUN_TEST(image_manifest_round_trip_and_strict_parsing);
    RUN_TEST(image_reserved_names_anchor_on_the_final_suffix);
    RUN_TEST(image_manifest_rejects_unusable_records);
    RUN_TEST(image_preview_permission_detail_is_explicit_and_false_is_unchanged);
    RUN_TEST(image_prepared_plan_runs_under_a_one_time_approval);
    RUN_TEST(image_prepared_plan_ignores_a_record_edited_after_approval);
    RUN_TEST(image_prepared_plan_refuses_changed_reference_bytes);
    RUN_TEST(image_execution_without_a_prepared_plan_refuses);
    RUN_TEST(image_retained_failure_is_distinct_from_a_strict_rejection);
    RUN_TEST(image_destinations_reject_aliases);
    RUN_TEST(image_writer_guard_is_exclusive_and_identified);
    RUN_TEST(image_running_record_without_a_live_owner_is_interrupted);
    RUN_TEST(image_private_writes_are_atomic_and_bounded);
    RUN_TEST(image_export_grammar_and_settings);
    RUN_TEST(image_export_tools_carry_the_whole_operation);
    RUN_TEST(image_export_records_are_derived_and_never_replayed);
    RUN_TEST(image_export_allow_once_is_exact_and_scoped);
    RUN_TEST(image_export_snapshot_and_retained_result);
    RUN_TEST(image_export_commit_never_follows_its_target);
    rmdir(root);
    for (size_t i = 0; i < 4; i++) {
        if (saved[i]) setenv(vars[i], saved[i], 1);
        else unsetenv(vars[i]);
        free(saved[i]);
    }
}
