#!/usr/bin/env python3
"""Generate the tny static site into site/."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "site"

REPO = "https://github.com/thehumanworks/tny"
INSTALL = "git clone https://github.com/thehumanworks/tny && cd tny && make"
VERSION = "0.1.0"
SIZE = "0.41mib"

NAV = (
    ("docs", "docs/index.html", "docs"),
    ("cli", "docs/cli.html", "cli"),
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
    prefix = "../" if from_docs else ""
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
    drawer = ""
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
            "Tiny 0.41mb binary",
            "Fifteen times smaller than fx on macOS. Designed for instant installation and embedding in resource-constrained environments and agent sandboxes.",
        ),
        (
            "Instant time to prompt",
            'The interactive shell first-paints in <span class="hi">3–4ms</span> and does no backend I/O before accepting input. Host processes are pre-warmed after first paint so Enter is not a spawn.',
        ),
        (
            "Four first-class backends",
            'Cursor via the SDK Bridge, Codex via <code>app-server</code> WebSockets, other agents via <a href="docs/backends.html">ACP</a>, and a native OpenAI-compatible tool loop.',
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
        ],
        body=article("Installation", "Clone, make, strip. Host agents stay on PATH.", inner),
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
<p>Spawns <code>cursor-sdk-bridge</code> and speaks Connect <code>sdk.v1</code> over HTTP/1.1. This is Cursor's own headless loop, not <code>agent acp</code>.</p>
{cmd("export CURSOR_API_KEY=…")}
{cmd("tny --provider cursor")}
<p>Resolve the bridge from <code>CURSOR_SDK_BRIDGE_BIN</code> or <code>PATH</code>. The bridge is 23–43 MiB of Bun — that weight is why it is never linked into tny.</p>
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
{note("Images", "Cursor and Codex ignore <code>--image</code> with a status line. The native loop accepts it.")}
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
tny models
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
tny --add-dir DIR           # repeatable, process-only
tny --permission-mode ask|auto|yolo   # default: yolo
tny --json                  # where listed
tny -r                      # session picker (TUI)
tny -c                      # resume last for this workspace</code></pre>
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
<p><code>--json</code> is accepted on <code>ask</code>, <code>status</code>, <code>doctor</code>, <code>permissions</code>, <code>models</code>, <code>session</code>, <code>sessions</code>, <code>workspace</code>, and <code>usage</code>.</p>
<h2 id="examples">Examples</h2>
{cmd("tny")}
{cmd('tny ask "explain src/main.c"')}
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


def docs_tui() -> str:
    inner = f"""
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
<p>Runtime: <code>/models</code> <code>/model</code> <code>/permissions</code> <code>/sandbox</code> <code>/backend</code> <code>/status</code> <code>/usage</code></p>
<p>Tools: <code>/mcp</code> <code>/skills</code> <code>/workspace</code> <code>/image</code> <code>/undo</code> <code>/copy</code> <code>/trace</code></p>
<p>Auth: <code>/login</code> <code>/logout</code> <code>/setup</code> — dispatched to the active backend. Backend-specific commands degrade to "not available" instead of crashing.</p>
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
    <tr><td>cursor</td><td><code>agent_id</code>, <code>run_id</code>, workspace, model</td></tr>
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
        body=article("Sessions", "Workspace-scoped history with a thin alias for host threads.", inner),
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
<p>Default: <code>yolo</code> for every provider. Host providers run their own loops and never hand tny a real gate. <code>ask</code> and <code>auto</code> are opt-ins via <code>--permission-mode</code>, <code>TNY_PERMISSION_MODE</code>, or <code>permission_mode</code> in settings, and are only enforceable on the native loop.</p>
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
{note("Host mapping", "Cursor's SDK bridge is headless — there is no per-call approval RPC. Codex and ACP requests map onto y / a / n. In default yolo they are accepted silently.")}
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
    inner = f"""
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
<p>Load <code>AGENTS.md</code> (and <code>CLAUDE.md</code> if <code>AGENTS.md</code> is absent) from <code>~/.tny/</code>, launch ancestors, and the primary workspace. Extra dirs do not contribute instructions. <code>context: false</code> disables this.</p>
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
    inner = f"""
<h2 id="kinds">Two kinds of backend</h2>
<table>
  <thead><tr><th>Kind</th><th>Backends</th><th>Who runs tools?</th></tr></thead>
  <tbody>
    <tr><td>Host</td><td>Cursor bridge, Codex app-server, ACP client</td><td>The host process</td></tr>
    <tr><td>Native</td><td>OpenAI-compatible</td><td>tny</td></tr>
  </tbody>
