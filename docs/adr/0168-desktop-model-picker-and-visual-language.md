# 0168 — Desktop companion: composer model picker and a flat visual language

Date: 2026-09-23
Status: experimental; extends [0166](0166-slint-desktop-companion.md)

## Context

The Slint desktop companion (ADR 0166) always ran the CLI's resolved default
provider and model. The only way to change either was to leave the GUI. Its
layout marked every region with its own background, rules and outlined
fields. It also had controls whose purpose was not stated, such as a
"Use local tools" button next to "Connect SSH". Users read these as busy
and unclear.

## Decision

**Provider and model picker in the composer.** As in common chat clients, the
picker lives in the prompt box beside Send. It shows `provider · model` and
opens a two-column popover: providers on the left, the selected provider's
models on the right, with a "Default" entry and a free-form "other model ID"
field for gateways whose catalog is missing or incomplete.

- The provider list comes from `tny providers --json`, a local configuration
  check that makes no model request. It is loaded at startup and on Refresh.
  Only `name`, `active` and `healthy` are used. The free-form `hint` can
  contain base URLs and is never displayed. Unhealthy providers are shown as
  "Needs setup" with a tooltip that gives the CLI setup command. They cannot
  be selected.
- A model catalog (`tny --provider P models --json`) is a provider request.
  The GUI makes it only when the picker opens or a provider is chosen, never
  at startup. The catalog is cached per provider per window, and Refresh
  clears the cache. Both catalog shapes are accepted: plain id strings and
  normalized `{id, name, description}` objects. The CLI's `default`
  fallback row counts as "no catalog". Catalog calls never use `--ssh`.
- The chosen provider and model are passed to `ask` as leading
  `--provider P [--model M]`. "Default" omits `--model`. Providers must pass
  the existing flag-safe provider check. Model IDs must not be empty, contain
  control characters or start with `-`. Invalid values are rejected before
  any process starts.
- Opening a saved chat sets the picker to the provider and model that the
  CLI would resume with (the same legacy fallbacks as ADR 0166's resume pin).
  A choice the user makes afterwards is explicit and takes precedence over
  the saved pin. Without a choice, the bridge still pins the saved session's
  provider and model, so a resumed transcript never silently moves to a new
  default. New chats keep the last choice.

**Visual language.** The whole window uses one page tone. Hierarchy comes from
type weight and lighter tones of the same green-grey scale, not from panels,
rules or background changes. Only surfaces that float above the page (the
composer, fields, popovers, tooltips) are elevated, and only by shadow.
Interaction never changes a color: clickable elements get a pointer cursor,
and state such as "current chat" or "selected" is shown by weight and a
check mark. Disabled controls fade to a lighter tone.

**Plain language and tooltips.** Every icon-only or non-obvious control has a
tooltip saying what it does and what it affects. The local/SSH choice is a
labelled radio group, "Where tools run: This computer / SSH host", with an
explanation, replacing the separate "Use local tools" button. The saved-chat
confirmation now asks "Where should tools run when you continue this chat?".
Enter sends and Shift+Enter inserts a new line.

## Consequences

The GUI allowlist grows by `providers --json` and provider-scoped
`models --json`. There is still no general argv gateway, no provider
implementation and no credential handling in the GUI. The picker cannot set
reasoning effort. `tny models` for ACP providers depends on the adapter and
may report no catalog, in which case the picker explains that "Default" or a
typed ID still work.

## Verification

`cd gui && cargo test`: fake-executable tests cover leading flag order, the
explicit choice overriding the saved-session pin without reading it,
pre-spawn rejection of flag-like and invalid values, provider-scoped
non-SSH catalog calls, both catalog shapes, hint removal, and the picker's
view model and saved-choice fallbacks. On a Linux/Hyprland headless output,
synthetic pointer and key events drove a fake `tny` executable. They
confirmed that Enter sends `--provider grok --model grok-4.7 ask …`, that
startup makes no `models` request, that real hover tooltips appear, that
popovers open above the composer, and that the layout holds at 1000×800.
No live provider turn was run.

Footprint (Linux x86-64, `cargo build --release --locked`, `strip
--strip-unneeded` copy): 19,903,904 → 20,703,520 bytes (+799,616, +4.0%).
