#!/usr/bin/env python3
"""Generate the tny static site into site/."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "site"

REPO = "https://github.com/thehumanworks/tny"
INSTALL = "git clone https://github.com/thehumanworks/tny && cd tny && make"
TNYTTY_INSTALL = (
    "git clone https://github.com/thehumanworks/tny && cd tny && make tnytty"
)
VERSION = "0.3.0"
SIZE = "0.68mib"
SITE_BASE = "https://thehumanworks.github.io/tny/"

NAV = (
    ("docs", "docs/index.html", "docs"),
    ("cli", "docs/cli.html", "cli"),
    ("tnytty", "docs/tnytty.html", "tnytty"),
    ("source", REPO, "source"),
)

SIDEBAR = (
    (
        "getting started",
        (
            ("Quick start", "docs/index.html"),
            ("Installation", "docs/install.html"),
            ("Providers", "docs/providers.html"),
        ),
    ),
    (
        "using tny",
        (
            ("tny ask", "docs/ask.html"),
            ("CLI", "docs/cli.html"),
            ("Workflows", "docs/workflows.html"),
            ("Interactive shell", "docs/tui.html"),
            ("Sessions", "docs/sessions.html"),
        ),
    ),
    (
        "configure",
        (
            ("Permissions", "docs/permissions.html"),
            ("Tools, MCP, skills", "docs/tools.html"),
        ),
    ),
    (
        "reference",
        (
            ("Backends", "docs/backends.html"),
            ("Architecture", "docs/architecture.html"),
            ("Size and speed", "docs/size.html"),
        ),
    ),
    (
        "tnytty",
        (
            ("The tiny terminal", "docs/tnytty.html"),
            ("CLI", "docs/tnytty-cli.html"),
            ("Configuration", "docs/tnytty-config.html"),
            ("HTTP API", "docs/tnytty-api.html"),
            ("Architecture", "docs/tnytty-architecture.html"),
        ),
    ),
)


def rel(from_docs: bool, target: str) -> str:
    if target.startswith("http"):
        return target
    if from_docs:
        if target.startswith("docs/"):
            return target[len("docs/") :]
        return "../" + target
    return target


def svg_copy() -> str:
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
        'viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
        'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
        '<rect width="14" height="14" x="8" y="8" rx="2" ry="2"></rect>'
        '<path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"></path>'
        "</svg>"
    )


def svg_slash() -> str:
    return (
        '<svg class="brand-slash" width="16" height="16" viewBox="0 0 16 16" '
        'fill="none" aria-hidden="true">'
        '<path fill-rule="evenodd" clip-rule="evenodd" '
        'd="m4.02 15.4.3-.7 6-14 .29-.68 1.37.59-.3.69-6 14-.29.68z" '
        'fill="var(--logo-divider)"></path>'
        "</svg>"
    )


def header(from_docs: bool, active: str | None) -> str:
    home = rel(from_docs, "index.html")
    items = []
    for key, href, label in NAV:
        url = rel(from_docs, href)
        extra = ' target="_blank" rel="noreferrer"' if href.startswith("http") else ""
        cur = ' aria-current="page"' if key == active else ""
        items.append(f'<a href="{url}"{extra}{cur}>{label}</a>')
    return f"""<header class="site-header">
  <a class="brand" href="{home}" aria-label="tny home">{svg_slash()}<span class="brand-word">tny</span></a>
  <nav class="nav">{"".join(items)}</nav>
  <button type="button" class="theme-cta" data-theme aria-label="Toggle color theme">theme</button>
  <button type="button" class="install-cta" data-site-install aria-label="Copy install command">{svg_copy()}<span data-copy-status>install</span></button>
</header>"""


def sidebar_html(from_docs: bool, current: str) -> str:
    groups = []
    for title, links in SIDEBAR:
        items = []
        for label, href in links:
            url = rel(from_docs, href)
            cur = ' aria-current="page"' if href == current else ""
            items.append(f'<a href="{url}"{cur}>{label}</a>')
        groups.append(
            f'<div class="docs-nav-group"><p>{title}</p><div>{"".join(items)}</div></div>'
        )
    return f'<nav aria-label="Documentation">{"".join(groups)}</nav>'


def toc_html(entries: list[tuple[str, str]]) -> str:
    if not entries:
        return ""
    links = "".join(f'<a href="#{hid}">{label}</a>' for hid, label in entries)
    return f'<aside class="toc"><p>on this page</p>{links}</aside>'


def html_esc(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def cmd(command: str) -> str:
    safe = html_esc(command)
    return (
        f'<div class="cmd"><div class="left"><span class="prompt">$</span>'
        f"<code>{safe}</code></div>"
        f'<button type="button" data-copy="{safe}" aria-label="Copy command">'
        f"{svg_copy()}</button></div>"
    )


# Python < 3.12 forbids backslashes inside f-string expressions; keep the
# escaped example out of the f-string.
STDIN_EXAMPLE = cmd("printf 'summarize src/\\n' | tny ask --stdin")


def note(title: str, body: str) -> str:
    return f'<div class="note"><strong>{title}</strong><p>{body}</p></div>'


def page_shell(
    *,
    title: str,
    description: str,
    from_docs: bool,
    active: str | None,
    canonical: str,
    body: str,
    current_doc: str | None = None,
    toc: list[tuple[str, str]] | None = None,
    extra_head: str = "",
    extra_scripts: str = "",
) -> str:
    prefix = "../" if from_docs else ""

    extra_body = body
    if current_doc:
        side = sidebar_html(True, current_doc)
        extra_body = f"""<div class="docs-shell">
  <div class="docs-mobile">
    <button type="button" id="docs-open" aria-expanded="false" aria-controls="docs-drawer">Browse</button>
  </div>
  <div class="drawer" id="docs-drawer" aria-hidden="true">
    <div class="drawer-backdrop" id="docs-backdrop"></div>
    <div class="drawer-panel">
      <button type="button" id="docs-close">Close</button>
      {side}
    </div>
  </div>
  <main class="docs-layout">
    <aside class="docs-sidebar">{side}</aside>
    {body}
    {toc_html(toc or [])}
  </main>
</div>"""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, interactive-widget=resizes-content">
  <title>{title}</title>
  <meta name="description" content="{description}">
  <meta name="theme-color" content="#ffffff" media="(prefers-color-scheme: light)">
  <meta name="theme-color" content="#000000" media="(prefers-color-scheme: dark)">
  <link rel="canonical" href="https://thehumanworks.github.io/tny/{canonical}">
  <meta property="og:title" content="{title}">
  <meta property="og:description" content="{description}">
  <meta property="og:type" content="website">
  <meta property="og:image" content="https://thehumanworks.github.io/tny/og.svg">
  <meta name="twitter:card" content="summary_large_image">
  <link rel="icon" href="{prefix}favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="{prefix}assets/site.css">
{extra_head}  <script>
    (function () {{
      try {{
        var t = localStorage.getItem("theme");
        if (t === "light" || t === "dark") document.documentElement.classList.add(t);
      }} catch (e) {{}}
    }})();
  </script>
</head>
<body>
{header(from_docs, active)}
{extra_body}
<script src="{prefix}assets/site.js"></script>
{extra_scripts}</body>
</html>
"""


def article(title: str, lede: str, inner: str) -> str:
    return f'<article class="article"><h1>{title}</h1><p class="lede">{lede}</p>{inner}</article>'


def landing() -> str:
    features = [
        (
            "Tiny 0.68mb binary",
            "Nine times smaller than fx on macOS. Designed for instant installation and embedding in resource-constrained environments and agent sandboxes.",
        ),
        (
            "Instant time to prompt",
            'The interactive shell first-paints in <span class="hi">3–4ms</span> and does no backend I/O before accepting input. Host processes are pre-warmed after first paint so Enter is not a spawn.',
        ),
        (
            "Four first-class backends",
            'Cursor via the complete public SDK Bridge v1.0.30, Codex via <code>app-server</code> WebSockets, other agents via <a href="docs/backends.html">ACP</a>, and a native OpenAI-compatible tool loop.',
        ),
        (
            "Minimal memory footprint",
            "About 2.1 MiB RSS for <code>--version</code>. Pack many instances on one machine without dragging a runtime along.",
        ),
        (
            "Shell-like UI and ergonomics",
            "A Unix shell, not an IDE in the terminal. Append-only transcript, a pinned composer, slash commands, and sparing use of paints.",
        ),
        (
            "Context efficient",
            "Minimal system prompt and lazy tool catalogs, so token cost and time-to-first-token stay on the model — not the harness.",
        ),
        (
            "Host processes stay external",
            "<code>cursor-sdk-bridge</code> and <code>codex</code> are spawned or attached, never embedded. The tny binary does not ship Node, Bun, or Rust.",
        ),
        (
            "Model and provider agnostic",
            'Local models, OpenRouter, Groq, Azure, Cursor, Codex subscriptions, or any <a href="docs/backends.html">ACP</a> agent.',
        ),
    ]
    body = f"""<main class="landing">
  <div class="landing-hero">
    <div>
      <p class="tagline">Tiny C11 coding-agent harness.</p>
      <p class="install-line">
        <button type="button" data-copy="{INSTALL}" aria-label="Copy install command">
          <span class="prompt">$ </span><code>{INSTALL}</code>
          <span class="copy-hint" aria-hidden="true"><svg width="14" height="14" viewBox="0 0 16 16" fill="none"><rect x="5.5" y="5.5" width="8" height="8" rx="1.5" stroke="currentColor" stroke-width="1.5"></rect><path d="M10.5 3.5v-1a1 1 0 0 0-1-1h-6a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h1" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"></path></svg></span>
          <span class="copied-label" data-copy-status role="status">copied</span>
        </button>
      </p>
      <p class="meta">
        <span>v{VERSION}</span>
        <span aria-hidden="true"> · </span>
        <span>{SIZE}</span>
        <span aria-hidden="true"> · </span>
        status: <span class="status">experimental</span>
        <span class="info">
          <button type="button" aria-label="Stability note" aria-describedby="version-note">(i)</button>
          <span class="tooltip" role="tooltip" id="version-note">use at your own risk. the binary is small; the protocols still move.</span>
        </span>
      </p>
    </div>
    <div class="term-wrap">
      <div class="term" id="tny-term" role="application" aria-label="tny interactive shell">
        <div class="term-bar">
          <div class="term-dots" aria-hidden="true"><span></span><span></span><span></span></div>
          <div class="term-title">tny.wasm</div>
        </div>
        <div class="term-main term-main--wasm">
          <div class="term-xterm" data-term-xterm></div>
        </div>
        <div class="term-status" data-term-status><span class="status-row"><span class="auto">yolo</span> · openai · wasm</span></div>
      </div>
    </div>
    <div class="prose">
      <p><span class="name">tny</span> is a coding agent harness and CLI written in C11, built to beat <a href="https://fx.sh">fx</a> on size and startup while keeping a Unix-shell UI.</p>
      <p>It focuses on a thin multiplexed frontend over host agents, plus a native OpenAI-compatible loop for BYOK providers. The stripped binary is {SIZE}.</p>
      <p>The terminal on this page is the real <span class="name">tny</span> binary compiled to WebAssembly — the same sources and the same CI test suite as the native CLI. Pass <code>OPENAI_API_KEY</code> (and optionally <code>OPENAI_BASE_URL</code>) in the URL hash or paste them at the prompt. Keys stay in this tab and go only to the provider you set — never to GitHub. Your provider must allow browser (CORS) calls; <code>api.openai.com</code> does not.</p>
      <p>For end users, the form factor aims to be closer to a Unix shell than a heavy "IDE in the terminal" TUI.</p>
      <p>It's open source, model-agnostic, and suitable for local models, subscriptions, and cloud inference.</p>
      <p>The same monorepo ships <a href="docs/tnytty.html">tnytty</a>, a tiny C11 terminal emulator: a headless VT core, a pty host, kitty graphics with bundled <code>icat</code>, and a REST API so every session is scriptable and shareable.</p>
    </div>
    <button type="button" class="scroll-hint" data-scroll="features" aria-label="Scroll to features">***</button>
  </div>
  <section class="features" id="features">
    {"".join(f"<div><h2>{h}</h2><p>{p}</p></div>" for h, p in features)}
  </section>
</main>"""
    return page_shell(
        title="tny — Tiny C11 coding-agent harness",
        description="Tiny, open, native coding-agent harness. A C11 CLI and TUI that drives Cursor, Codex, ACP, and OpenAI-compatible providers.",
        from_docs=False,
        active=None,
        canonical="",
        body=body,
        extra_head=(
            '  <meta name="referrer" content="no-referrer">\n'
            '  <link rel="stylesheet" href="assets/vendor/xterm.css">\n'
            '  <script src="assets/term-core.js"></script>\n'
            "  <script>\n"
            "    window.__tnyBoot = window.tnyTermCore.takeSecretsFromLocation(location, history);\n"
            "  </script>\n"
        ),
        extra_scripts=(
            '<script src="assets/vendor/xterm.js"></script>\n'
            '<script src="assets/term-wasm.js"></script>\n'
        ),
    )