</table>
<p>Never leak host-specific types into the TUI. Every backend maps onto one event set: <code>text_delta</code>, <code>thinking</code>, <code>tool_start</code>, <code>tool_end</code>, <code>permission_request</code>, <code>plan</code>, <code>usage</code>, <code>turn_end</code>, <code>error</code>.</p>
<h2 id="cursor">Cursor SDK Bridge</h2>
<p>A local Connect server embedding <code>@cursor/sdk</code>, exposing <code>sdk.v1</code> over HTTP/1.1. Classic gRPC/HTTP2 will not connect. Pin standalone + protos to a release, not <code>main</code>.</p>
<p>Auth is two secrets: <code>CURSOR_API_KEY</code> on the env and on RPC options, plus a per-process bearer from the ready line. Never log the ready-line JSON.</p>
<p>This path is headless. There is no Allow/Deny RPC. Per-call approvals are ACP (<code>agent acp</code>), a different backend.</p>
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
<p>The Responses API (<code>POST /v1/responses</code>) with typed SSE events is the default wire; legacy Chat Completions stays available per provider via <code>wire_api: "chat"</code>. This is the only backend where tny executes tools itself. The agent loop assembles preamble + <code>AGENTS.md</code> + skill catalog + history, posts, executes tool calls, and repeats until final text, a step limit, cancel, or deny. Sessions store the portable chat-shaped transcript on either wire.</p>
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
    inner = f"""
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
  Connect sdk.v1   ws:// JSON-RPC          stdio JSON-RPC HTTP SSE + tools</code></pre>
<p>One tny process, one primary workspace. Host processes are children or attach targets. Always have a shutdown path: cancel turn → close stream → Shutdown/EOF → wait → kill. Drain host stderr on a dedicated reader — a full pipe stalls the bridge and most ACP agents.</p>
<h2 id="loop">One event loop</h2>
<p>POSIX <code>poll</code>/<code>kqueue</code> only. No libuv. The TUI pre-warm is the one deliberate extra thread: it runs <code>connect()</code> plus <code>create_or_resume()</code> and hands the backend back before any events flow.</p>
<h2 id="state">Config and state</h2>
<table>
  <thead><tr><th>Path</th><th>Contents</th></tr></thead>
  <tbody>
    <tr><td><code>~/.tny/settings.json</code></td><td>Model, permission mode, UI, per-workspace overrides</td></tr>
    <tr><td><code>~/.tny/mcp.json</code></td><td>Trusted MCP servers only</td></tr>
    <tr><td><code>~/.tny/sessions/</code></td><td>Transcripts and recovery</td></tr>
    <tr><td><code>~/.tny/skills/</code></td><td>Managed skill installs</td></tr>
    <tr><td><code>&lt;repo&gt;/.tny.json</code></td><td>Repo-safe limits only</td></tr>
    <tr><td><code>&lt;repo&gt;/AGENTS.md</code></td><td>Project instructions</td></tr>
  </tbody>
</table>
<p>Credentials stay in the OS store or env vars. Not in project JSON. Never log tokens or ready-line JSON.</p>
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
<p>Same machine, macOS arm64, hyperfine, fx 0.0.3 vs tny {VERSION}:</p>
<table>
  <thead><tr><th>Metric</th><th>fx 0.0.3</th><th>tny {VERSION}</th><th>Result</th></tr></thead>
  <tbody>
    <tr><td>Stripped binary</td><td>6,748,416 B (6.4 MiB)</td><td>426,792 B (0.41 MiB)</td><td>15.8× smaller</td></tr>
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


def not_found() -> str:
    return page_shell(
        title="404 — tny",
        description="Page not found.",
        from_docs=False,
        active=None,
        canonical="404.html",
        body='<main class="not-found"><div><h1>404</h1><p>This page could not be found. <a href="index.html">Home</a> · <a href="docs/index.html">Docs</a></p></div></main>',
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
        "docs/tui.html": docs_tui(),
        "docs/sessions.html": docs_sessions(),
        "docs/permissions.html": docs_permissions(),
        "docs/tools.html": docs_tools(),
        "docs/backends.html": docs_backends(),
        "docs/architecture.html": docs_architecture(),
        "docs/size.html": docs_size(),
    }
    for rel_path, html in pages.items():
        write(SITE / rel_path, html)
        print("wrote", rel_path)


if __name__ == "__main__":
    main()
