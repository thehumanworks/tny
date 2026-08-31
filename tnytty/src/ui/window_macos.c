/* window_macos.c — the macOS native window (docs/adr/0005).
 *
 * AppKit, CoreText and CoreAnimation are driven through the Objective-C
 * runtime from plain C11: objc_getClass / sel_registerName / typed casts
 * of objc_msgSend. No .m file, no Objective-C compiler, so the tnytty
 * build stays a C11 build (root AGENTS.md invariant).
 *
 * This file owns exactly three things: the window and its layer, the
 * CoreText glyph cache that feeds the platform-free rasterizer, and the
 * translation of NSEvent key presses into (tt_key, mods, text). It runs
 * no loop: tt_window_pump() drains AppKit's queue and returns, leaving
 * the single poll(2) loop in gui.c in charge. */
#include "ui/window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AppKit enum values; the headers are Objective-C, the numbers are ABI. */
#define NS_STYLE_TITLED           (1UL << 0)
#define NS_STYLE_CLOSABLE         (1UL << 1)
#define NS_STYLE_MINIATURIZABLE   (1UL << 2)
#define NS_STYLE_RESIZABLE        (1UL << 3)
#define NS_STYLE_FULLSIZE_CONTENT (1UL << 15)
#define NS_BACKING_BUFFERED       2UL
#define NS_ACTIVATION_REGULAR     0L
#define NS_TITLE_HIDDEN           1L
#define NS_EVENT_L_MOUSE_DOWN     1UL
#define NS_EVENT_L_MOUSE_UP       2UL
#define NS_EVENT_L_MOUSE_DRAGGED  6UL
#define NS_EVENT_KEY_DOWN         10UL
#define NS_EVENT_MASK_ANY         (~0UL)
#define NS_MOD_SHIFT              (1UL << 17)
#define NS_MOD_CONTROL            (1UL << 18)
#define NS_MOD_OPTION             (1UL << 19)
#define NS_MOD_COMMAND            (1UL << 20)
#define NS_VIEW_SIZABLE           (2UL | 16UL) /* width | height */
/* Public NSVisualEffectView enums (AppKit/NSVisualEffectView.h). */
#define NS_VISUAL_UNDER_WINDOW 21L
#define NS_VISUAL_BEHIND       0L
#define NS_VISUAL_FOLLOWS      0L
#define NS_GLASS_REGULAR       0L

/* NSEvent function-key codepoints (NSUpArrowFunctionKey & friends). */
#define FN_UP     0xF700
#define FN_DOWN   0xF701
#define FN_LEFT   0xF702
#define FN_RIGHT  0xF703
#define FN_F1     0xF704
#define FN_F12    0xF70F
#define FN_INSERT 0xF727
#define FN_DELETE 0xF728
#define FN_HOME   0xF729
#define FN_END    0xF72B
#define FN_PGUP   0xF72C
#define FN_PGDN   0xF72D

/* ---- objc_msgSend, typed --------------------------------------------- */

static id msg(id r, SEL op) { return ((id (*)(id, SEL))objc_msgSend)(r, op); }
static id msg_p(id r, SEL op, const void *a) {
    return ((id (*)(id, SEL, const void *))objc_msgSend)(r, op, a);
}
static void msg_v(id r, SEL op) { ((void (*)(id, SEL))objc_msgSend)(r, op); }
static void msg_vp(id r, SEL op, const void *a) {
    ((void (*)(id, SEL, const void *))objc_msgSend)(r, op, a);
}
static void msg_vb(id r, SEL op, BOOL a) { ((void (*)(id, SEL, BOOL))objc_msgSend)(r, op, a); }
static void msg_vi(id r, SEL op, long a) { ((void (*)(id, SEL, long))objc_msgSend)(r, op, a); }
static void msg_vu(id r, SEL op, unsigned long a) {
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(r, op, a);
}
static void msg_vd(id r, SEL op, double a) { ((void (*)(id, SEL, double))objc_msgSend)(r, op, a); }
static BOOL msg_b(id r, SEL op) { return ((BOOL (*)(id, SEL))objc_msgSend)(r, op); }
static unsigned long msg_ul(id r, SEL op) {
    return ((unsigned long (*)(id, SEL))objc_msgSend)(r, op);
}

/* Struct and floating-point returns take a different entry point on
 * x86_64; arm64 returns both in registers through objc_msgSend. */
#if defined(__x86_64__)
static CGRect msg_rect(id r, SEL op) {
    CGRect out;
    ((void (*)(CGRect *, id, SEL))objc_msgSend_stret)(&out, r, op);
    return out;
}
static CGRect msg_rect_frame_style(id r, SEL op, CGRect a, unsigned long b) {
    CGRect out;
    ((void (*)(CGRect *, id, SEL, CGRect, unsigned long))objc_msgSend_stret)(&out, r, op, a, b);
    return out;
}
static double msg_dbl(id r, SEL op) { return ((double (*)(id, SEL))objc_msgSend_fpret)(r, op); }
#else
static CGRect msg_rect(id r, SEL op) { return ((CGRect (*)(id, SEL))objc_msgSend)(r, op); }
static CGRect msg_rect_frame_style(id r, SEL op, CGRect a, unsigned long b) {
    return ((CGRect (*)(id, SEL, CGRect, unsigned long))objc_msgSend)(r, op, a, b);
}
static double msg_dbl(id r, SEL op) { return ((double (*)(id, SEL))objc_msgSend)(r, op); }
#endif