def docs_quick() -> str:
    inner = f"""
<h2 id="install">Install and build</h2>
{cmd(INSTALL)}
<p>A C11 compiler and <code>make</code> are enough. Vendored libraries (yyjson, picohttpparser, wslay) ship in the repo — nothing is downloaded at build time. The installer recipe is in <a href="install.html">Installation</a> if you want <code>~/.local/bin</code>.</p>
{cmd("tny doctor")}
<p><code>doctor</code> reports OS, libc, and which optional host binaries are missing. It never needs a network key.</p>
<h2 id="first-request">Run your first request</h2>
<p>Start tny from the project you want to work on. The launch directory becomes the primary workspace:</p>
{cmd("cd path/to/project")}
{cmd("tny")}
<p>Type a request that names real files or commands, then press enter. The model's reply streams into the transcript. Tool calls appear as they run. Escape or ctrl-c interrupts the turn; ctrl-o opens the full transcript.</p>
<p>For scripts and CI, skip the shell:</p>
{cmd('tny ask --json "list the public CLI"')}
<h2 id="providers">Pick a provider</h2>
<p>tny is a frontend. The default provider is the last one you used, then whatever credentials are already on the machine — an OpenAI-compatible key, a Codex login, or <code>CURSOR_API_KEY</code>. Override it:</p>
{cmd('tny --provider cursor ask "explain this repo"')}
{cmd("tny --provider codex")}
{cmd("tny --provider acp --agent gemini -- --acp")}
<p>See <a href="providers.html">Providers</a> for env vars and host binaries.</p>
<h2 id="permissions">What tny asks before it acts</h2>
<p>tny starts in <code>yolo</code> permission mode. Consenting to run an agent in a workspace is the approval. Host providers (Cursor, Codex, ACP) run their own loops; tny does not pretend to gate them.</p>
<p><code>ask</code> and <code>auto</code> are explicit opt-ins on the native OpenAI-compatible loop. Listing, globbing, and reading files inside the workspace never need approval. Writes, shell, and paths outside the workspace do — when you opt in.</p>
<p>Switch modes with <code>/permissions</code> or <code>--permission-mode</code>. Details in <a href="permissions.html">Permissions</a>.</p>
{note("Approve deliberately", "yolo disables tny permission checks and the command sandbox for that process. Use it in a workspace you are willing to lose. Host sandboxes, if any, still apply.")}
<h2 id="things">Things to know</h2>
<p>In an interactive session, type <code>/</code> for commands, <code>@</code> for files, and <code>$</code> for skills.</p>
<table>
  <thead><tr><th>Need</th><th>Use</th></tr></thead>
  <tbody>
    <tr><td>Open commands</td><td>Type <code>/</code></td></tr>
    <tr><td>Find a file</td><td>Type <code>@</code></td></tr>
    <tr><td>Find a skill</td><td>Type <code>$</code></td></tr>
    <tr><td>Inspect status</td><td><code>/status</code></td></tr>
    <tr><td>Choose a model</td><td><code>/model</code></td></tr>
    <tr><td>Change permissions</td><td><code>/permissions</code></td></tr>
    <tr><td>New session</td><td><code>/new</code></td></tr>
    <tr><td>One-shot from a script</td><td><code>tny ask --json "…"</code></td></tr>
    <tr><td>A scriptable terminal</td><td><a href="tnytty.html">tnytty</a></td></tr>
  </tbody>
</table>
"""
    return page_shell(
        title="Quick start — tny",
        description="Install tny, run a first request, and learn the commands worth knowing.",
        from_docs=True,
        active="docs",
        canonical="docs/",
        current_doc="docs/index.html",
        toc=[
            ("install", "Install and build"),
            ("first-request", "First request"),
            ("providers", "Providers"),
            ("permissions", "Permissions"),
            ("things", "Things to know"),
        ],
        body=article(
            "Quick start",
            "Build tny, run a first request, and learn the commands worth knowing.",
            inner,
        ),
    )


def docs_install() -> str:
    inner = f"""
<h2 id="from-source">From source</h2>
<p>tny is C11. There is no bundled Node, no package registry, and no download at compile time.</p>
{cmd(INSTALL)}
<p>Requirements: a C11 compiler (<code>cc</code>) and GNU or BSD <code>make</code>. Python 3 is only needed for the integration fixtures.</p>
{cmd("make            # release build → build/tny (stripped)")}
{cmd("make test       # unit + fixture-driven integration")}
{cmd("make install    # copy build/tny to ~/.local/bin")}
<p><code>make install</code> uses <code>PREFIX=$HOME/.local</code> by default. Add <code>~/.local/bin</code> to <code>PATH</code> if it is not already there.</p>
<h2 id="script">setup.sh</h2>
<p>The same recipe as a one-liner. Read it before piping to a shell.</p>
{cmd("curl -fsSL https://thehumanworks.github.io/tny/setup.sh | bash")}
<h2 id="ci">CI binaries</h2>
<p>Every pull request builds stripped artifacts on Linux x86_64 and aarch64 (glibc and musl static), Darwin arm64 (Apple Silicon / Metal — not Intel x86), and Windows x86_64 (MSYS2). Download them from the <code>ci</code> workflow run.</p>
<h2 id="size">What you should see</h2>
<p>A stripped Linux, macOS arm64, or Windows binary well under 2 MiB. Current measured size is {SIZE} on macOS arm64. <code>tny --version</code> and <code>tny ask --help</code> should return in a couple of milliseconds.</p>
{cmd("tny --version")}
{cmd("tny doctor --json")}
<h2 id="hosts">Optional host binaries</h2>
<p>The tny binary is the harness. Host agents stay on <code>PATH</code>:</p>
<table>
  <thead><tr><th>Provider</th><th>Host</th></tr></thead>
  <tbody>
    <tr><td><code>cursor</code></td><td><code>cursor-sdk-bridge</code> (never linked into tny)</td></tr>
    <tr><td><code>codex</code></td><td><code>codex</code> CLI / <code>codex app-server</code></td></tr>
    <tr><td><code>acp</code></td><td>whatever you pass to <code>--agent</code></td></tr>
    <tr><td><code>openai</code></td><td>none — tny owns the loop</td></tr>
  </tbody>
</table>
<p><code>tny doctor</code> tells you which of those are missing. Missing hosts are not a failed install.</p>
<h2 id="tnytty">tnytty</h2>
<p>The same clone builds the tiny terminal. It is a sibling app, not part of the harness binary:</p>
{cmd("make tnytty")}
<p>Equivalent: <code>make -C tnytty</code>. The stripped binary lands at <code>tnytty/build/tnytty</code>. Details in <a href="tnytty.html">tnytty</a>.</p>
"""
    return page_shell(
        title="Installation — tny",
        description="Build tny from source and install the stripped binary.",
        from_docs=True,
        active="docs",
        canonical="docs/install.html",
        current_doc="docs/install.html",
        toc=[
            ("from-source", "From source"),
            ("script", "setup.sh"),
            ("ci", "CI binaries"),
            ("size", "What you should see"),
            ("hosts", "Optional hosts"),
            ("tnytty", "tnytty"),
        ],
        body=article(
            "Installation", "Clone, make, strip. Host agents stay on PATH.", inner
        ),
    )


def docs_providers() -> str:
    inner = f"""
<h2 id="order">How a provider is chosen</h2>
<p>Leading <code>--provider</code> (alias <code>--backend</code>) wins. Otherwise tny uses, in order:</p>
<ol>
  <li>the provider last used, recorded in <code>~/.tny/settings.json</code></li>
  <li><code>openai</code> if <code>OPENAI_BASE_URL</code> or <code>OPENAI_API_KEY</code> is set</li>
  <li><code>codex</code> if a Codex login exists (<code>~/.codex/auth.json</code>)</li>
  <li><code>cursor</code> if <code>CURSOR_API_KEY</code> is set</li>
  <li><code>openai</code>, whose connect error explains how to configure a key</li>
</ol>
<h2 id="openai">openai</h2>
<p>Native loop. tny owns tools, MCP, skills, permissions, sessions, and <code>tny acp</code>.</p>
{cmd("export OPENAI_API_KEY=…")}
{cmd("export OPENAI_BASE_URL=https://openrouter.ai/api/v1   # optional")}
{cmd("tny --provider openai --model anthropic/claude-sonnet-4.6")}
<p>The default wire is the Responses API (<code>POST /v1/responses</code>, typed SSE) — what current OpenAI models require for function tools + reasoning effort. Providers that only speak legacy Chat Completions still work: set <code>wire_api: "chat"</code> in the provider profile, <code>OPENAI_WIRE_API=chat</code>, or pass <code>--wire-api chat</code>. OpenAI, OpenRouter, Groq, Together, DeepSeek, ollama, llama.cpp, vLLM, and Azure (custom auth header) all fit one of the two wires.</p>
<h2 id="cursor">cursor</h2>
<p>Spawns <code>cursor-sdk-bridge</code> v1.0.30 and implements its complete public Connect <code>sdk.v1</code> HTTP/1.1 contract: local/cloud agents, durable runs, images, modes, MCP/subagents, built-in tool allow/deny, artifacts, usage, and authenticated custom-tool/store callbacks. This is Cursor's supported headless loop, not <code>agent acp</code> or the rejected private <code>agent.v1</code> HTTP/2 wire.</p>
{cmd("export CURSOR_API_KEY=…")}
{cmd("tny --provider cursor")}
{cmd("tny cursor models")}
<p>Configure trusted protojson under <code>settings.cursor</code>: runtime, state/store, Agent/Send options, sandbox/review, MCP, subagents, and tool selection. Registered libtny custom tools can execute through the local callback boundary; Cursor still owns built-in tools and their permission policy. Resolve the bridge from <code>CURSOR_SDK_BRIDGE_BIN</code> or <code>PATH</code>. The bridge is 23–43 MiB of Bun — that weight is why it is never linked into tny.</p>
<p><code>tny cursor</code> exposes catalog, agent/run lifecycle, messages, artifacts/download, usage, and a checked raw 27-route RPC. In wasm, conversations report <code>cursor: conversational sdk.v1 bridge is unavailable in WebAssembly</code> and management reports <code>cursor: sdk.v1 management is unavailable in WebAssembly</code> before bridge work.</p>
<h2 id="codex">codex</h2>
<p>Attaches to a live <code>codex app-server</code> or spawns one on an ephemeral loopback port. Subscriptions need no API key; Codex's own login is enough.</p>
{cmd("tny --provider codex")}
{cmd("tny --provider codex --codex-ws ws://127.0.0.1:4500")}
<p>Without <code>--codex-ws</code>, tny tries <code>TNY_CODEX_WS</code>, then a loopback host registered by a running tny TUI, then a spawn.</p>
<h2 id="acp">acp</h2>
<p>Drive any ACP agent as a client, or serve tny's native loop to an editor.</p>
{cmd("tny --provider acp --agent gemini -- --acp")}
{cmd("tny acp")}
<p><code>tny acp</code> is the server. It exposes only the native OpenAI-compatible loop. Cursor and Codex already have their own IDE surfaces.</p>
"""
    return page_shell(
        title="Providers — tny",
        description="Choose Cursor, Codex, ACP, or an OpenAI-compatible endpoint.",
        from_docs=True,
        active="docs",
        canonical="docs/providers.html",
        current_doc="docs/providers.html",
        toc=[
            ("order", "Selection order"),
            ("openai", "openai"),
            ("cursor", "cursor"),
            ("codex", "codex"),
            ("acp", "acp"),
        ],
        body=article(
            "Providers",
            "Four backends, one event loop. Host processes stay external.",
            inner,
        ),
    )


