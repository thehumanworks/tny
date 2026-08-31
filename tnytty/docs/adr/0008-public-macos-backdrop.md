# 0008 — The macOS backdrop uses public AppKit compositor materials

Date: 2026-08-30
Status: accepted

## Context

The native window has a transparent titlebar but its framebuffer was fully
opaque. The desired glass backdrop must keep text crisp, respect macOS
accessibility settings, and preserve tnytty's C11 renderer and small binary.

Apple exposes two relevant public surfaces:

- [`NSGlassEffectView`](https://developer.apple.com/documentation/appkit/nsglasseffectview)
  on macOS 26 embeds a custom control in the exact Liquid Glass material.
  Apple's [AppKit design guidance](https://developer.apple.com/videos/play/wwdc2025/310/?time=1050)
  says glass elements float at the top functional layer and should wrap their
  `contentView`; nearby elements belong in `NSGlassEffectContainerView`.
- [`NSVisualEffectView`](https://developer.apple.com/documentation/appkit/nsvisualeffectview)
  is the semantic whole-window backdrop API. Its `behindWindow` blending mode
  samples and blurs the desktop or other windows behind the app, and
  `underWindowBackground` is the documented material for this role.

Neither public API exposes Apple's shader, a WindowServer backdrop texture, or
a numeric blur radius. Metal can sample textures an application owns, but the
public Metal API does not hand an application the compositor's behind-window
surface. Reproducing the exact material with a custom shader would therefore
require screen capture or private APIs, neither of which is acceptable.

## Decision

On macOS 26 and newer, the terminal window uses `NSGlassEffectView` with the
framebuffer view installed through its required `contentView` property,
regular style, zero corner radius and a background-derived tint. This is the
exact public Liquid Glass material, created dynamically through the existing
Objective-C runtime seam so the C11-only architecture does not gain a second
language.

On older macOS releases, it falls back to `NSVisualEffectView` with
`underWindowBackground`, `behindWindow` blending and the system-following
active state.

`backdrop-blur` controls whether that public compositor material is present.
When disabled, the same transparent pixels reveal an unblurred backdrop.
`backdrop-opacity` is an integer from 0 to 100, default 92, applied to pixels
that match the configured default terminal background. Those pixels are
converted to premultiplied alpha in the macOS presentation adapter. Glyphs and
explicit ANSI backgrounds remain opaque, so translucency does not lower text
contrast or leak through application surfaces that intentionally chose a
background color.

Only the dirty presentation band is converted. The VT core, cell rasterizer,
HTTP screen model, selection and other platform windows continue to see plain
RGB cells.

The titlebar remains transparent by default and still reserves the traffic
light inset. The status bar becomes opt-in (`status-bar = false` by default),
returning its row to terminal content.

## Fallbacks

- If `NSGlassEffectView` is unavailable, tnytty uses the documented visual
  effect material. If neither public view can be created, it keeps the
  transparent, unblurred window rather than using a private material.
- `backdrop-opacity = 100` produces the old opaque terminal background.
- System Reduce Transparency and material changes across macOS releases are
  honored by AppKit; tnytty does not try to override them.

## Consequences

- On macOS 26+, this is Apple's public Liquid Glass view. It is not a claim
  that tnytty has access to SwiftUI's shader or a direct Metal equivalent.
- Presentation owns one RGBA buffer and updates only dirty bands before the
  existing `CGImage`/`CALayer` blit. Performance and size changes must be
  measured with the rest of the feature.