static id cls(const char *name) { return (id)objc_getClass(name); }
static SEL sel(const char *name) { return sel_registerName(name); }
static id nsstr(const char *s) {
    return ((id (*)(id, SEL, const char *))objc_msgSend)(cls("NSString"),
                                                         sel("stringWithUTF8String:"), s ? s : "");
}

/* ---- CoreText glyph cache -------------------------------------------- */

#define FACE_REGULAR     0
#define FACE_BOLD        1
#define FACE_ITALIC      2
#define FACE_BOLD_ITALIC 3
#define FACE_COUNT       4
#define GLYPH_SLOTS      2048

typedef struct {
    uint64_t key; /* 0 = empty; else (cp + 1) | face << 40 */
    uint8_t *alpha;
    int w, h, left, top;
    bool blank;
} glyph_slot;

struct tt_window {
    id app, win, backdrop, view, layer;
    CTFontRef face[FACE_COUNT];
    int cell_w, cell_h, baseline;
    int pad_left, pad_top, pad_right, pad_bottom;
    double scale;
    uint32_t fg, bg;
    uint32_t palette[TT_PALETTE_LEN];
    bool bold_brightens;
    int status_h;
    uint32_t status_fg, status_bg;
    char *clip; /* last pasteboard read, owned */
    glyph_slot *cache;
    int cache_used;
    /* mask scratch: 3 cells wide, 2 cells tall, cell-relative origin at
     * (cell_w, cell_h / 2) so a nerd glyph may overhang either side. */
    CGContextRef mask_ctx;
    uint8_t *mask_px;
    int mask_w, mask_h, mask_stride;
    /* present */
    CGContextRef bmp_ctx;
    uint32_t *bmp_px;
    const uint32_t *bmp_src;
    int bmp_w, bmp_h;
    unsigned backdrop_alpha;
    bool backdrop_blur;
    /* event state */
    int last_px_w, last_px_h;
    bool last_focus, closed, close_sent;
};

static CTFontRef make_face(const char *name, double size, CTFontSymbolicTraits traits) {
    CFStringRef cfname =
        CFStringCreateWithCString(NULL, name && *name ? name : "Menlo", kCFStringEncodingUTF8);
    CTFontRef base = cfname ? CTFontCreateWithName(cfname, size, NULL) : NULL;
    if (cfname) CFRelease(cfname);
    if (!base) base = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, size, NULL);
    if (!base || !traits) return base;
    CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(base, size, NULL, traits, traits);
    if (!styled) return base;
    CFRelease(base);
    return styled;
}

static int face_index(uint16_t attrs) {
    int f = 0;
    if (attrs & VT_ATTR_BOLD) f |= FACE_BOLD;
    if (attrs & VT_ATTR_ITALIC) f |= FACE_ITALIC;
    return f;
}

static uint64_t glyph_key(uint32_t cp, int face) {
    return ((uint64_t)cp + 1) | ((uint64_t)face << 40);
}

static void cache_clear(tt_window *w) {
    for (int i = 0; i < GLYPH_SLOTS; i++) {
        free(w->cache[i].alpha);
        w->cache[i].alpha = NULL;
        w->cache[i].key = 0;
    }
    w->cache_used = 0;
}

/* Rasterize one codepoint through CTLine, which picks fallback fonts for
 * PUA/nerd and emoji codepoints and handles non-BMP surrogate pairs. */