def docs_ask() -> str:
    inner = f"""
<h2 id="shape">Shape</h2>
<p>One turn, then exit. Stdout is assistant Markdown. Stderr is progress, tool lines, and diagnostics. Interactive prompts are never the only path.</p>
{cmd('tny ask "summarize this repository"')}
{STDIN_EXAMPLE}
{cmd('tny ask --json --no-save "list the public CLI"')}
{cmd('tny ask --resume last "now add tests"')}
<h2 id="exit">Exit codes</h2>
<table>
  <thead><tr><th>Code</th><th>Meaning</th></tr></thead>
  <tbody>
    <tr><td>0</td><td>Finished</td></tr>
    <tr><td>1</td><td>Startup or config</td></tr>
    <tr><td>2</td><td>Run failed</td></tr>
    <tr><td>130</td><td>Interrupted</td></tr>
  </tbody>
</table>
<h2 id="json">JSON mode</h2>
<p><code>--json</code> writes one object to stdout. Field names are stable:</p>
<pre><code>{{
  "output": "…",
  "exit_code": 0,
  "provider": "openai",
  "model": "provider/model",
  "session_id": "…",
  "steps": 1,
  "tool_calls": [{{"name": "read_file", "status": "success"}}]
}}</code></pre>
<h2 id="ci">Scripts and CI</h2>
<p><code>tny ask</code> never blocks on an approval. Unresolved permissions fail the run unless you pass <code>--auto</code> (native loop) or stay on the default <code>--yolo</code>. Host providers must be pre-authorized or they fail closed.</p>
<p>When the prompt is piped on stdin, connect overlaps the read so the provider is already warming while the prompt arrives.</p>
{note("Images", "The native loop and Cursor v1.0.30 accept <code>--image PATH</code> (repeatable, max 16; 8 MiB each) after magic-byte MIME validation. A 17th flag exits before files or backend connection. Cursor sends base64 <code>SdkImageData</code>. Codex still ignores image input. libtny ABI 1 does not expose image attachments yet.")}
"""
    return page_shell(
        title="tny ask — tny",
        description="One-shot requests for scripts and CI, with Markdown or JSON.",
        from_docs=True,
        active="docs",
        canonical="docs/ask.html",
        current_doc="docs/ask.html",
        toc=[
            ("shape", "Shape"),
            ("exit", "Exit codes"),
            ("json", "JSON mode"),
            ("ci", "Scripts and CI"),
        ],
        body=article(
            "tny ask",
            "One turn, then exit. Built for scripts, agents, and CI.",
            inner,
        ),
    )


def docs_cli() -> str:
    inner = f"""
<h2 id="tree">Command tree</h2>
<pre><code>tny                         # interactive TUI, fresh session
tny ask [prompt]            # one turn, then exit
tny resume [last|&lt;id&gt;]      # interactive resume
tny acp                     # ACP server (native loop only)
tny sessions
tny session last|&lt;id&gt;
tny providers
tny tasks
tny task show NAME          # inspect one resolved preset
tny models
tny cursor COMMAND          # Cursor v1.0.30 management/raw RPC
tny permissions
tny workspace list|add|remove|clear
tny status
tny doctor
tny usage
tny login | logout | setup</code></pre>
<h2 id="globals">Global flags</h2>
<p>Global flags are leading — they come before the command.</p>
<pre><code>tny --provider cursor|codex|acp|openai [command]
tny --cwd DIR
tny --model ID
tny --task NAME             # runtime preset: review|optimizer|document|retro
tny --add-dir DIR           # repeatable, process-only
tny --permission-mode ask|auto|yolo   # default: yolo
tny --json                  # where listed
tny -r                      # session picker (TUI)
tny -c                      # resume last for this workspace</code></pre>
<h2 id="task-presets">Runtime task presets</h2>
<p><code>--task NAME</code> selects one of the built-ins <code>review</code>,
<code>optimizer</code>, <code>document</code>, or <code>retro</code>, or a custom
Markdown file at <code>.tny/tasks/NAME.md</code> (project) or
<code>~/.tny/tasks/NAME.md</code> (user). Project definitions win. Files may
use only <code>name:</code> and <code>description:</code> frontmatter and a
non-empty UTF-8 body no larger than 256 KiB. SSH sessions use built-ins only.
The selected name/source/digest is recorded with the session; resuming restores
it and rejects a mismatched explicit selector. <code>--agent CMD</code> remains
the ACP executable/WebSocket option.</p>
<pre><code>cat &gt; .tny/tasks/release-review.md &lt;&lt;'TASK'
---
name: release-review
description: Review release-readiness risks
---
Inspect correctness, compatibility, rollback, and test coverage.
TASK
tny --task release-review ask "Review this release"</code></pre>
<h2 id="cursor-management">Cursor management</h2>
<p><code>tny cursor</code> starts a short-lived v1.0.30 bridge and exposes the complete public management surface. Create/resume/send preserve trusted <code>settings.cursor</code> local/cloud options. Downloads stream decoded artifact bytes with a strict 8 MiB cap; delete requires <code>--yes</code>.</p>
<pre><code>tny cursor ping | version | me | models | repositories
tny cursor create [NAME]
tny cursor resume|reload|close AGENT_ID
tny cursor send AGENT_ID MESSAGE
tny cursor wait|run|conversation RUN_ID
tny cursor runs|agent|messages|artifacts AGENT_ID
tny cursor observe RUN_ID [AFTER_OFFSET]
tny cursor cancel RUN_ID [AGENT_ID]
tny cursor agents
tny cursor archive|unarchive AGENT_ID
tny cursor delete AGENT_ID --yes
tny cursor download AGENT_ID PATH
tny cursor usage AGENT_ID [RUN_ID]
tny cursor rpc SERVICE METHOD [JSON|-] [--yes]</code></pre>
<p>Raw RPC accepts only the 27 client-to-bridge routes and one bounded UTF-8 JSON object from an argument or stdin. Unary JSON and stream frames are preserved. Pipe secret-bearing requests so they do not enter shell history. This command and Cursor conversations return a clean unsupported error in wasm.</p>
<h2 id="help">Help shape</h2>
<p>Every subcommand has <code>--help</code> with copy-paste examples. Missing required values print the error, then a correct example, then exit 1. No timed prompts.</p>
{cmd("tny ask --help")}
<h2 id="provider-flags">Provider flags</h2>
<table>
  <thead><tr><th>Provider</th><th>Flags / env</th></tr></thead>
  <tbody>
    <tr><td>cursor</td><td><code>--bridge-bin</code>, <code>CURSOR_SDK_BRIDGE_BIN</code>, <code>CURSOR_API_KEY</code></td></tr>
    <tr><td>codex</td><td><code>--codex-ws</code>, <code>--codex-bin</code>, <code>--ws-token-file</code>, <code>TNY_CODEX_WS</code>, <code>CODEX_REMOTE_TOKEN</code></td></tr>
    <tr><td>acp</td><td><code>--agent CMD</code> plus extra args after <code>--</code></td></tr>
    <tr><td>openai</td><td><code>--base-url</code>, <code>--api-key-env</code>, <code>OPENAI_BASE_URL</code>, <code>OPENAI_API_KEY</code></td></tr>
  </tbody>
</table>
<h2 id="json-cmds">JSON where it matters</h2>
<p><code>--json</code> is accepted on <code>ask</code>, <code>status</code>, <code>doctor</code>, <code>permissions</code>, <code>models</code>, <code>tasks</code>, <code>task show</code>, <code>session</code>, <code>sessions</code>, <code>workspace</code>, and <code>usage</code>.</p>
<h2 id="examples">Examples</h2>
{cmd("tny")}
{cmd('tny ask "explain src/main.c"')}
{cmd('tny --task review ask "inspect the current diff"')}
{cmd("tny task show review")}
{cmd('tny ask --json --provider openai "list exported symbols"')}
{cmd('tny --provider cursor ask --model composer-2 "fix the leak"')}
{cmd("tny --provider acp --agent gemini -- --acp")}
"""
    return page_shell(
        title="CLI — tny",
        description="Command tree, leading flags, and agent-friendly output.",
        from_docs=True,
        active="cli",
        canonical="docs/cli.html",
        current_doc="docs/cli.html",
        toc=[
            ("tree", "Command tree"),
            ("globals", "Global flags"),
            ("cursor-management", "Cursor management"),
            ("help", "Help"),
            ("provider-flags", "Provider flags"),
            ("json-cmds", "JSON"),
            ("examples", "Examples"),
        ],
        body=article(
            "CLI",
            "Every input is a flag or stdin. Designed so humans and coding agents can run it without menus.",
            inner,
        ),
    )


def docs_workflows() -> str:
    inner = f"""
<h2 id="shell">Bash and Zsh</h2>
<p><code>make install</code>, Nix, and release archives install a sourceable library at <code>&lt;prefix&gt;/share/tny/tny-workflows.sh</code>.</p>
{cmd('. "$HOME/.local/share/tny/tny-workflows.sh"')}
<pre><code>tny_workflow_begin
trap 'tny_workflow_cleanup' EXIT

tny_task architecture --task review --provider codex -- "Audit the architecture"
tny_task tests --task review --provider cursor -- "Audit the tests"
tny_task implement \\
  --after architecture --after tests --no-context --provider codex -- \\
  "Implement from the architecture report after both reviews finish"

tny_workflow_run --jobs 2
tny_result implement</code></pre>
<p>The two root tasks run together. <code>implement</code> starts only after both succeed, receives the architecture output, and treats tests as ordering-only. <code>--no-context</code> applies to the immediately preceding <code>--after</code> edge.</p>
<h2 id="semantics">Execution contract</h2>
<table>
  <thead><tr><th>Rule</th><th>Behavior</th></tr></thead>
  <tbody>
    <tr><td>Preflight</td><td>Undefined dependencies and cycles fail before an agent starts.</td></tr>
    <tr><td>Parallelism</td><td>Ready tasks run up to <code>--jobs</code>; default four.</td></tr>
    <tr><td>Failure</td><td>A failure blocks descendants only. Independent branches finish.</td></tr>
    <tr><td>Context</td><td>Successful direct-dependency outputs are appended in declaration order.</td></tr>
    <tr><td>Ordering only</td><td><code>--after NAME --no-context</code> makes that edge wait without appending output.</td></tr>
    <tr><td>Storage</td><td>Shell tasks are ephemeral by default; captured streams live under <code>TNY_WORKFLOW_DIR</code>.</td></tr>
  </tbody>
</table>
<h2 id="task-options">Task options</h2>
<p><code>tny_task</code> accepts repeated <code>--after</code>, plus <code>--task</code>, <code>--provider</code>, <code>--model</code>, <code>--effort</code>, <code>--cwd</code>, <code>--system-prompt</code>, <code>--permission-mode</code>, <code>--max-steps</code>, <code>--ssh</code>, <code>--ssh-cwd</code>, <code>--agent</code>, <code>--fast</code>, <code>--persist</code>, <code>--stdin</code>, and <code>--no-context</code>. Arguments are passed directly; the helper never uses <code>eval</code>.</p>
<p>Each task starts in its own process group using <code>setsid</code>, or Perl with <code>POSIX::setsid</code> on macOS, so cancellation can terminate descendants and escalate to <code>KILL</code> after a bounded grace period.</p>
<p>Inspect with <code>tny_status NAME</code>, <code>tny_result NAME</code>, <code>tny_result_path NAME</code>, <code>tny_stderr NAME</code>, and <code>tny_workflow_report</code>.</p>
<h2 id="task-types">Runtime task presets</h2>
<p>A task preset is reusable runtime configuration: system-level instructions that describe how an agent should approach a workflow node, while the task prompt remains the concrete job. The workflow helper passes <code>--task NAME</code> directly to <code>tny</code>, so the CLI, TUI, SDKs, and shell use the same definitions. Built-ins are <code>review</code>, <code>optimizer</code>, <code>document</code>, and <code>retro</code>.</p>
<pre><code>tny_task review-change --task review -- "Review the current diff"
tny_task optimize --task optimizer --after review-change -- "Optimize accepted findings"
tny_task docs --task document --after optimize -- "Document final behavior"
tny_task retro --task retro --after docs -- "Capture durable lessons"</code></pre>
<p>Create a workflow-local type without editing the library:</p>
<pre><code>tny_task_type security-review --stdin &lt;&lt;'TASK'
Act as a security reviewer. Inspect trust boundaries, auth, secrets, and input handling.
TASK

tny_task audit --task security-review -- "Audit this change"
tny_task_types</code></pre>
<p>Custom workflow types remain available for compatibility. They are stored under <code>$TNY_WORKFLOW_DIR/task-types/</code> and exposed to each child through an explicit <code>TNY_WORKFLOW_TASK_DIR</code>; bodies are resolved by <code>tny</code>, never composed by the shell. The launcher sets an explicit empty value when no workflow-local override applies, preventing ambient path leakage. For persistent cross-surface presets, use <code>.tny/tasks/NAME.md</code> or <code>~/.tny/tasks/NAME.md</code>. See issue <a href="https://github.com/thehumanworks/tny/issues/81">#81</a>.</p>
<h2 id="sdks">Python and TypeScript</h2>
<p>Both native SDKs expose <code>Workflow</code> with the same validated graph, bounded concurrency, direct-output fan-in, blocked-descendant behavior, and result ordering. Each active task owns a separate native runtime, preserving libtny's owner-thread rules.</p>
<pre><code># Python
workflow = tny.Workflow(config, max_concurrency=2)
workflow.task("inspect", "Inspect")
workflow.task("tests", "Test")
workflow.task("implement", "Implement", depends_on=(
    "inspect", tny.WorkflowDependency("tests", include_output=False)
))
result = await workflow.run_async()</code></pre>
<pre><code>// TypeScript
const workflow = new Workflow({{ runtime, maxConcurrency: 2 }});
workflow.task("inspect", "Inspect");
workflow.task("tests", "Test");
workflow.task("implement", "Implement", {{
  dependsOn: ["inspect", {{ name: "tests", includeOutput: false }}],
}});
const result = await workflow.run();</code></pre>
<p>Task failures remain values so independent work can finish. Call <code>raise_for_failure()</code> in Python or <code>raiseForFailure()</code> in TypeScript when the aggregate should throw.</p>
<h2 id="safety">Safety and limits</h2>
<p>Dependency output is untrusted model text. The envelope labels it as context but cannot eliminate prompt injection. Keep permissions least-privileged and use separate worktrees for parallel agents that write files.</p>
<p>Direct dependency output is bounded to 1 MiB by default. Runs have no implicit retry, cache, distributed queue, or resume policy. The shell reaches every CLI provider; native SDK workflows can select OpenAI-compatible or Cursor sdk.v1 libtny runtimes. Cursor requires explicit state directory, API key, and model configuration plus an external bridge.</p>
<p><a href="https://github.com/thehumanworks/tny/blob/main/docs/workflows.md">Read the complete workflow reference</a>.</p>
"""
    return page_shell(
        title="Workflows — tny",
        description="Dependency chains and bounded parallel agents from Bash, Zsh, Python, and TypeScript.",
        from_docs=True,
        active="docs",
        canonical="docs/workflows.html",
        current_doc="docs/workflows.html",
        toc=[
            ("shell", "Bash and Zsh"),
            ("semantics", "Semantics"),
            ("task-options", "Task options"),
            ("task-types", "Task types"),
            ("sdks", "SDKs"),
            ("safety", "Safety"),
        ],
        body=article(
            "Workflows",
            "Validated dependency chains, bounded fan-out, and ordered fan-in.",
            inner,
        ),
    )


