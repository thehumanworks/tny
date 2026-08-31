# tnytty — configuration

tnytty needs no config file: every key has a default, and supported CLI flags
override the corresponding file values. The file exists so the native window
([ADR 0005](adr/0005-native-renderer-and-macos-window.md)) can be styled
once instead of on every launch.

## Location

```text
$XDG_CONFIG_HOME/tnytty/config      # if XDG_CONFIG_HOME is set
~/.config/tnytty/config             # otherwise
```

A missing file is not an error. An unreadable one, or one with a bad
value, is: `tnytty` prints the path, the line number and what it
expected, and exits 2.

## Format

`key = value`, one per line. `#` starts a comment line. Whitespace around
the key and the value is trimmed; there are no sections, no quoting, and
no continuations.

```ini
# ~/.config/tnytty/config
font = JetBrains Mono
font-size = 14
macos-titlebar = transparent
padding = 10
backdrop-opacity = 92
backdrop-blur = true
```

## Keys

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `font` | any installed family name | *(empty)* | Monospaced face for the window. Empty means Menlo, falling back to the system fixed-pitch UI font. |
| `font-size` | 4 – 288 | `13` | Points. Device pixels are derived from the display's backing scale. |
| `macos-titlebar` | `transparent`, `opaque` | `transparent` | Transparent extends the terminal background under the traffic lights and hides the title; the grid is inset below them so no text is covered. Opaque restores the system titlebar and shows the session title. |
| `padding` | 0 – 256 | `8` | Points of blank margin around the grid. With a transparent titlebar the top margin additionally includes the titlebar height. |

### Window backdrop

The macOS window uses the public, compositor-managed AppKit backdrop
([ADR 0008](adr/0008-public-macos-backdrop.md)). These settings affect cells
painted with the default terminal background; glyphs and explicit ANSI cell
backgrounds stay opaque for legibility.

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `backdrop-opacity` | 0 – 100 | `92` | Opacity percentage of the default terminal background. `100` is fully opaque; lower values reveal more of the backdrop. |
| `backdrop-blur` | `true`, `false` | `true` | Use AppKit's system-managed behind-window blur. `false` keeps the configured translucency but shows the unblurred desktop/window behind it. |

The blur is deliberately a switch, not a numeric radius. AppKit chooses the
material's blur, tint, vibrancy and accessibility fallback; it exposes no public
blur-radius control or WindowServer backdrop texture. On macOS, Reduce
Transparency and the active/inactive window state therefore remain authoritative.

### Colors

Values are `#rrggbb` (the `#` is optional). The defaults are a dark
theme; see [contrast](#contrast) for why these numbers and not xterm's.

| Key | Default | Meaning |
| --- | --- | --- |
| `foreground` | `#d7dae3` | Default text color |
| `background` | `#14161f` | Terminal background; also the window background, which is what makes the transparent titlebar read as one surface |
| `divider` | `#3a4152` | The 1 px rule drawn between split panes ([ADR 0006](adr/0006-split-panes-and-the-layout-tree.md)). Dim by design: it separates panes without competing with their text. Invisible when nothing is split. |
| `palette0` … `palette15` | see the table below | SGR colors 30–37 / 90–97 and the low 16 of the 256-color space. Indices 16–255 are the fixed xterm cube and grayscale ramp and are not configurable. |
| `bold-brightens` | `true` | SGR 1 on an indexed foreground 0–7 also selects the bright entry 8–15. Bold always uses the bold *face* as well; turning this off keeps the weight and drops the color change. |

### Behavior

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `copy-on-select` | `true`, `false` | `true` | Releasing a drag puts the selected text on the system pasteboard. Cmd-C copies regardless. |
| `status-bar` | `true`, `false` | `false` | A one-line bar along the bottom edge that shows transient messages ("Copied 42 characters") for two seconds. Its height is taken out of the grid, so the session gets one row fewer; enable it explicitly when wanted. |

Booleans also accept `yes`/`no` and `1`/`0`.

### Contrast

The stock xterm palette puts SGR 34 at `#0000ee`. On a near-black
background that is unreadable — and a bold blue path segment is exactly
what most shell prompts use. The defaults below are chosen so **every
entry except index 0 clears WCAG AA (4.5:1) against the default
background**, measured, not eyeballed:
`tests/test_config.c` recomputes these ratios and fails the build if a
palette change drops one below the line.

| Key | Color | Role | Contrast on `#14161f` |
| --- | --- | --- | --- |
| `palette0` | `#2a2f3a` | black — a *background* color, exempt | 1.35:1 |
| `palette1` | `#f07178` | red | 6.30:1 |
| `palette2` | `#9ece6a` | green | 9.87:1 |
| `palette3` | `#e0c980` | yellow | 11.02:1 |
| `palette4` | `#7aa2f7` | blue | 7.16:1 |
| `palette5` | `#c792ea` | magenta | 7.50:1 |
| `palette6` | `#56cfd8` | cyan | 9.72:1 |
| `palette7` | `#b9bfca` | white | 9.76:1 |
| `palette8` | `#7a8296` | bright black | 4.69:1 |
| `palette9` | `#ff8b92` | bright red | 8.04:1 |
| `palette10` | `#b4f08a` | bright green | 13.55:1 |
| `palette11` | `#ffe08a` | bright yellow | 13.98:1 |
| `palette12` | `#9fc1ff` | bright blue | 9.92:1 |
| `palette13` | `#e0b0ff` | bright magenta | 10.16:1 |
| `palette14` | `#7fe6ee` | bright cyan | 12.42:1 |
| `palette15` | `#eef1f7` | bright white | 15.94:1 |

Default text (`foreground` on `background`) is 12.91:1 — AAA.

Two attributes deliberately reduce contrast, because reducing it is what
they mean:

- **faint** (SGR 2) blends the foreground 69 % of the way from the
  background, which floors at about 2.5:1 for the dimmest palette entry.
  It stays legible as de-emphasized text and never collapses into the
  background.
- **bold** never *lowers* contrast: with `bold-brightens` on it moves to
  the bright half, and every bright entry is in the table above.

Index 0 is the one entry below the line. It is a background color —
`\033[40m` — and giving it 4.5:1 against the background would make it
useless as one. Nothing draws text in it by default.

Unknown keys and lines without `=` are **warned about on stderr and
skipped**, so a config written for a newer tnytty still starts. A key
that is known but whose value is not is a **clean error**, because the
line was meant for something.

The file is limited to 64 KiB and each line to 511 bytes. Larger input
is a clean error rather than silently ignoring a suffix or later
settings.

## Flags win

Style keys that have a flag on `tnytty gui` override the file for that run
([cli.md](cli.md)):

```sh
tnytty gui --titlebar opaque --font Menlo --font-size 15 --padding 0
```

Colors, backdrop and behavior keys have no flags yet: they are settings you
choose once, not per-run switches.

A bad value on the command line is an error, never a warning.

## Not configurable yet

Cursor style, key bindings, selection colors (the highlight inverts the
cell), the status-bar timeout, the split ratio (always 50/50; dividers
are not draggable yet) and scrollback size are compiled-in defaults in
this phase. Each becomes a key when the behavior behind it exists; new keys
are additive, and removing or renaming one needs an ADR.
