/* Bounded PNG/JPEG/WebP header dimensions (ADR 0088). Every read is checked
 * against the caller's length and the format's own declared lengths first;
 * truncated, inconsistent or impossible headers report nothing. */
#include "core/image_dimensions.h"
#include <string.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t le24(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}
static uint32_t le32(const uint8_t *p) { return le24(p) | (uint32_t)p[3] << 24; }

static bool edge_ok(uint32_t v) { return v && v <= TNY_IMAGE_DIMENSION_MAX; }

/* PNG colour types accept only their own bit depths (PNG 1.2, table 11.1). */
static bool png_depth_ok(uint8_t colour, uint8_t depth) {
    switch (colour) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

/* Only the 17-byte type+IHDR data is checksummed; compressed pixels are not
 * decoded or otherwise certified by this metadata reader. */
static uint32_t png_header_crc(const uint8_t *header) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < 17; i++) {
        crc ^= header[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xEDB88320) : 0);
    }
    return crc ^ UINT32_MAX;
}

static tny_image_dim_status png_dimensions(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h) {
    /* signature, length, type, the whole 13-byte IHDR and its CRC field. */
    if (n < 8 + 4 + 4 + 13 + 4 || be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4) != 0)
        return TNY_IMAGE_DIM_UNVERIFIABLE;
    uint32_t width = be32(d + 16), height = be32(d + 20);
    if (!edge_ok(width) || !edge_ok(height) || !png_depth_ok(d[25], d[24]) || d[26] || d[27] ||
        d[28] > 1 || png_header_crc(d + 12) != be32(d + 29))
        return TNY_IMAGE_DIM_UNVERIFIABLE;
    *w = width;
    *h = height;
    return TNY_IMAGE_DIM_OK;
}

static bool jpeg_frame(uint8_t marker) {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

/* Sequential, extended and progressive frames — Huffman or arithmetic — carry
 * one plain raster whose frame header this reader understands. Lossless,
 * differential and hierarchical frames do not, and are reported as such. */
static bool jpeg_frame_supported(uint8_t marker) {
    return marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || marker == 0xC9 || marker == 0xCA;
}

static tny_image_dim_status jpeg_dimensions(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h) {
    for (size_t i = 2; i + 1 < n;) { /* the SOI itself is the magic bytes */
        if (d[i] != 0xFF) return TNY_IMAGE_DIM_UNVERIFIABLE;
        uint8_t marker = d[i + 1];
        if (marker == 0xFF) { /* legal fill byte before the next marker */
            i++;
            continue;
        }
        i += 2;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        /* A scan or end of image before any frame header leaves nothing to read. */
        if (marker == 0x00 || marker == 0xD9 || marker == 0xDA) return TNY_IMAGE_DIM_UNVERIFIABLE;
        if (i + 2 > n) return TNY_IMAGE_DIM_UNVERIFIABLE;
        uint32_t segment = be16(d + i);
        if (segment < 2 || segment > n - i) return TNY_IMAGE_DIM_UNVERIFIABLE;
        if (jpeg_frame(marker)) {
            if (!jpeg_frame_supported(marker)) return TNY_IMAGE_DIM_UNSUPPORTED;
            if (segment < 8) return TNY_IMAGE_DIM_UNVERIFIABLE;
            uint32_t precision = d[i + 2], height = be16(d + i + 3), width = be16(d + i + 5),
                     components = d[i + 7];
            if ((precision != 8 && (marker == 0xC0 || precision != 12)) || !components ||
                segment != 8 + 3 * components || !edge_ok(width) || !edge_ok(height))
                return TNY_IMAGE_DIM_UNVERIFIABLE;
            *w = width;
            *h = height;
            return TNY_IMAGE_DIM_OK;
        }
        i += segment;
    }
    return TNY_IMAGE_DIM_UNVERIFIABLE;
}

static tny_image_dim_status webp_dimensions(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h) {
    if (n < 12) return TNY_IMAGE_DIM_UNVERIFIABLE;
    uint32_t riff = le32(d + 4);
    if (riff < 4 || riff > UINT32_MAX - 9 || (riff & 1) || riff > n - 8)
        return TNY_IMAGE_DIM_UNVERIFIABLE;
    uint32_t width = 0, height = 0;
    size_t i = 12, end = (size_t)8 + riff;
    while (end - i >= 8) {
        const uint8_t *tag = d + i, *payload = d + i + 8;
        uint32_t size = le32(d + i + 4);
        if (size > end - (i + 8)) return TNY_IMAGE_DIM_UNVERIFIABLE;
        if ((size & 1) && (size == end - (i + 8) || payload[size] != 0))
            return TNY_IMAGE_DIM_UNVERIFIABLE;
        if (memcmp(tag, "VP8 ", 4) == 0) {
            /* Lossy key frame: 3-byte frame tag, start code, 14-bit edges. */
            if (size < 10) return TNY_IMAGE_DIM_UNVERIFIABLE;
            if ((le24(payload) & 1) || payload[3] != 0x9D || payload[4] != 0x01 ||
                payload[5] != 0x2A)
                return TNY_IMAGE_DIM_UNVERIFIABLE;
            width = (payload[6] | (uint32_t)payload[7] << 8) & 0x3FFF;
            height = (payload[8] | (uint32_t)payload[9] << 8) & 0x3FFF;
        } else if (memcmp(tag, "VP8L", 4) == 0) {
            if (size < 5 || payload[0] != 0x2F) return TNY_IMAGE_DIM_UNVERIFIABLE;
            uint32_t bits = le32(payload + 1);
            if (bits >> 29) return TNY_IMAGE_DIM_UNVERIFIABLE;
            width = (bits & 0x3FFF) + 1;
            height = ((bits >> 14) & 0x3FFF) + 1;
        } else if (memcmp(tag, "VP8X", 4) == 0) {
            if (size != 10) return TNY_IMAGE_DIM_UNVERIFIABLE;
            width = le24(payload + 4) + 1; /* canvas size, one based */
            height = le24(payload + 7) + 1;
            /* Readers must ignore VP8X reserved fields, but the canvas
             * product has an explicit format bound independent of edges. */
            if ((uint64_t)width * height > UINT32_MAX) return TNY_IMAGE_DIM_UNVERIFIABLE;
        } else {
            i += (size_t)8 + size + (size & 1); /* odd payloads carry one padding byte */
            continue;
        }
        if (!edge_ok(width) || !edge_ok(height)) return TNY_IMAGE_DIM_UNVERIFIABLE;
        *w = width;
        *h = height;
        return TNY_IMAGE_DIM_OK;
    }
    /* Only a complete unknown chunk sequence is an unsupported encoding. */
    return i == end ? TNY_IMAGE_DIM_UNSUPPORTED : TNY_IMAGE_DIM_UNVERIFIABLE;
}

tny_image_dim_status tny_image_dimensions(const uint8_t *data, size_t n, uint32_t *width,
                                          uint32_t *height) {
    uint32_t w = 0, h = 0;
    tny_image_dim_status status = TNY_IMAGE_DIM_UNVERIFIABLE;
    if (data && n >= 12) {
        if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) status = png_dimensions(data, n, &w, &h);
        else if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
            status = jpeg_dimensions(data, n, &w, &h);
        else if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
            status = webp_dimensions(data, n, &w, &h);
    }
    if (status != TNY_IMAGE_DIM_OK) w = h = 0;
    if (width) *width = w;
    if (height) *height = h;
    return status;
}