def docs_tui() -> str:
    inner = """
<h2 id="layout">Layout</h2>
<p>A Unix shell, not an IDE. No ncurses, no mouse-required panes.</p>
<pre><code>[transcript: user / assistant / tools / approvals]
[status: backend  model  perm  session  cwd]
&gt; composer</code></pre>
<p>The transcript is append-only with scrollback. Markdown-ish: headings, lists, fenced code, diffs as plain text with +/- coloring. The composer supports multiline via Shift-Enter, Alt-Enter, or <code>\\</code> then Enter.</p>
<h2 id="input">Input</h2>
<table>
  <thead><tr><th>Input</th><th>Action</th></tr></thead>
  <tbody>
    <tr><td>text</td><td>user prompt</td></tr>
    <tr><td><code>/</code> at start</td><td>command palette</td></tr>
    <tr><td><code>@</code></td><td>workspace file picker</td></tr>
    <tr><td><code>$</code></td><td>skill picker</td></tr>
    <tr><td>Up / Down</td><td>prompt history at the draft edge</td></tr>
    <tr><td>Esc or Ctrl-C</td><td>interrupt; second Ctrl-C exits if idle</td></tr>
    <tr><td>Ctrl-O</td><td>full transcript</td></tr>
    <tr><td>Ctrl-X</td><td>subagent manager (native loop)</td></tr>
  </tbody>
</table>
<p>Menus are transient overlays. They draw in the bottom block, Escape hides them, and the next submit clears them — they never enter the scrollback. <code>/</code> <code>@</code> <code>$</code> popovers disable while an approval is focused so paths like <code>/tmp/x</code> stay literal.</p>
<h2 id="slash">Slash commands</h2>
<p>Sessions: <code>/help</code> <code>/clear</code> <code>/new</code> <code>/reset</code> <code>/resume</code> <code>/continue</code> <code>/rename</code> <code>/compact</code> <code>/quit</code></p>
<p>Runtime: <code>/models</code> <code>/model</code> <code>/permissions</code> <code>/sandbox</code> <code>/backend</code> <code>/task</code> <code>/status</code> <code>/usage</code></p>
<p>Tools: <code>/mcp</code> <code>/skills</code> <code>/workspace</code> <code>/image</code> <code>/ssh</code> <code>/undo</code> <code>/copy</code> <code>/trace</code></p>
<p>Auth: <code>/login</code> <code>/logout</code> <code>/setup</code> — dispatched to the active backend. Backend-specific commands degrade to "not available" instead of crashing.</p>
<h2 id="tasks">Task presets</h2>
<p><code>/task</code> lists runtime-owned presets; <code>/task NAME</code> selects one before the first turn and <code>/task clear</code> removes it. The status row shows the active task. Resuming restores the exact saved task snapshot, while changing a task after a turn requires <code>/new</code>.</p>
<p>SSH task discovery is builtin-only. To prevent local project instructions from crossing into a remote workspace, <code>/ssh</code> refuses to attach while a user, project, or workflow task is selected; clear it first, attach, then select a builtin.</p>
<h2 id="startup">Startup</h2>
<p>First paint never waits on a backend. After the banner, the TUI pre-warms the selected provider's host on a background thread so the first prompt adopts a live connection. Failures stay silent and resurface on the ordinary lazy path. One-shot CLI commands do not pre-warm.</p>
<h2 id="browser">Browser demo</h2>
<p>The terminal on the landing page is the real tny binary compiled to WebAssembly, running inside xterm.js. On a phone it fits the viewport so lines wrap instead of clipping; the on-screen keyboard shrinks the pane via the visual viewport. Pass <code>OPENAI_API_KEY</code> and optionally <code>OPENAI_BASE_URL</code> in the URL hash (<code>#OPENAI_API_KEY=…</code>) or paste them at the pre-launch prompt. Keys are sanitized at intake and stay in this tab. Workspace tools that need a host binary return a clean error; the provider must allow CORS — <code>api.openai.com</code> does not.</p>
"""
    return page_shell(
        title="Interactive shell — tny",
        description="Transcript, composer, slash commands, and keys.",
        from_docs=True,
        active="docs",
        canonical="docs/tui.html",
        current_doc="docs/tui.html",
        toc=[
            ("layout", "Layout"),
            ("input", "Input"),
            ("slash", "Slash commands"),
            ("tasks", "Task presets"),
            ("startup", "Startup"),
            ("browser", "Browser demo"),
        ],
        body=article(
            "Interactive shell",
            "Streaming transcript, a pinned composer, a one-line status footer.",
            inner,
        ),
    )


def docs_sessions() -> str:
    inner = f"""
<h2 id="id">Identity</h2>
<p>Session ids are generated by tny, opaque, and unique per workspace. The list is workspace-scoped by default (<code>--all</code> for every workspace).</p>
{cmd("tny -r")}
{cmd("tny resume last")}
{cmd("tny resume <id>")}
{cmd('tny ask --resume last "continue"')}
<h2 id="disk">On disk</h2>
<pre><code>~/.tny/sessions/&lt;workspace-hash&gt;/&lt;id&gt;/
  session.json      # metadata, messages, tool steps (redacted)
  results/          # large tool-result blobs
  recovery.json     # partial assistant text + last event offset</code></pre>
<p><code>session.json</code> never stores API keys or MCP header values.</p>
<h2 id="hosts">Host mapping</h2>
<p>Host backends store their own threads. tny keeps a thin local alias so <code>tny resume last</code> still works.</p>
<table>
  <thead><tr><th>Backend</th><th>Stored pointer</th></tr></thead>
  <tbody>
    <tr><td>cursor</td><td>versioned <code>cursor-sdk.v1</code>: <code>agent_id</code>, <code>run_id</code>, exclusive <code>after_offset</code>, local/cloud runtime</td></tr>
    <tr><td>codex</td><td><code>thread_id</code></td></tr>
    <tr><td>acp</td><td>agent argv + <code>sessionId</code></td></tr>
    <tr><td>openai</td><td>full transcript</td></tr>
  </tbody>
</table>
<h2 id="compact">Compaction</h2>
<p>After eight completed turns on the native loop, the latest four stay verbatim and older turns become a structured summary. <code>/compact</code> forces condensation of everything before the latest turn. The on-disk transcript stays intact; only the model view shrinks.</p>
<h2 id="recover">Recovery</h2>
<p>Streaming text and tool progress persist continuously. <code>/continue</code> and <code>tny ask --resume last --continue-recovery</code> replay from <code>recovery.json</code>. <code>tny session recover &lt;id&gt;</code> copies a corrupt session. <code>tny doctor</code> prints the recover command when it can fix a file.</p>
"""
    return page_shell(
        title="Sessions — tny",
        description="Save, resume, compact, and recover.",
        from_docs=True,
        active="docs",
        canonical="docs/sessions.html",
        current_doc="docs/sessions.html",
        toc=[
            ("id", "Identity"),
            ("disk", "On disk"),
            ("hosts", "Host mapping"),
            ("compact", "Compaction"),
            ("recover", "Recovery"),
        ],
        body=article(
            "Sessions",
            "Workspace-scoped history with a thin alias for host threads.",
            inner,
        ),
    )


def docs_permissions() -> str:
    inner = f"""
<h2 id="modes">Modes</h2>
<table>
  <thead><tr><th>Mode</th><th>Native behavior</th></tr></thead>
  <tbody>
    <tr><td><code>ask</code></td><td>Prompt on unresolved sensitive tools</td></tr>
    <tr><td><code>auto</code></td><td>Rules and session grants first; remaining sensitive calls may use a cheap heuristic or the active model; still-unresolved prompts (TUI) or fails (CI)</td></tr>
    <tr><td><code>yolo</code></td><td>Skip tny permission checks and the command sandbox for this process. Does not rewrite saved settings</td></tr>
  </tbody>
</table>
<p>Default: <code>yolo</code> for every provider. Host providers run their own loops and do not hand tny a gate for built-in tools. <code>ask</code> and <code>auto</code> govern the native loop; for Cursor, only explicitly registered custom-tool callbacks cross tny's policy boundary, and sensitive callbacks fail closed unless yolo.</p>
<h2 id="what">What needs approval</h2>
<p>Never: <code>list_files</code>, <code>glob_files</code>, <code>grep_files</code>, <code>read_file</code>, <code>file_info</code> inside the workspace.</p>
<p>Always, unless a rule or grant allows it: writes, deletes, renames, <code>run_command</code>, <code>open_file</code>, <code>install_skill</code>, <code>vision</code>, any path outside the workspace, MCP <code>tools/call</code>.</p>
<p>Prompt choices: Yes / Yes and don't ask again (session grant) / No. Keys: <code>y</code> / <code>a</code> / <code>n</code>.</p>
<h2 id="rules">Persistent rules</h2>
<p>Only in <code>~/.tny/settings.json</code>. Project <code>.tny.json</code> cannot grant authority. Last match wins. Workspace rules beat user-global. Wildcards are glob-style.</p>
<pre><code>{{
  "permission": {{
    "*": "ask",
    "bash": {{ "git *": "allow", "git push *": "deny" }},
    "edit": {{ "docs/*": "allow", "*": "deny" }}
  }}
}}</code></pre>
<h2 id="sandbox">Sandbox</h2>
<p>Separate from permission. An allowed command still runs inside <code>os</code>, <code>none</code>, or <code>auto</code>. <code>yolo</code> forces effective <code>none</code> for the process. v1 may ship <code>none</code> until seatbelt lands; <code>doctor</code> says so.</p>
{note("Host mapping", "Cursor's built-in tools remain headless — there is no per-call approval RPC. Registered Cursor custom-tool callbacks are the narrow exception and retain tny validation/sensitivity policy. Codex and ACP requests map onto y / a / n.")}
"""
    return page_shell(
        title="Permissions — tny",
        description="ask, auto, yolo, rules, and sandbox.",
        from_docs=True,
        active="docs",
        canonical="docs/permissions.html",
        current_doc="docs/permissions.html",
        toc=[
            ("modes", "Modes"),
            ("what", "What needs approval"),
            ("rules", "Rules"),
            ("sandbox", "Sandbox"),
        ],
        body=article(
            "Permissions",
            "yolo is the default. ask and auto are explicit opt-ins on the native loop.",
            inner,
        ),
    )