static bool raster(tt_window *w, uint32_t cp, int face, glyph_slot *slot) {
    UniChar u16[2];
    CFIndex n = 0;
    if (cp < 0x10000) {
        u16[n++] = (UniChar)cp;
    } else {
        uint32_t v = cp - 0x10000;
        u16[n++] = (UniChar)(0xd800 + (v >> 10));
        u16[n++] = (UniChar)(0xdc00 + (v & 0x3ff));
    }
    CFStringRef str = CFStringCreateWithCharacters(NULL, u16, n);
    if (!str) return false;
    CFStringRef keys[1] = {kCTFontAttributeName};
    CFTypeRef vals[1] = {w->face[face]};
    CFDictionaryRef attrs =
        CFDictionaryCreate(NULL, (const void **)keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = attrs ? CFAttributedStringCreate(NULL, str, attrs) : NULL;
    CTLineRef line = as ? CTLineCreateWithAttributedString(as) : NULL;
    CFRelease(str);
    if (attrs) CFRelease(attrs);
    if (as) CFRelease(as);
    if (!line) return false;

    memset(w->mask_px, 0, (size_t)w->mask_stride * (size_t)w->mask_h);
    double pen_x = w->cell_w;
    /* CG bitmap origin is bottom-left; the cell's top edge sits
     * top_slack pixels below the mask's top edge (integer, because the
     * mask offsets handed back to the rasterizer are whole pixels). */
    int top_slack = w->cell_h / 2;
    double pen_y = (double)(w->mask_h - top_slack - w->baseline);
    CGContextSetTextPosition(w->mask_ctx, pen_x, pen_y);
    CTLineDraw(line, w->mask_ctx);
    CFRelease(line);

    int x0 = w->mask_w, y0 = w->mask_h, x1 = -1, y1 = -1;
    for (int y = 0; y < w->mask_h; y++) {
        const uint8_t *row = w->mask_px + (size_t)y * (size_t)w->mask_stride;
        for (int x = 0; x < w->mask_w; x++) {
            if (!row[x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) {
        slot->blank = true;
        return true;
    }
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    uint8_t *a = malloc((size_t)bw * (size_t)bh);
    if (!a) return false;
    for (int y = 0; y < bh; y++)
        memcpy(a + (size_t)y * (size_t)bw,
               w->mask_px + (size_t)(y0 + y) * (size_t)w->mask_stride + x0, (size_t)bw);
    slot->alpha = a;
    slot->w = bw;
    slot->h = bh;
    slot->left = x0 - w->cell_w;
    slot->top = y0 - top_slack;
    slot->blank = false;
    return true;
}

static bool glyph_lookup(void *user, uint32_t cp, uint16_t attrs, tt_glyph *out) {
    tt_window *w = user;
    int face = face_index(attrs);
    uint64_t key = glyph_key(cp, face);
    size_t i = (size_t)((key * 0x9e3779b97f4a7c15ull) >> 51) % GLYPH_SLOTS;
    for (int probe = 0; probe < GLYPH_SLOTS; probe++) {
        glyph_slot *s = &w->cache[(i + (size_t)probe) % GLYPH_SLOTS];
        if (s->key == key) {
            if (s->blank) return false;
            out->alpha = s->alpha;
            out->w = s->w;
            out->h = s->h;
            out->left = s->left;
            out->top = s->top;
            return true;
        }
        if (s->key) continue;
        if (w->cache_used > GLYPH_SLOTS * 3 / 4) cache_clear(w);
        s = &w->cache[(i + (size_t)probe) % GLYPH_SLOTS];
        memset(s, 0, sizeof *s);
        if (!raster(w, cp, face, s)) return false;
        s->key = key;
        w->cache_used++;
        if (s->blank) return false;
        out->alpha = s->alpha;
        out->w = s->w;
        out->h = s->h;
        out->left = s->left;
        out->top = s->top;
        return true;
    }
    return false;
}

/* ---- window ----------------------------------------------------------- */

/* NSEvent location -> device pixels with a top-left origin, matching the
 * rasterizer's framebuffer. NSView's default coordinates run bottom-up.
 *
 * Returns false when the press did not land on the view. That is not a
 * nicety: AppKit synthesizes mouse-downs whose locationInWindow is a
 * sentinel far outside the window (300000, -299382 on macOS 27) when it
 * activates an app, and a caller that clamps such a point onto the grid
 * starts a selection in the last cell -- a phantom caret in the corner
 * that no mouse-up ever clears (docs/adr/0006). */
static bool event_point(tt_window *w, id e, int *px, int *py) {
    CGPoint p = ((CGPoint (*)(id, SEL))objc_msgSend)(e, sel("locationInWindow"));
    CGPoint v = ((CGPoint (*)(id, SEL, CGPoint, id))objc_msgSend)(
        w->view, sel("convertPoint:fromView:"), p, NULL);
    CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(w->view, sel("bounds"));
    *px = (int)lround(v.x * w->scale);
    *py = (int)lround((b.size.height - v.y) * w->scale);
    return v.x >= 0.0 && v.y >= 0.0 && v.x < b.size.width && v.y < b.size.height;
}

static void backing_size(tt_window *w, int *px_w, int *px_h) {
    CGRect b = msg_rect(w->view, sel("bounds"));
    int bw = (int)lround(b.size.width * w->scale);
    int bh = (int)lround(b.size.height * w->scale);
    if (px_w) *px_w = bw < 1 ? 1 : bw;
    if (px_h) *px_h = bh < 1 ? 1 : bh;
}

static double titlebar_height(void) {
    CGRect frame = CGRectMake(0, 0, 600, 400);
    unsigned long style =
        NS_STYLE_TITLED | NS_STYLE_CLOSABLE | NS_STYLE_MINIATURIZABLE | NS_STYLE_RESIZABLE;
    CGRect content = msg_rect_frame_style(cls("NSWindow"),
                                          sel("contentRectForFrameRect:styleMask:"), frame, style);
    double h = frame.size.height - content.size.height;
    return h > 0 ? h : 28.0;
}

tt_window *tt_window_open(const tt_config *cfg, int cols, int rows, const char *title, char *err,
                          size_t errcap) {
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    tt_window *w = calloc(1, sizeof *w);
    if (!w) {
        if (err && errcap) snprintf(err, errcap, "gui: out of memory");
        msg_v(pool, sel("drain"));
        return NULL;
    }
    w->cache = calloc(GLYPH_SLOTS, sizeof *w->cache);
    if (!w->cache) {
        free(w);
        if (err && errcap) snprintf(err, errcap, "gui: out of memory");
        msg_v(pool, sel("drain"));
        return NULL;
    }
    /* Dark by default: the window background is the terminal background,
     * so the content reads continuously under the traffic lights. The
     * palette itself lives in the config (docs/config.md). */
    w->fg = cfg->fg;
    w->bg = cfg->bg;
    int backdrop_opacity = cfg->backdrop_opacity;
    if (backdrop_opacity < 0) backdrop_opacity = 0;
    if (backdrop_opacity > 100) backdrop_opacity = 100;
    w->backdrop_alpha = (unsigned)(backdrop_opacity * 255 + 50) / 100;
    w->backdrop_blur = cfg->backdrop_blur;
    memcpy(w->palette, cfg->palette, sizeof w->palette);
    w->bold_brightens = cfg->bold_brightens;
    /* A bar just lighter than the terminal background, with dimmed
     * foreground text: legible without competing with the grid. */
    w->status_bg = tt_render_mix(cfg->bg, cfg->fg, 26);
    w->status_fg = tt_render_mix(cfg->bg, cfg->fg, 160);

    w->app = msg(cls("NSApplication"), sel("sharedApplication"));
    if (!w->app) {
        if (err && errcap) snprintf(err, errcap, "gui: no window server (headless session?)");
        goto fail;
    }
    ((void (*)(id, SEL, long))objc_msgSend)(w->app, sel("setActivationPolicy:"),
                                            NS_ACTIVATION_REGULAR);

    id screen = msg(cls("NSScreen"), sel("mainScreen"));
    w->scale = screen ? msg_dbl(screen, sel("backingScaleFactor")) : 1.0;
    if (!(w->scale >= 1.0)) w->scale = 1.0;

    double px_size = cfg->font_size * w->scale;
    w->face[FACE_REGULAR] = make_face(cfg->font, px_size, 0);
    w->face[FACE_BOLD] = make_face(cfg->font, px_size, kCTFontTraitBold);
    w->face[FACE_ITALIC] = make_face(cfg->font, px_size, kCTFontTraitItalic);
    w->face[FACE_BOLD_ITALIC] =
        make_face(cfg->font, px_size, kCTFontTraitBold | kCTFontTraitItalic);
    if (!w->face[FACE_REGULAR]) {
        if (err && errcap) snprintf(err, errcap, "gui: cannot load font '%s'", cfg->font);
        goto fail;
    }
    for (int i = 1; i < FACE_COUNT; i++)
        if (!w->face[i]) w->face[i] = (CTFontRef)CFRetain(w->face[FACE_REGULAR]);

    UniChar m = 'M';
    CGGlyph mg = 0;
    double adv = px_size * 0.6;
    if (CTFontGetGlyphsForCharacters(w->face[FACE_REGULAR], &m, &mg, 1))
        adv = CTFontGetAdvancesForGlyphs(w->face[FACE_REGULAR], kCTFontOrientationHorizontal, &mg,
                                         NULL, 1);
    double ascent = CTFontGetAscent(w->face[FACE_REGULAR]);
    double descent = CTFontGetDescent(w->face[FACE_REGULAR]);
    double leading = CTFontGetLeading(w->face[FACE_REGULAR]);
    w->cell_w = (int)ceil(adv);
    w->cell_h = (int)ceil(ascent + descent + leading);
    w->baseline = (int)ceil(ascent);
    if (w->cell_w < 1) w->cell_w = 1;
    if (w->cell_h < 1) w->cell_h = 1;

    w->status_h = cfg->status_bar ? w->cell_h + (int)lround(4.0 * w->scale) : 0;
    int pad = (int)lround((double)cfg->padding * w->scale);
    bool transparent = cfg->titlebar == TT_TITLEBAR_TRANSPARENT;
    double tb = titlebar_height();
    w->pad_left = w->pad_right = w->pad_bottom = pad;
    /* Transparent titlebar: the grid starts below the traffic lights so
     * no text hides behind them. */
    w->pad_top = pad + (transparent ? (int)lround(tb * w->scale) : 0);

    w->mask_w = w->cell_w * 3;
    w->mask_h = w->cell_h * 2;
    w->mask_stride = w->mask_w;
    w->mask_px = calloc((size_t)w->mask_stride * (size_t)w->mask_h, 1);
    if (!w->mask_px) {
        if (err && errcap) snprintf(err, errcap, "gui: out of memory");
        goto fail;
    }
    w->mask_ctx = CGBitmapContextCreate(w->mask_px, (size_t)w->mask_w, (size_t)w->mask_h, 8,
                                        (size_t)w->mask_stride, NULL, kCGImageAlphaOnly);
    if (!w->mask_ctx) {
        if (err && errcap) snprintf(err, errcap, "gui: cannot create a glyph bitmap");
        goto fail;
    }
    CGContextSetShouldAntialias(w->mask_ctx, true);
    CGContextSetShouldSmoothFonts(w->mask_ctx, false);

    double content_w = (double)(w->pad_left + w->pad_right + cols * w->cell_w) / w->scale;
    double content_h =
        (double)(w->pad_top + w->pad_bottom + w->status_h + rows * w->cell_h) / w->scale;
    CGRect rect = CGRectMake(0, 0, content_w, content_h);
    unsigned long style =
        NS_STYLE_TITLED | NS_STYLE_CLOSABLE | NS_STYLE_MINIATURIZABLE | NS_STYLE_RESIZABLE;
    if (transparent) style |= NS_STYLE_FULLSIZE_CONTENT;

    w->win = ((id (*)(id, SEL, CGRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        msg(cls("NSWindow"), sel("alloc")), sel("initWithContentRect:styleMask:backing:defer:"),
        rect, style, NS_BACKING_BUFFERED, NO);
    if (!w->win) {
        if (err && errcap) snprintf(err, errcap, "gui: cannot create a window");
        goto fail;
    }
    /* The polling seam observes a red-button close after AppKit handles the
     * event. Retain the NSWindow until tt_window_close so `view` and `layer`
     * cannot become dangling pointers between those two steps. */
    msg_vb(w->win, sel("setReleasedWhenClosed:"), NO);
    msg_vp(w->win, sel("setTitle:"), nsstr(title ? title : "tnytty"));
    id appearance =
        msg_p(cls("NSAppearance"), sel("appearanceNamed:"), nsstr("NSAppearanceNameDarkAqua"));
    if (appearance) msg_vp(w->win, sel("setAppearance:"), appearance);
    id color = ((id (*)(id, SEL, double, double, double, double))objc_msgSend)(
        cls("NSColor"), sel("colorWithSRGBRed:green:blue:alpha:"),
        (double)((w->bg >> 16) & 0xff) / 255.0, (double)((w->bg >> 8) & 0xff) / 255.0,
        (double)(w->bg & 0xff) / 255.0, 1.0);
    /* The framebuffer supplies premultiplied alpha for default-background
     * pixels. A transparent NSWindow lets the public compositor material (or
     * the unblurred desktop when disabled) show through those pixels. */
    msg_vb(w->win, sel("setOpaque:"), NO);
    id clear = msg(cls("NSColor"), sel("clearColor"));
    msg_vp(w->win, sel("setBackgroundColor:"), clear ? clear : color);
    if (transparent) {
        msg_vb(w->win, sel("setTitlebarAppearsTransparent:"), YES);
        msg_vi(w->win, sel("setTitleVisibility:"), NS_TITLE_HIDDEN);
    }

    w->view = msg(((id (*)(id, SEL, CGRect))objc_msgSend)(msg(cls("NSView"), sel("alloc")),
                                                          sel("initWithFrame:"), rect),
                  sel("autorelease"));
    msg_vu(w->view, sel("setAutoresizingMask:"), NS_VIEW_SIZABLE);
    /* Layer-hosting (setLayer: before setWantsLayer:) keeps AppKit from
     * repainting over the bitmap we blit. */
    w->layer = msg(msg(cls("CALayer"), sel("alloc")), sel("init"));
    msg_vp(w->layer, sel("setContentsGravity:"), CFSTR("topLeft"));
    msg_vd(w->layer, sel("setContentsScale:"), w->scale);
    ((void (*)(id, SEL, CGRect))objc_msgSend)(w->layer, sel("setFrame:"), rect);
    msg_vp(w->view, sel("setLayer:"), w->layer);
    msg_vb(w->view, sel("setWantsLayer:"), YES);
    if (w->backdrop_blur && cls("NSGlassEffectView")) {
        /* macOS 26+: this is Apple's public exact Liquid Glass surface. Its
         * contentView contract lets AppKit keep terminal content legible as
         * the compositor-managed material adapts behind it. */
        w->backdrop =
            msg(((id (*)(id, SEL, CGRect))objc_msgSend)(msg(cls("NSGlassEffectView"), sel("alloc")),
                                                        sel("initWithFrame:"), rect),
                sel("autorelease"));
        if (w->backdrop) {
            msg_vu(w->backdrop, sel("setAutoresizingMask:"), NS_VIEW_SIZABLE);
            msg_vi(w->backdrop, sel("setStyle:"), NS_GLASS_REGULAR);
            msg_vd(w->backdrop, sel("setCornerRadius:"), 0.0);
            id tint = ((id (*)(id, SEL, double, double, double, double))objc_msgSend)(
                cls("NSColor"), sel("colorWithSRGBRed:green:blue:alpha:"),
                (double)((w->bg >> 16) & 0xff) / 255.0, (double)((w->bg >> 8) & 0xff) / 255.0,
                (double)(w->bg & 0xff) / 255.0, (double)w->backdrop_alpha / 255.0 * 0.18);
            if (tint) msg_vp(w->backdrop, sel("setTintColor:"), tint);
            msg_vp(w->backdrop, sel("setContentView:"), w->view);
        }
    }
    if (w->backdrop_blur && !w->backdrop && cls("NSVisualEffectView")) {
        /* Older macOS: public semantic whole-window blur. No private CAFilter
         * or direct WindowServer backdrop texture is used. */
        w->backdrop =
            msg(((id (*)(id, SEL, CGRect))objc_msgSend)(
                    msg(cls("NSVisualEffectView"), sel("alloc")), sel("initWithFrame:"), rect),
                sel("autorelease"));
        if (w->backdrop) {
            msg_vu(w->backdrop, sel("setAutoresizingMask:"), NS_VIEW_SIZABLE);
            msg_vi(w->backdrop, sel("setMaterial:"), NS_VISUAL_UNDER_WINDOW);
            msg_vi(w->backdrop, sel("setBlendingMode:"), NS_VISUAL_BEHIND);
            msg_vi(w->backdrop, sel("setState:"), NS_VISUAL_FOLLOWS);
            msg_vp(w->backdrop, sel("addSubview:"), w->view);
        }
    }
    msg_vp(w->win, sel("setContentView:"), w->backdrop ? w->backdrop : w->view);

    msg_v(w->win, sel("center"));
    msg_vp(w->win, sel("makeKeyAndOrderFront:"), NULL);
    msg_vb(w->app, sel("activateIgnoringOtherApps:"), YES);
    msg_v(w->app, sel("finishLaunching"));

    backing_size(w, &w->last_px_w, &w->last_px_h);
    w->last_focus = msg_b(w->win, sel("isKeyWindow")) != 0;
    msg_v(pool, sel("drain"));
    return w;

fail:
    tt_window_close(w);
    msg_v(pool, sel("drain"));
    return NULL;
}

void tt_window_close(tt_window *w) {
    if (!w) return;
    if (w->win) {
        msg_v(w->win, sel("close"));
        msg_v(w->win, sel("release"));
    }
    if (w->layer) msg_v(w->layer, sel("release"));
    if (w->bmp_ctx) CGContextRelease(w->bmp_ctx);
    free(w->bmp_px);
    if (w->mask_ctx) CGContextRelease(w->mask_ctx);
    free(w->mask_px);
    for (int i = 0; i < FACE_COUNT; i++)
        if (w->face[i]) CFRelease(w->face[i]);
    if (w->cache) {
        cache_clear(w);
        free(w->cache);
    }
    free(w->clip);
    free(w);
}

void tt_window_render_config(tt_window *w, tt_render_config *out) {
    memset(out, 0, sizeof *out);
    out->cell_w = w->cell_w;
    out->cell_h = w->cell_h;
    out->pad_left = w->pad_left;
    out->pad_top = w->pad_top;
    out->pad_right = w->pad_right;
    out->pad_bottom = w->pad_bottom;
    out->fg = w->fg;
    out->bg = w->bg;
    memcpy(out->palette, w->palette, sizeof out->palette);
    out->bold_brightens = w->bold_brightens;
    out->status_h = w->status_h;
    out->status_fg = w->status_fg;
    out->status_bg = w->status_bg;
    out->glyph = glyph_lookup;
    out->glyph_user = w;
}

void tt_window_surface(tt_window *w, int *px_w, int *px_h) { backing_size(w, px_w, px_h); }

void tt_window_set_title(tt_window *w, const char *title) {
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    msg_vp(w->win, sel("setTitle:"), nsstr(title));
    msg_v(pool, sel("drain"));
}

/* ---- pasteboard -------------------------------------------------------- */

#define PB_TYPE_STRING CFSTR("public.utf8-plain-text")

void tt_window_set_clipboard(tt_window *w, const char *utf8, size_t len) {
    (void)w;
    if (!utf8 || !len) return;
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    id pb = msg(cls("NSPasteboard"), sel("generalPasteboard"));
    if (pb) {
        ((long (*)(id, SEL))objc_msgSend)(pb, sel("clearContents"));
        ((BOOL (*)(id, SEL, id, CFStringRef))objc_msgSend)(pb, sel("setString:forType:"),
                                                           nsstr(utf8), PB_TYPE_STRING);
    }
    msg_v(pool, sel("drain"));
}

const char *tt_window_clipboard(tt_window *w) {
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    id pb = msg(cls("NSPasteboard"), sel("generalPasteboard"));
    id str =
        pb ? ((id (*)(id, SEL, CFStringRef))objc_msgSend)(pb, sel("stringForType:"), PB_TYPE_STRING)
           : NULL;
    const char *utf8 =
        str ? ((const char *(*)(id, SEL))objc_msgSend)(str, sel("UTF8String")) : NULL;
    free(w->clip);
    w->clip = NULL;
    if (utf8) {
        size_t n = strlen(utf8);
        w->clip = malloc(n + 1);
        if (w->clip) memcpy(w->clip, utf8, n + 1);
    }
    msg_v(pool, sel("drain"));
    return w->clip;
}

/* ---- events ----------------------------------------------------------- */

static unsigned mods_from(unsigned long flags) {
    unsigned m = 0;
    if (flags & NS_MOD_SHIFT) m |= TT_MOD_SHIFT;
    if (flags & NS_MOD_OPTION) m |= TT_MOD_ALT;
    if (flags & NS_MOD_CONTROL) m |= TT_MOD_CTRL;
    if (flags & NS_MOD_COMMAND) m |= TT_MOD_SUPER;
    return m;
}

static tt_key key_from(unsigned short u, unsigned *mods) {
    if (u >= FN_F1 && u <= FN_F12) return (tt_key)(TT_KEY_F1 + (u - FN_F1));
    switch (u) {
    case FN_UP: return TT_KEY_UP;
    case FN_DOWN: return TT_KEY_DOWN;
    case FN_LEFT: return TT_KEY_LEFT;
    case FN_RIGHT: return TT_KEY_RIGHT;
    case FN_INSERT: return TT_KEY_INSERT;
    case FN_DELETE: return TT_KEY_DELETE;
    case FN_HOME: return TT_KEY_HOME;
    case FN_END: return TT_KEY_END;
    case FN_PGUP: return TT_KEY_PAGE_UP;
    case FN_PGDN: return TT_KEY_PAGE_DOWN;
    case 0x0d:
    case 0x03: return TT_KEY_ENTER;
    case 0x09: return TT_KEY_TAB;
    case 0x19: /* Shift-Tab arrives as EM */ *mods |= TT_MOD_SHIFT; return TT_KEY_TAB;
    case 0x7f:
    case 0x08: return TT_KEY_BACKSPACE;
    case 0x1b: return TT_KEY_ESCAPE;
    default: return TT_KEY_TEXT;
    }
}

static void copy_text(id str, char *out, size_t cap, size_t *len) {
    *len = 0;
    if (!str) return;
    const char *utf8 = ((const char *(*)(id, SEL))objc_msgSend)(str, sel("UTF8String"));
    if (!utf8) return;
    size_t n = strlen(utf8);
    if (n >= cap) n = cap - 1;
    memcpy(out, utf8, n);
    out[n] = '\0';
    *len = n;
}

bool tt_window_pump(tt_window *w, tt_win_ev *ev) {
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    bool got = false;
    memset(ev, 0, sizeof *ev);

    for (;;) {
        if (w->closed && !w->close_sent) {
            w->close_sent = true;
            ev->type = TT_WIN_EV_CLOSE;
            got = true;
            break;
        }
        int pw = 0, ph = 0;
        backing_size(w, &pw, &ph);
        if (pw != w->last_px_w || ph != w->last_px_h) {
            w->last_px_w = pw;
            w->last_px_h = ph;
            ((void (*)(id, SEL, CGRect))objc_msgSend)(w->layer, sel("setFrame:"),
                                                      msg_rect(w->view, sel("bounds")));
            ev->type = TT_WIN_EV_RESIZE;
            got = true;
            break;
        }
        bool focus = msg_b(w->win, sel("isKeyWindow")) != 0;
        if (focus != w->last_focus) {
            w->last_focus = focus;
            ev->type = TT_WIN_EV_FOCUS;
            ev->focused = focus;
            got = true;
            break;
        }
        if (!w->closed && !msg_b(w->win, sel("isVisible"))) {
            w->closed = true;
            continue;
        }

        id date = msg(cls("NSDate"), sel("distantPast"));
        id e = ((id (*)(id, SEL, unsigned long, id, CFStringRef, BOOL))objc_msgSend)(
            w->app, sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), NS_EVENT_MASK_ANY, date,
            CFSTR("kCFRunLoopDefaultMode"), YES);
        if (!e) break;

        unsigned long etype = msg_ul(e, sel("type"));
        if ((etype == NS_EVENT_L_MOUSE_DOWN || etype == NS_EVENT_L_MOUSE_DRAGGED ||
             etype == NS_EVENT_L_MOUSE_UP) &&
            msg_b(w->win, sel("isKeyWindow"))) {
            /* Let AppKit see it too: the titlebar, the traffic lights and
             * window dragging all live on the same button. */
            msg_vp(w->app, sel("sendEvent:"), e);
            bool inside = event_point(w, e, &ev->px_x, &ev->px_y);
            if (!inside && etype == NS_EVENT_L_MOUSE_DOWN) continue;
            ev->clicks = (int)msg_ul(e, sel("clickCount"));
            if (ev->clicks < 1) ev->clicks = 1;
            ev->type = etype == NS_EVENT_L_MOUSE_DOWN ? TT_WIN_EV_MOUSE_DOWN
                       : etype == NS_EVENT_L_MOUSE_UP ? TT_WIN_EV_MOUSE_UP
                                                      : TT_WIN_EV_MOUSE_DRAG;
            got = true;
            break;
        }
        if (etype == NS_EVENT_KEY_DOWN && msg_b(w->win, sel("isKeyWindow"))) {
            unsigned long flags = msg_ul(e, sel("modifierFlags"));
            unsigned m = mods_from(flags);
            id bare = msg(e, sel("charactersIgnoringModifiers"));
            unsigned short u = 0;
            if (bare && msg_ul(bare, sel("length")) > 0)
                u = ((unsigned short (*)(id, SEL, unsigned long))objc_msgSend)(
                    bare, sel("characterAtIndex:"), 0);
            if (m & TT_MOD_SUPER) {
                /* Which Command presses are ours is decided by the
                 * platform-free chord table, so this file holds no
                 * second copy of the key map. Everything else goes back
                 * to AppKit, which still owns Cmd-M, Cmd-H and friends. */
                char text[8];
                size_t tlen = 0;
                copy_text(bare, text, sizeof text, &tlen);
                unsigned kmods = m;
                tt_key k = key_from(u, &kmods);
                tt_chord chord = tt_key_chord(k, text, tlen, kmods);
                if (chord == TT_CHORD_NONE) {
                    msg_vp(w->app, sel("sendEvent:"), e);
                    continue;
                }
                ev->type = TT_WIN_EV_CHORD;
                ev->chord = chord;
                ev->mods = kmods;
                got = true;
                break;
            }
            ev->type = TT_WIN_EV_KEY;
            ev->mods = m;
            ev->key = key_from(u, &ev->mods);
            /* Option/Control compose on macOS (Option-a is "å"); for
             * Meta and Ctrl the unmodified character is what the pty
             * encoding needs. */
            id text = (m & (TT_MOD_ALT | TT_MOD_CTRL)) ? bare : msg(e, sel("characters"));
            copy_text(text, ev->text, sizeof ev->text, &ev->text_len);
            got = true;
            break;
        }
        msg_vp(w->app, sel("sendEvent:"), e);
    }

    msg_v(pool, sel("drain"));
    return got;
}

void tt_window_present(tt_window *w, const uint32_t *px, int px_w, int px_h, int y0, int y1) {
    if (y1 <= y0 || px_w < 1 || px_h < 1) return;
    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    if (!w->bmp_ctx || !w->bmp_px || w->bmp_w != px_w || w->bmp_h != px_h) {
        if (w->bmp_ctx) CGContextRelease(w->bmp_ctx);
        free(w->bmp_px);
        w->bmp_px = calloc((size_t)px_w * (size_t)px_h, sizeof *w->bmp_px);
        w->bmp_ctx = NULL;
        w->bmp_src = NULL;
        w->bmp_w = px_w;
        w->bmp_h = px_h;
        if (!w->bmp_px) {
            msg_v(pool, sel("drain"));
            return;
        }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        w->bmp_ctx =
            CGBitmapContextCreate(w->bmp_px, (size_t)px_w, (size_t)px_h, 8, (size_t)px_w * 4, cs,
                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
    }
    /* A new source allocation cannot rely on its dirty band describing the
     * pixels retained from the old source. */
    if (w->bmp_src != px) {
        w->bmp_src = px;
        y0 = 0;
        y1 = px_h;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > px_h) y1 = px_h;
    for (int y = y0; y < y1; y++) {
        const uint32_t *src = px + (size_t)y * (size_t)px_w;
        uint32_t *dst = w->bmp_px + (size_t)y * (size_t)px_w;
        for (int x = 0; x < px_w; x++) {
            uint32_t p = src[x] | 0xff000000u;
            if ((p & 0x00ffffffu) != w->bg || w->backdrop_alpha == 255) {
                dst[x] = p;
                continue;
            }
            unsigned a = w->backdrop_alpha;
            unsigned r = ((p >> 16) & 0xffu) * a / 255;
            unsigned g = ((p >> 8) & 0xffu) * a / 255;
            unsigned b = (p & 0xffu) * a / 255;
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    if (w->bmp_ctx) {
        CGImageRef img = CGBitmapContextCreateImage(w->bmp_ctx);
        if (img) {
            msg_v(cls("CATransaction"), sel("begin"));
            msg_vb(cls("CATransaction"), sel("setDisableActions:"), YES);
            msg_vp(w->layer, sel("setContents:"), img);
            msg_v(cls("CATransaction"), sel("commit"));
            msg_v(cls("CATransaction"), sel("flush"));
            CGImageRelease(img);
        }
    }
    msg_v(pool, sel("drain"));
}