static bool parse_edge(const char **text, uint32_t *out) {
    const char *p = *text;
    uint32_t value = 0;
    size_t digits = 0;
    if (*p < '1' || *p > '9') return false; /* rejects signs, spaces and leading zeros */
    for (; *p >= '0' && *p <= '9'; p++) {
        if (++digits > 7) return false;
        value = value * 10 + (uint32_t)(*p - '0');
    }
    *text = p;
    *out = value;
    return edge_ok(value);
}

bool tny_image_size_parse(const char *size, uint32_t *width, uint32_t *height) {
    uint32_t w = 0, h = 0;
    const char *p = size;
    bool ok = p && parse_edge(&p, &w) && *p == 'x';
    if (ok) {
        p++;
        ok = parse_edge(&p, &h) && !*p;
    }
    if (width) *width = ok ? w : 0;
    if (height) *height = ok ? h : 0;
    return ok;
}

tny_image_size_status tny_image_size_compare(const char *requested, tny_image_dim_status dimensions,
                                             uint32_t width, uint32_t height) {
    uint32_t want_width, want_height;
    if (!requested || !*requested || strcmp(requested, "auto") == 0) return TNY_IMAGE_SIZE_AUTO;
    if (dimensions == TNY_IMAGE_DIM_UNSUPPORTED) return TNY_IMAGE_SIZE_UNSUPPORTED;
    /* An opaque provider size token has no known dimensional meaning; it is
     * never resolved into a match from the returned bytes alone. */
    if (dimensions != TNY_IMAGE_DIM_OK ||
        !tny_image_size_parse(requested, &want_width, &want_height))
        return TNY_IMAGE_SIZE_UNVERIFIABLE;
    return want_width == width && want_height == height ? TNY_IMAGE_SIZE_MATCH
                                                        : TNY_IMAGE_SIZE_MISMATCH;
}

const char *tny_image_size_status_name(tny_image_size_status status) {
    switch (status) {
    case TNY_IMAGE_SIZE_MATCH: return "match";
    case TNY_IMAGE_SIZE_MISMATCH: return "mismatch";
    case TNY_IMAGE_SIZE_UNVERIFIABLE: return "unverifiable";
    case TNY_IMAGE_SIZE_UNSUPPORTED: return "unsupported";
    case TNY_IMAGE_SIZE_AUTO: break;
    }
    return "auto";
}