def docs_tools() -> str:
    inner = """
<h2 id="builtin">Built-in tools</h2>
<p>Native loop only, unless noted. Names match fx where muscle memory transfers.</p>
<table>
  <thead><tr><th>Area</th><th>Tools</th></tr></thead>
  <tbody>
    <tr><td>Files</td><td><code>list_files</code>, <code>glob_files</code>, <code>grep_files</code>, <code>read_file</code>, <code>write_file</code>, <code>edit_file</code>, <code>delete_file</code>, <code>rename_file</code>, <code>copy_file</code>, <code>create_folder</code>, <code>file_info</code></td></tr>
    <tr><td>Search</td><td><code>semantic_search</code> (lexical), <code>open_file</code></td></tr>
    <tr><td>Shell</td><td><code>terminal</code> (<code>run_command</code> is an alias)</td></tr>
    <tr><td>Web</td><td><code>web_search</code>, <code>web_fetch</code></td></tr>
    <tr><td>Skills</td><td><code>skill</code>, <code>install_skill</code></td></tr>
    <tr><td>Subagents</td><td><code>subagent</code></td></tr>
    <tr><td>MCP</td><td>search, select, then namespaced tools</td></tr>
    <tr><td>Runtime</td><td><code>ask_user_question</code>, <code>memory</code>, <code>read_tool_result</code></td></tr>
  </tbody>
</table>
<p>Large results are a bounded preview plus a session handle. <code>memory</code> writes <code>~/.tny/memories.json</code> only when asked.</p>
<h2 id="mcp">MCP</h2>
<p>Trusted profile only: <code>~/.tny/mcp.json</code>. Repo-local MCP files are never loaded — cloning a repo must not start a server. Treat server output as untrusted data.</p>
<p>v1 speaks stdio JSONL with lazy tool select. HTTP/SSE transports are deferred. <code>tny acp</code> uses only client-supplied <code>mcpServers</code>, not the user profile.</p>
<h2 id="skills">Skills</h2>
<p>A directory plus <code>SKILL.md</code> (YAML frontmatter <code>name</code>, <code>description</code>). Metadata is discovered at startup; the body loads only on invoke.</p>
<p>Search order, workspace upward, stop before <code>$HOME</code>: <code>skills/</code>, <code>.agents/skills/</code>, <code>.claude/skills/</code>, <code>.codex/skills/</code>, <code>.cursor/skills/</code>, <code>.opencode/skills/</code>. Then <code>~/.tny/skills/</code> and the same hidden names under <code>$HOME</code>.</p>
<h2 id="subagents">Subagents</h2>
<p>Child native sessions. The parent does not dump the child transcript into its own context. Children cannot raise permission mode above the creator. Host backends do not get tny-spawned subagents.</p>
<h2 id="agents-md">Project instructions</h2>
<p>Load <code>AGENTS.md</code> (and <code>CLAUDE.md</code> if <code>AGENTS.md</code> is absent) from <code>~/.tny/</code>, launch ancestors, and the primary workspace. Extra dirs do not contribute instructions. <code>context: false</code> disables this. Over <code>--ssh</code>, <code>~/.tny/</code> still loads as local user policy and project instructions come from the remote cwd — not the launch directory.</p>
"""
    return page_shell(
        title="Tools, MCP, skills — tny",
        description="Built-in tools, trusted MCP, skills, and subagents.",
        from_docs=True,
        active="docs",
        canonical="docs/tools.html",
        current_doc="docs/tools.html",
        toc=[
            ("builtin", "Built-in tools"),
            ("mcp", "MCP"),
            ("skills", "Skills"),
            ("subagents", "Subagents"),
            ("agents-md", "AGENTS.md"),
        ],
        body=article(
            "Tools, MCP, skills",
            "The native loop owns tools. Host backends own theirs.",
            inner,
        ),
    )


def docs_backends() -> str:
    inner = """
<h2 id="kinds">Two kinds of backend</h2>
<table>
  <thead><tr><th>Kind</th><th>Backends</th><th>Who runs tools?</th></tr></thead>
  <tbody>
    <tr><td>Host</td><td>Cursor bridge, Codex app-server, ACP client</td><td>The host process; tny executes only registered Cursor custom callbacks</td></tr>
    <tr><td>Native</td><td>OpenAI-compatible</td><td>tny</td></tr>
  </tbody>
</table>
<p>Never leak host-specific types into the TUI. Every backend maps onto one event set: <code>text_delta</code>, <code>thinking</code>, <code>tool_start</code>, <code>tool_end</code>, <code>permission_request</code>, <code>plan</code>, <code>usage</code>, <code>turn_end</code>, <code>error</code>.</p>
<h2 id="cursor">Cursor SDK Bridge</h2>
<p>tny pins the supported v1.0.30 release and implements all 5 services/29 RPCs: 27 outbound catalog/agent/run/artifact/usage/control calls plus reverse <code>CallCustomTool</code> and <code>CallStore</code>. Local/cloud AgentOptions cover images, modes, sandbox/review, MCP/subagents, and presence-sensitive built-in tool allow/deny. Classic gRPC/HTTP2 and Cursor's private <code>agent.v1</code> are not used.</p>
<p>Auth is <code>CURSOR_API_KEY</code> plus a per-process bridge bearer and independent loopback callback bearers. None enters logs or argv. Cursor owns built-in tools and exposes no per-call approval RPC; tny permission policy applies only to registered custom tools. Custom store callbacks own bounded local agent/run/event/checkpoint persistence.</p>
<p>Versioned session pointers retain agent, run, durable offset, and runtime. A dropped Send reconnects through ObserveRun without duplicating events; cancel waits for authoritative terminal state. <code>tny cursor</code> exposes full management/raw access. wasm reports distinct clean unsupported errors for conversations and management before spawn.</p>
<h2 id="codex">Codex app-server</h2>
<p>WebSocket JSON-RPC text frames, same surface the VS Code extension uses. The <code>"jsonrpc":"2.0"</code> header is omitted on the wire. Default to loopback; current Codex refuses non-loopback without <code>--ws-auth</code>. Do not send an <code>Origin</code> header.</p>
<p>Attach with <code>--codex-ws</code>, inherit a TUI-registered live host, or spawn on an ephemeral port. Never a fixed port that could collide.</p>
<h2 id="acp">ACP</h2>
<p>JSON-RPC 2.0 over stdio, one message per line. tny implements both sides:</p>
<table>
  <thead><tr><th>Mode</th><th>Command</th><th>Role</th></tr></thead>
  <tbody>
    <tr><td>Client</td><td><code>tny --provider acp --agent &lt;exe&gt;</code></td><td>Drive other agents</td></tr>
    <tr><td>Server</td><td><code>tny acp</code></td><td>Expose the native loop</td></tr>
  </tbody>
</table>
<p>Always answer <code>session/request_permission</code> or the agent hangs. Cursor extras (<code>cursor/ask_question</code>, <code>cursor/create_plan</code>) are answered if the argv is Cursor's ACP — that is still <code>--provider acp</code>.</p>
<h2 id="openai">OpenAI-compatible</h2>
<p>The Responses API (<code>POST /v1/responses</code>) with typed SSE events is the default wire; legacy Chat Completions stays available per provider via <code>wire_api: "chat"</code>. This is the only backend where tny owns the complete tool loop. The agent loop assembles preamble + <code>AGENTS.md</code> + skill catalog + history, posts, executes tool calls, and repeats until final text, a step limit, cancel, or deny. Sessions store the portable chat-shaped transcript on either wire.</p>
"""
    return page_shell(
        title="Backends — tny",
        description="Cursor bridge, Codex WebSocket, ACP, and the native OpenAI loop.",
        from_docs=True,
        active="docs",
        canonical="docs/backends.html",
        current_doc="docs/backends.html",
        toc=[
            ("kinds", "Two kinds"),
            ("cursor", "Cursor"),
            ("codex", "Codex"),
            ("acp", "ACP"),
            ("openai", "OpenAI-compatible"),
        ],
        body=article(
            "Backends",
            "A thin multiplexed frontend over host harnesses, plus one native loop.",
            inner,
        ),
    )


def docs_architecture() -> str:
    inner = """
<h2 id="picture">Process model</h2>
<pre><code>                 +-------------------------------------+
                 |  cli / tui  (one event loop)        |
                 +------------------+------------------+
                                    | normalized events
                 +------------------v------------------+
                 |  session + permission + render bus  |
                 +------------------+------------------+
        +---------------+-----------+----------+---------------+
        v               v                      v               v
  cursor-bridge    codex-app-server        acp-client     openai-native
  Connect sdk.v1   ws:// JSON-RPC          stdio JSON-RPC HTTP SSE + tools
  + callbacks</code></pre>
<p>One tny process, one primary workspace. Host processes are children or attach targets. Always have a shutdown path: cancel turn → close stream → Shutdown/EOF → wait → kill. Drain host stderr on a dedicated reader — a full pipe stalls the bridge and most ACP agents.</p>
<h2 id="loop">One event loop</h2>
<p>POSIX <code>poll</code>/<code>kqueue</code> only. No libuv. TUI pre-warm runs <code>connect()</code> plus <code>create_or_resume()</code> on one bounded thread and hands the backend back before events flow. Cursor's authenticated callback server normally shares the event loop; blocking Create/Resume may lend store traffic to one bounded pump thread, while owner-thread custom tools fail closed there.</p>
<h2 id="state">Config and state</h2>
<table>
  <thead><tr><th>Path</th><th>Contents</th></tr></thead>
  <tbody>
    <tr><td><code>~/.tny/settings.json</code></td><td>Model, permission mode, trusted Cursor local/cloud sdk.v1 options, UI, per-workspace overrides</td></tr>
    <tr><td><code>~/.tny/mcp.json</code></td><td>Trusted MCP servers only</td></tr>
    <tr><td><code>~/.tny/sessions/</code></td><td>Transcripts and recovery</td></tr>
    <tr><td><code>~/.tny/skills/</code></td><td>Managed skill installs</td></tr>
    <tr><td><code>~/.tny/tasks/</code></td><td>User task-preset Markdown definitions</td></tr>
    <tr><td><code>&lt;repo&gt;/.tny.json</code></td><td>Repo-safe limits only</td></tr>
    <tr><td><code>&lt;repo&gt;/.tny/tasks/</code></td><td>Project task-preset Markdown definitions</td></tr>
    <tr><td><code>&lt;repo&gt;/AGENTS.md</code></td><td>Project instructions</td></tr>
  </tbody>
</table>
<p>Credentials stay in the OS store or env vars. Not in project JSON. Never log tokens or ready-line JSON.</p>
<p>Cursor versioned pointers retain <code>agent_id</code>, <code>run_id</code>, <code>after_offset</code>, and local/cloud runtime. The bridge owns built-in tools; tny owns only explicit custom-tool and optional custom-store callbacks. The supported v1.0.30 bridge remains external; no private <code>agent.v1</code> HTTP/2 protocol is embedded. wasm returns distinct clean unsupported errors for conversations and management before bridge/callback work.</p>
<h2 id="lang">Language</h2>
<p>C11, vendored .c files you can see in <code>nm</code>: yyjson, picohttpparser, wslay. ANSI TUI, not a widget kit. macOS TLS is Security.framework, <code>dlopen</code>'d at first use. Never static OpenSSL or libcurl.</p>
"""
    return page_shell(
        title="Architecture — tny",
        description="One event loop, four backends, host processes stay external.",
        from_docs=True,
        active="docs",
        canonical="docs/architecture.html",
        current_doc="docs/architecture.html",
        toc=[
            ("picture", "Process model"),
            ("loop", "Event loop"),
            ("state", "Config"),
            ("lang", "Language"),
        ],
        body=article(
            "Architecture",
            "A frontend plus one native loop. Not a fourth coding agent.",
            inner,
        ),
    )


def docs_size() -> str:
    inner = f"""
<h2 id="why">Why this exists</h2>
<p>fx v0.0.3 is a 6.44 MiB macOS / 11.12 MiB static Linux Zig binary. tny's job is to keep the Unix-shell harness and undercut those numbers in C11.</p>
<h2 id="measured">Measured bake-off</h2>
<p>Same machine, macOS arm64, hyperfine. Binary size is current for tny {VERSION}; startup and RSS retain the historical v0.1.0 bake-off until the next performance remeasurement:</p>
<table>
  <thead><tr><th>Metric</th><th>fx 0.0.3</th><th>tny</th><th>Result</th></tr></thead>
  <tbody>
    <tr><td>Stripped binary</td><td>6,748,416 B (6.4 MiB)</td><td>714,624 B (0.68 MiB)</td><td>9.4× smaller</td></tr>
    <tr><td><code>--version</code></td><td>2.2 ms ± 0.3</td><td>1.7 ms ± 0.2</td><td>1.3× faster</td></tr>
    <tr><td>Max RSS</td><td>3.0 MiB</td><td>2.1 MiB</td><td>1.4× less memory</td></tr>
    <tr><td>TUI first prompt</td><td>—</td><td>3.3–4.3 ms</td><td>budget &lt; 10 ms</td></tr>
  </tbody>
</table>
<p>Do not publish a 10 µs claim. That number is fx's <code>FX_BENCH=1</code> path (parse argv, exit before TTY). Beat measured exec and first paint.</p>
<h2 id="ttft">Time to first token</h2>
<p>Everything between Enter and the provider seeing the turn is pre-paid or overlapped. The TUI warms the host and creates the session in the background. <code>tny ask</code> connects while it reads a piped prompt. Codex one-shots attach to an already-running app-server.</p>
<table>
  <thead><tr><th>Path</th><th>before</th><th>after</th></tr></thead>
  <tbody>
    <tr><td>TUI: Enter → first output</td><td>411 ms</td><td>6 ms</td></tr>
    <tr><td><code>ask</code> with piped stdin</td><td>875 ms</td><td>449 ms</td></tr>
    <tr><td>codex one-shot (attach vs spawn)</td><td>235 ms</td><td>11 ms</td></tr>
  </tbody>
</table>
<p>What remains is the model's own time to first token. Client-side, tny is not the bottleneck.</p>
<h2 id="budgets">Budgets</h2>
<p>These apply to the tny executable only. Host binaries do not count.</p>
<table>
  <thead><tr><th>Build</th><th>Must</th><th>Stretch</th></tr></thead>
  <tbody>
    <tr><td>macOS arm64, stripped</td><td>&lt; 1.8 MiB</td><td>&lt; 1.2 MiB</td></tr>
    <tr><td>Linux musl static, stripped</td><td>&lt; 1.5 MiB</td><td>&lt; 1.0 MiB</td></tr>
    <tr><td>Windows x86_64 (MSYS), stripped</td><td>&lt; 2.0 MiB</td><td>—</td></tr>
    <tr><td><code>--version</code> / <code>ask --help</code></td><td>&lt; 5 ms median</td><td>&lt; 2 ms</td></tr>
    <tr><td>TUI first prompt (no spawn)</td><td>&lt; 10 ms</td><td>&lt; 5 ms</td></tr>
  </tbody>
</table>
<h2 id="how">How it stays small</h2>
<ul>
  <li>C11, no C++ stdlib, no Zig runtime extras.</li>
  <li>ANSI TUI, not a widget kit.</li>
  <li>yyjson + picohttpparser + wslay, vendored as .c files.</li>
  <li>SecureTransport <code>dlopen</code>'d at first TLS use — eager framework linking costs ~1.2 ms per launch.</li>
  <li>Lazy backend load. No upgrade/MCP/skill walk before first prompt.</li>
  <li>No WASM, NAPI, sounds, or bundled Node in the default CLI.</li>
</ul>
"""
    return page_shell(
        title="Size and speed — tny",
        description="Budgets and measured bake-off versus fx.",
        from_docs=True,
        active="docs",
        canonical="docs/size.html",
        current_doc="docs/size.html",
        toc=[
            ("why", "Why"),
            ("measured", "Bake-off"),
            ("ttft", "TTFT"),
            ("budgets", "Budgets"),
            ("how", "How"),
        ],
        body=article(
            "Size and speed",
            "Beat measured fx, not the README. Host binaries are not in the budget.",
            inner,
        ),
    )


def docs_tnytty() -> str:
    inner = f"""
<h2 id="what">What it is</h2>
<p><span class="name">tnytty</span> is a C11 terminal emulator in the tny monorepo. The core is a headless VT library. Adapters around it run a real program in a real pty, paint a native window, or serve every session over HTTP so a script, an agent, or another person can read the screen and type keys.</p>
<p>Same principles as the harness: one small static-friendly binary, microsecond <code>--help</code>, one <code>poll(2)</code> loop, docs as the contract. It is not bundled into <code>tny</code>. Host agents stay out of this binary too.</p>
<h2 id="build">Build</h2>
{cmd(TNYTTY_INSTALL)}
<p>A C11 compiler and <code>make</code> are enough. Vendored libraries come from the shared <code>third_party/</code> tree. The release binary is <code>tnytty/build/tnytty</code>. Stripped size target: under 500 KiB on Linux x86_64.</p>
{cmd("tnytty --version")}
{cmd("tnytty --help")}
<p>Neither flag opens a pty or a socket.</p>
<h2 id="first">First session</h2>
<p>With no arguments tnytty is a passthrough terminal running <code>$SHELL</code>. The local termios is restored on exit, and the child's exit code is the process exit code.</p>
{cmd("tnytty")}
{cmd("tnytty run --listen 127.0.0.1:7681 -- htop")}
{cmd("curl -s 127.0.0.1:7681/v1/sessions")}
<p>The attached tty stays interactive. The same session is also a REST object: read the grid, send keys, share the URL.</p>
<h2 id="pillars">What you get</h2>
<table>
  <thead><tr><th>Pillar</th><th>Behavior</th></tr></thead>
  <tbody>
    <tr><td>VT engine</td><td>xterm-compatible subset: cursor, erase, scroll regions, alternate screen, SGR 16/256/truecolor, UTF-8 with wide and combining glyphs, scrollback, title, bracketed paste, DSR/DA</td></tr>
    <tr><td>Nerd fonts</td><td>Private Use Area glyphs are width 1 by policy. Width comes from built-in tables, not locale <code>wcwidth</code></td></tr>
    <tr><td>Kitty graphics</td><td>APC <code>G</code> sequences are parsed, recorded per session, and passed through. <code>tnytty icat</code> is a subcommand, not a second binary</td></tr>
    <tr><td>HTTP API</td><td>Create, read, type into, resize, and tear down sessions with <code>curl</code>. Loopback is open; anything else needs a bearer token</td></tr>
    <tr><td>Native window</td><td><code>tnytty gui</code> on macOS: CoreText grid, transparent titlebar by default, iTerm2-style split panes</td></tr>
  </tbody>
</table>
<h2 id="who">Who it is for</h2>
<ul>
  <li>Humans who want a lean terminal — passthrough today, platform windows where they exist.</li>
  <li>Agents and scripts that need a terminal as a service: spawn a TUI, read the rendered screen as text or JSON, send keys, tear it down. No expect scripts.</li>
  <li>Session sharing: give someone the base URL and token and they watch or drive the same live pty.</li>
</ul>
<h2 id="not">What it is not</h2>
<p>Not an IDE. Not a shell — it spawns <code>$SHELL</code> or the command after <code>--</code>. The VT core has no panes; split panes live in the macOS window adapter. There is no in-tree web frontend. Font rasterization is the renderer's job.</p>
<h2 id="platforms">Platforms</h2>
<table>
  <thead><tr><th>Platform</th><th>Today</th></tr></thead>
  <tbody>
    <tr><td>Linux (glibc, musl)</td><td><code>run</code>, <code>serve</code>, <code>icat</code>, full API. <code>gui</code> exits 1 with a clean error</td></tr>
    <tr><td>macOS arm64</td><td>Same, plus <code>gui</code></td></tr>
    <tr><td>Windows</td><td><code>icat</code> works. <code>run</code> / <code>serve</code> / <code>gui</code> exit 1 with a clean error until ConPTY</td></tr>
    <tr><td>iOS</td><td>Remote-only by design — no local pty. Future renderer attaches over the HTTP API</td></tr>
  </tbody>
</table>
<p>Next: <a href="tnytty-cli.html">CLI</a>, <a href="tnytty-api.html">HTTP API</a>, <a href="tnytty-architecture.html">architecture</a>.</p>
"""
    return page_shell(
        title="tnytty — the tiny terminal",
        description="C11 terminal emulator: headless VT core, pty sessions, kitty graphics, and a REST API.",
        from_docs=True,
        active="tnytty",
        canonical="docs/tnytty.html",
        current_doc="docs/tnytty.html",
        toc=[
            ("what", "What it is"),
            ("build", "Build"),
            ("first", "First session"),
            ("pillars", "What you get"),
            ("who", "Who it is for"),
            ("not", "What it is not"),
            ("platforms", "Platforms"),
        ],
        body=article(
            "tnytty",
            "The tiny terminal. A headless VT core, a pty, and a REST API.",
            inner,
        ),
    )


def docs_tnytty_cli() -> str:
    inner = f"""
<h2 id="tree">Command tree</h2>
<p>Noninteractive-first, like the harness. Layered <code>--help</code> with examples. No pty or socket until a subcommand needs one.</p>
<pre><code>tnytty                      # alias for tnytty run (spawn $SHELL)
tnytty run [flags] [-- CMD ARGS...]
tnytty gui [flags] [-- CMD ARGS...]
tnytty serve [flags]
tnytty icat [flags] FILE|-
tnytty --help | --version</code></pre>
<h2 id="run">tnytty run</h2>
<p>Attach the current terminal to a new session: raw-mode passthrough both ways, mirrored through the VT core so the session stays scriptable while a human types. Exits with the child's exit code. Local termios is always restored.</p>
<table>
  <thead><tr><th>Flag</th><th>Meaning</th></tr></thead>
  <tbody>
    <tr><td><code>-- CMD ARGS...</code></td><td>Command to run (default <code>$SHELL</code>, else <code>/bin/sh</code>)</td></tr>
    <tr><td><code>--cols N --rows N</code></td><td>Initial size (default: the attached tty)</td></tr>
    <tr><td><code>--listen HOST:PORT</code></td><td>Also serve the HTTP API on this loop</td></tr>
    <tr><td><code>--token T</code></td><td>API bearer token</td></tr>
  </tbody>
</table>
<p><code>SIGWINCH</code> on the attached tty resizes the session live.</p>
{cmd("tnytty run --listen 127.0.0.1:7681 -- htop")}
<h2 id="gui">tnytty gui</h2>
<p>A native window — tnytty's own renderer, not a passthrough into another terminal. macOS only in this phase. Everywhere else exits 1 with <code>gui: not supported on this platform yet</code>.</p>
<p>The window starts a session the same way <code>run</code> does, sizes the grid from the window's pixels, and exits with the child's exit code. Closing the window or Cmd-Q ends every session in it.</p>
<p>A window can hold several sessions as split panes. Each pane has its own session, scrollback, and selection. Every pane of a <code>--listen</code> window is a session in the API. A new pane runs the same command from the launch directory. Up to 32 panes.</p>
<table>
  <thead><tr><th>Flag</th><th>Meaning</th></tr></thead>
  <tbody>
    <tr><td><code>--titlebar transparent|opaque</code></td><td>Overrides <code>macos-titlebar</code></td></tr>
    <tr><td><code>--font NAME</code></td><td>Monospaced family</td></tr>
    <tr><td><code>--font-size N</code></td><td>Points</td></tr>
    <tr><td><code>--padding N</code></td><td>Points around the grid</td></tr>
    <tr><td><code>--cols N --rows N</code></td><td>Initial grid (default 100×30)</td></tr>
    <tr><td><code>--listen HOST:PORT</code></td><td>HTTP API on the same loop</td></tr>
    <tr><td><code>--token T</code></td><td>API bearer token</td></tr>
  </tbody>
</table>
{cmd("tnytty gui")}
{cmd("tnytty gui --titlebar opaque -- htop")}
{cmd("tnytty gui --listen 127.0.0.1:7681 -- vim")}
<p>Command chords belong to the window and never reach the child. Bindings follow iTerm2:</p>
<table>
  <thead><tr><th>Chord</th><th>Action</th></tr></thead>
  <tbody>
    <tr><td>Cmd-D</td><td>Split vertically (new pane to the right)</td></tr>
    <tr><td>Cmd-Shift-D</td><td>Split horizontally (new pane below)</td></tr>
    <tr><td>Cmd-W</td><td>Close the focused pane; closes the window when it is the last</td></tr>
    <tr><td>Cmd-Opt-arrows</td><td>Focus the neighbouring pane, or do nothing if there is none</td></tr>
    <tr><td>Cmd-[ / Cmd-]</td><td>Previous / next pane in reading order, wrapping</td></tr>
    <tr><td>Cmd-C / Cmd-V</td><td>Copy the selection / paste into the focused pane</td></tr>
    <tr><td>Cmd-Q</td><td>Quit</td></tr>
  </tbody>
</table>
<p>Click a pane to focus it. Click-drag selects characters; double-click a word; triple-click a line. Releasing a drag copies to the pasteboard unless <code>copy-on-select = false</code>. Mouse reporting to the child (SGR 1006) is not wired yet.</p>
<p>Not in this phase: scrollback scrolling, kitty graphics drawn in the window (they are still parsed and recorded), and IME / marked text.</p>
<h2 id="serve">tnytty serve</h2>
<p>Headless. No local tty. Sessions exist only through the API.</p>
<table>
  <thead><tr><th>Flag</th><th>Meaning</th></tr></thead>
  <tbody>
    <tr><td><code>--listen HOST:PORT</code></td><td>Bind address (default <code>127.0.0.1:7681</code>)</td></tr>
    <tr><td><code>--token T</code></td><td>Bearer token; required for non-loopback binds. Auto-generated and printed if omitted</td></tr>
  </tbody>
</table>
{cmd('tnytty serve --listen 0.0.0.0:7681 --token "$TNYTTY_TOKEN"')}
<h2 id="icat">tnytty icat</h2>
<p>Print an image inline in any kitty-graphics terminal (kitty, ghostty, WezTerm, tnytty renderers). Reads a file or stdin (<code>-</code>). PNG is transmitted as-is. Other formats are a clean error in this phase.</p>
{cmd("tnytty icat photo.png")}
{cmd("curl -s https://example.com/x.png | tnytty icat -")}
<h2 id="env">Environment</h2>
<p><code>TNYTTY_TOKEN</code> (API token), <code>SHELL</code> (default command), <code>XDG_CONFIG_HOME</code> / <code>HOME</code> (the <a href="tnytty-config.html">config file</a>). <code>NO_COLOR</code> does not apply — tnytty emits the child's bytes, not its own styling.</p>
"""
    return page_shell(
        title="CLI — tnytty",
        description="tnytty run, gui, serve, and icat.",
        from_docs=True,
        active="tnytty",
        canonical="docs/tnytty-cli.html",
        current_doc="docs/tnytty-cli.html",
        toc=[
            ("tree", "Command tree"),
            ("run", "run"),
            ("gui", "gui"),
            ("serve", "serve"),
            ("icat", "icat"),
            ("env", "Environment"),
        ],
        body=article(
            "tnytty CLI",
            "run, gui, serve, icat. Flags before a pty is opened.",
            inner,
        ),
    )


def docs_tnytty_config() -> str:
    inner = f"""
<h2 id="location">Location</h2>
<p>tnytty needs no config file. Every key has a default. CLI flags override both. The file exists so the native window can be styled once.</p>
<pre><code>$XDG_CONFIG_HOME/tnytty/config      # if XDG_CONFIG_HOME is set
~/.config/tnytty/config             # otherwise</code></pre>
<p>A missing file is fine. An unreadable one, or one with a bad value, is not: tnytty prints the path, the line number, and what it expected, then exits 2.</p>
<h2 id="format">Format</h2>
<p><code>key = value</code>, one per line. <code>#</code> starts a comment. Whitespace around the key and value is trimmed. No sections, no quoting, no continuations.</p>
<pre><code># ~/.config/tnytty/config
font = JetBrains Mono
font-size = 14
macos-titlebar = transparent
padding = 10</code></pre>
<p>Unknown keys and lines without <code>=</code> are warned on stderr and skipped, so a file written for a newer tnytty still starts. A known key with a bad value is a clean error. The file is limited to 64 KiB; each line to 511 bytes.</p>
<h2 id="window">Window</h2>
<table>
  <thead><tr><th>Key</th><th>Values</th><th>Default</th></tr></thead>
  <tbody>
    <tr><td><code>font</code></td><td>installed family name</td><td>empty → Menlo, then the system fixed-pitch UI font</td></tr>
    <tr><td><code>font-size</code></td><td>4–288</td><td><code>13</code></td></tr>
    <tr><td><code>macos-titlebar</code></td><td><code>transparent</code>, <code>opaque</code></td><td><code>transparent</code></td></tr>
    <tr><td><code>padding</code></td><td>0–256</td><td><code>8</code></td></tr>
  </tbody>
</table>
<p>Transparent extends the terminal background under the traffic lights and hides the title; the grid is inset so no text is covered. Opaque restores the system titlebar and shows the session title.</p>
<h2 id="colors">Colors</h2>
<p>Values are <code>#rrggbb</code> (the <code>#</code> is optional). Defaults are a dark theme chosen so every palette entry except index 0 clears WCAG AA (4.5:1) against the default background. The unit suite recomputes those ratios and fails the build if a change drops one.</p>
<table>
  <thead><tr><th>Key</th><th>Default</th><th>Meaning</th></tr></thead>
  <tbody>
    <tr><td><code>foreground</code></td><td><code>#d7dae3</code></td><td>Default text (12.91:1 on the background — AAA)</td></tr>
    <tr><td><code>background</code></td><td><code>#14161f</code></td><td>Terminal and window background</td></tr>
    <tr><td><code>divider</code></td><td><code>#3a4152</code></td><td>1 px rule between split panes</td></tr>
    <tr><td><code>palette0</code> … <code>palette15</code></td><td>see below</td><td>SGR 30–37 / 90–97 and the low 16 of the 256-color space</td></tr>
    <tr><td><code>bold-brightens</code></td><td><code>true</code></td><td>SGR 1 on indexed 0–7 also selects bright 8–15</td></tr>
  </tbody>
</table>
<p>Indices 16–255 are the fixed xterm cube and grayscale ramp and are not configurable. Index 0 is a background color and is allowed to sit below 4.5:1.</p>
<table>
  <thead><tr><th>Key</th><th>Color</th><th>Role</th></tr></thead>
  <tbody>
    <tr><td><code>palette0</code></td><td><code>#2a2f3a</code></td><td>black</td></tr>
    <tr><td><code>palette1</code></td><td><code>#f07178</code></td><td>red</td></tr>
    <tr><td><code>palette2</code></td><td><code>#9ece6a</code></td><td>green</td></tr>
    <tr><td><code>palette3</code></td><td><code>#e0c980</code></td><td>yellow</td></tr>
    <tr><td><code>palette4</code></td><td><code>#7aa2f7</code></td><td>blue</td></tr>
    <tr><td><code>palette5</code></td><td><code>#c792ea</code></td><td>magenta</td></tr>
    <tr><td><code>palette6</code></td><td><code>#56cfd8</code></td><td>cyan</td></tr>
    <tr><td><code>palette7</code></td><td><code>#b9bfca</code></td><td>white</td></tr>
    <tr><td><code>palette8</code></td><td><code>#7a8296</code></td><td>bright black</td></tr>
    <tr><td><code>palette9</code></td><td><code>#ff8b92</code></td><td>bright red</td></tr>
    <tr><td><code>palette10</code></td><td><code>#b4f08a</code></td><td>bright green</td></tr>
    <tr><td><code>palette11</code></td><td><code>#ffe08a</code></td><td>bright yellow</td></tr>
    <tr><td><code>palette12</code></td><td><code>#9fc1ff</code></td><td>bright blue</td></tr>
    <tr><td><code>palette13</code></td><td><code>#e0b0ff</code></td><td>bright magenta</td></tr>
    <tr><td><code>palette14</code></td><td><code>#7fe6ee</code></td><td>bright cyan</td></tr>
    <tr><td><code>palette15</code></td><td><code>#eef1f7</code></td><td>bright white</td></tr>
  </tbody>
</table>
<h2 id="behavior">Behavior</h2>
<table>
  <thead><tr><th>Key</th><th>Values</th><th>Default</th></tr></thead>
  <tbody>
    <tr><td><code>copy-on-select</code></td><td><code>true</code>, <code>false</code> (also yes/no, 1/0)</td><td><code>true</code></td></tr>
    <tr><td><code>status-bar</code></td><td><code>true</code>, <code>false</code></td><td><code>true</code> — one-line transient messages; <code>false</code> returns the row to the grid</td></tr>
  </tbody>
</table>
<h2 id="flags">Flags win</h2>
<p>Every window key has a flag on <code>tnytty gui</code>. A bad flag is an error, never a warning. Colors and behavior keys have no flags yet.</p>
{cmd("tnytty gui --titlebar opaque --font Menlo --font-size 15 --padding 0")}
<h2 id="later">Not configurable yet</h2>
<p>Cursor style, key bindings, selection colors (the highlight inverts the cell), the status-bar timeout, the split ratio (always 50/50; dividers are not draggable), and scrollback size are compiled-in. New keys are additive.</p>
"""
    return page_shell(
        title="Configuration — tnytty",
        description="Config file location, keys, palette, and flag overrides.",
        from_docs=True,
        active="tnytty",
        canonical="docs/tnytty-config.html",
        current_doc="docs/tnytty-config.html",
        toc=[
            ("location", "Location"),
            ("format", "Format"),
            ("window", "Window"),
            ("colors", "Colors"),
            ("behavior", "Behavior"),
            ("flags", "Flags"),
            ("later", "Not yet"),
        ],
        body=article(
            "tnytty configuration",
            "Optional file. Flags override it. Missing is not an error.",
            inner,
        ),
    )


def docs_tnytty_api() -> str:
    inner = f"""
<h2 id="bind">Binding and auth</h2>
<p>Served by <code>tnytty serve</code>, <code>tnytty run --listen</code>, or <code>tnytty gui --listen</code>. JSON in, JSON out, except the plain-text screen dump. HTTP/1.1, <code>Connection: close</code> per response. SSE streaming is later work.</p>
<ul>
  <li>Default bind: <code>127.0.0.1:7681</code>. Loopback needs no token.</li>
  <li>Non-loopback (<code>--listen 0.0.0.0:7681</code>) requires a token: <code>--token</code> / <code>TNYTTY_TOKEN</code>, or tnytty generates one and prints it once. Requests then need <code>Authorization: Bearer &lt;token&gt;</code>.</li>
  <li><code>--token</code> on loopback enforces the token there too.</li>
  <li>Token comparison is constant-time. Failures are <code>401 {{"error":"unauthorized"}}</code>.</li>
</ul>
<p>Sharing a session is sharing the base URL plus the token. Anyone with both can read the screen and type into the pty. Treat the token like an SSH key. There is no TLS in-process — put a reverse proxy in front of a public bind.</p>
<h2 id="health">GET /v1/health</h2>
<p><code>200 {{"ok":true,"version":"…","sessions":N}}</code>. No auth on loopback; token required elsewhere.</p>
<h2 id="list">GET /v1/sessions</h2>
<p><code>200 {{"sessions":[{{...session}},...]}}</code></p>
<h2 id="create">POST /v1/sessions</h2>
<p>Body, all optional: <code>{{"cmd":["bash","-l"],"cols":80,"rows":24}}</code>. Defaults: <code>$SHELL</code> (else <code>/bin/sh</code>), 80×24. <code>201</code> with the session object, or <code>500</code> if spawn fails. Sessions created through a <code>run</code> or <code>gui</code> listener share that process's event loop.</p>
<h2 id="one">GET /v1/sessions/{{id}}</h2>
<pre><code>{{
  "id": "a1b2c3d4",
  "cmd": ["bash", "-l"],
  "cols": 80, "rows": 24,
  "title": "~/src — bash",
  "alive": true, "exit_code": null,
  "created_unix": 1756400000,
  "graphics": 2
}}</code></pre>
<p><code>404 {{"error":"no such session"}}</code> if the id is unknown. Ids are short random hex, not sequential, because they appear in shared URLs.</p>
<h2 id="screen">GET /v1/sessions/{{id}}/screen</h2>
<p>Default / <code>?format=text</code>: <code>text/plain; charset=utf-8</code> — the grid as UTF-8 lines, trailing blanks trimmed, one newline per row.</p>
<p><code>?format=json</code> adds cursor, size, title, and styled runs:</p>
<pre><code>{{
  "cols": 80, "rows": 24,
  "cursor": {{"x": 3, "y": 0, "visible": true}},
  "alt_screen": false,
  "lines": [
    {{"text": "hi",
     "runs": [{{"start":0,"len":2,"fg":"#ff0000","bg":"","attrs":["bold"]}}]}}
  ]
}}</code></pre>
<p>Colors: <code>""</code> default, <code>"@n"</code> for indexed n, <code>"#rrggbb"</code> for truecolor. Attrs: <code>bold, faint, italic, underline, blink, reverse, hidden, strike</code>.</p>
<h2 id="input">POST /v1/sessions/{{id}}/input</h2>
<p>Body <code>{{"text":"ls -la\\r"}}</code> (UTF-8, written verbatim) or <code>{{"base64":"…"}}</code> for exact bytes. <code>200 {{"written":N}}</code>. Control characters are the caller's job (<code>\\r</code> for Enter). The API never rewrites input.</p>
<p>Bytes that do not fit in the kernel pty buffer are queued on the session and drained as the child reads, so <code>"written":N</code> always equals the bytes you sent. The queue is 4 MiB per session. A write that would exceed it is rejected whole with <code>503 {{"error":"input queue full"}}</code>. Request bodies are capped at 1 MiB.</p>
<h2 id="resize">POST /v1/sessions/{{id}}/resize</h2>
<p>Body <code>{{"cols":120,"rows":40}}</code> → grid reflow, <code>TIOCSWINSZ</code>, <code>SIGWINCH</code>. <code>200</code> with the session. A session attached to <code>run</code> or a GUI pane gets its geometry from that frontend and returns <code>409</code>.</p>
<h2 id="delete">DELETE /v1/sessions/{{id}}</h2>
<p><code>SIGHUP</code> to the child process group, reap, drop the session. <code>200 {{"ok":true}}</code>. Attached <code>run</code> / pane sessions return <code>409</code> — close the terminal or pane instead. Teardown escalates to <code>SIGKILL</code> after 100 ms if the child ignores <code>SIGHUP</code>.</p>
<h2 id="errors">Errors</h2>
<p>Always JSON: <code>{{"error":"&lt;message&gt;"}}</code>. 400 bad request, 401 auth, 404 unknown session or route, 405 method, 409 the attached frontend owns geometry or lifetime, 500 spawn/OS, 503 input queue full.</p>
<h2 id="examples">Examples</h2>
{cmd('tnytty serve --listen 0.0.0.0:7681 --token "$TNYTTY_TOKEN"')}
<pre><code>curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \\
     -d '{{"cmd":["htop"],"cols":120,"rows":32}}' \\
     http://host:7681/v1/sessions
curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \\
     http://host:7681/v1/sessions/a1b2c3d4/screen
curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \\
     -d '{{"text":"q"}}' http://host:7681/v1/sessions/a1b2c3d4/input</code></pre>
"""
    return page_shell(
        title="HTTP API — tnytty",
        description="REST surface for scriptable, shareable terminal sessions.",
        from_docs=True,
        active="tnytty",
        canonical="docs/tnytty-api.html",
        current_doc="docs/tnytty-api.html",
        toc=[
            ("bind", "Auth"),
            ("health", "health"),
            ("list", "list"),
            ("create", "create"),
            ("one", "one session"),
            ("screen", "screen"),
            ("input", "input"),
            ("resize", "resize"),
            ("delete", "delete"),
            ("errors", "Errors"),
            ("examples", "Examples"),
        ],
        body=article(
            "tnytty HTTP API",
            "curl is the reference client. A leaked token is shell access.",
            inner,
        ),
    )


def docs_tnytty_architecture() -> str:
    inner = """
<h2 id="shape">Shape</h2>
<pre><code>            keystrokes / HTTP POST input          bytes from child
                     │                                   ▲
                     ▼                                   │
   ┌──────────┐   write   ┌─────────┐   read   ┌─────────────────┐
   │ adapters │ ────────► │   pty   │ ───────► │  vt core (lib)  │
   │ cli/api  │           │  seam   │          │ grid+parser+gfx │
   └──────────┘           └─────────┘          └─────────────────┘
        ▲                                               │
        └── screen dumps (text/JSON), passthrough bytes ┘</code></pre>
<p>Three layers, one rule: the core is headless and I/O-free.</p>
<h2 id="vt">VT core</h2>
<p><code>src/vt/</code> is a pure state machine. <code>vt_feed(vt, bytes, len)</code> consumes child output and updates a cell grid. It performs no I/O. When the emulated program asks a question (DSR, DA), the core emits the answer through a caller-provided <code>respond</code> callback. Kitty graphics APC payloads go through a <code>graphics</code> callback plus a per-session record.</p>
<p>Cells store a codepoint, one combining mark, packed attributes, and tagged fg/bg (default / 256 / truecolor). Width comes from built-in tables: East-Asian wide blocks and emoji are 2, combining marks are 0, Private Use Area is 1. The parser is incremental — UTF-8, CSI, OSC, and APC all survive arbitrary read-boundary splits, enforced by the unit suite.</p>
<h2 id="pty">pty seam</h2>
<p><code>src/term/pty.h</code> is the platform seam. POSIX uses <code>posix_openpt</code> / <code>fork</code> / <code>TIOCSCTTY</code>. Other platforms implement the same header (ConPTY on Windows, none on iOS). Platform code lives only behind this seam, never as <code>#ifdef</code>s in the core.</p>
<h2 id="session">Session registry</h2>
<p>A session is pty + VT + metadata (id, argv, size, creation time, exit status). The registry owns lifecycle. Both adapters address sessions through it. Input is queued, never dropped: a write is all-or-nothing from the caller's side, capped at 4 MiB. Past the cap the HTTP adapter returns 503. <code>tnytty run</code> applies back-pressure on the attached tty instead.</p>
<h2 id="http">HTTP adapter</h2>
<p>A minimal HTTP/1.1 server on vendored picohttpparser. Request handling is a pure function from (method, path, body, auth) to a response buffer, so the router is unit-testable without sockets.</p>
<h2 id="ui">Native renderer</h2>
<p>The window seam sits beside the pty seam: one implementation per platform (<code>window_macos.c</code>; <code>window_stub.c</code> elsewhere). A CPU cell rasterizer reads the same getters the HTTP screen endpoint reads and paints an RGBA framebuffer, dirty rows only. Split panes share one framebuffer and one loop. Glyph masks come from the platform through one callback.</p>
<h2 id="loop">One event loop</h2>
<p>A single <code>poll(2)</code> multiplexes the listening socket, HTTP connections, every session's pty master, and (in <code>run</code>) stdin plus <code>SIGWINCH</code> via a self-pipe. No threads in the hot path. Each ready pty gets a bounded number of reads per turn so a continuous producer cannot starve HTTP, signals, or sibling panes.</p>
<p><code>tnytty gui</code> keeps that loop and adds the window. AppKit's queue is not a pollable fd, so the poll timeout is bounded (8 ms) and each turn also drains the queue and presents dirty rows. The main thread is the only thread that touches a VT core.</p>
<p>Foreground adapters mark their terminal or pane sessions as attached. The HTTP API can read the screen and write input, but cannot resize or destroy them behind the adapter's retained pointer.</p>
<h2 id="deps">Vendored dependencies</h2>
<p>From the shared root <code>third_party/</code> only: picohttpparser, yyjson, greatest. The VT core depends on none of them. No ncurses. No GUI toolkit in the core.</p>
<h2 id="platforms">Platforms</h2>
<table>
  <thead><tr><th>Platform</th><th>Phase 1</th><th>Target</th></tr></thead>
  <tbody>
    <tr><td>Linux</td><td>works — <code>run</code>, <code>serve</code>, <code>icat</code>, API. <code>gui</code> is a clean error</td><td>Wayland/X11 window behind the same seam; static musl publish builds</td></tr>
    <tr><td>macOS arm64</td><td>works, including <code>gui</code></td><td>first-class</td></tr>
    <tr><td>Windows</td><td>clean error from <code>run</code>/<code>serve</code>/<code>gui</code>; <code>icat</code> works</td><td>ConPTY + DirectWrite window</td></tr>
    <tr><td>iOS</td><td>remote-only — no local pty</td><td>renderer + HTTP client attaching to a remote session</td></tr>
  </tbody>
</table>
<p>New platform support is a new <code>pty_*.c</code> (and <code>window_*.c</code> if there is a window) plus a row on this table plus CI. The VT core must compile on every target, including iOS and wasm.</p>
"""
    return page_shell(
        title="Architecture — tnytty",
        description="Headless VT core, pty seam, one event loop, platform adapters.",
        from_docs=True,
        active="tnytty",
        canonical="docs/tnytty-architecture.html",
        current_doc="docs/tnytty-architecture.html",
        toc=[
            ("shape", "Shape"),
            ("vt", "VT core"),
            ("pty", "pty seam"),
            ("session", "Sessions"),
            ("http", "HTTP"),
            ("ui", "Renderer"),
            ("loop", "Event loop"),
            ("deps", "Dependencies"),
            ("platforms", "Platforms"),
        ],
        body=article(
            "tnytty architecture",
            "The core takes bytes and returns state. Everything else is an adapter.",
            inner,
        ),
    )


def not_found() -> str:
    return page_shell(
        title="404 — tny",
        description="Page not found.",
        from_docs=False,
        active=None,
        canonical="404.html",
        body='<main class="not-found"><div><h1>404</h1><p>This page could not be found. <a href="index.html">Home</a> · <a href="docs/index.html">Docs</a> · <a href="docs/tnytty.html">tnytty</a></p></div></main>',
    )


def sitemap_xml(page_names: list[str]) -> str:
    urls = []
    for rel_path in page_names:
        if rel_path == "404.html":
            continue
        if rel_path == "index.html":
            loc = SITE_BASE
        elif rel_path == "docs/index.html":
            loc = SITE_BASE + "docs/"
        else:
            loc = SITE_BASE + rel_path
        urls.append(f"  <url><loc>{loc}</loc></url>")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        + "\n".join(urls)
        + "\n</urlset>\n"
    )


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if not text.endswith("\n"):
        path.write_text(text + "\n", encoding="utf-8")


def main() -> None:
    pages = {
        "index.html": landing(),
        "404.html": not_found(),
        "docs/index.html": docs_quick(),
        "docs/install.html": docs_install(),
        "docs/providers.html": docs_providers(),
        "docs/ask.html": docs_ask(),
        "docs/cli.html": docs_cli(),
        "docs/workflows.html": docs_workflows(),
        "docs/tui.html": docs_tui(),
        "docs/sessions.html": docs_sessions(),
        "docs/permissions.html": docs_permissions(),
        "docs/tools.html": docs_tools(),
        "docs/backends.html": docs_backends(),
        "docs/architecture.html": docs_architecture(),
        "docs/size.html": docs_size(),
        "docs/tnytty.html": docs_tnytty(),
        "docs/tnytty-cli.html": docs_tnytty_cli(),
        "docs/tnytty-config.html": docs_tnytty_config(),
        "docs/tnytty-api.html": docs_tnytty_api(),
        "docs/tnytty-architecture.html": docs_tnytty_architecture(),
    }
    for rel_path, html in pages.items():
        write(SITE / rel_path, html)
        print("wrote", rel_path)
    write(SITE / "sitemap.xml", sitemap_xml(list(pages)))
    print("wrote sitemap.xml")


if __name__ == "__main__":
    main()
