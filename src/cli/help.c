/* help.c — static help text; layered per command with examples (docs/cli.md). */
#include "cli/cli.h"
#include <stdio.h>
#include <string.h>

void help_root(void) {
    fputs("tny v" TNY_VERSION "\n"
          "Fast, tiny coding-agent harness for the terminal.\n"
          "\n"
          "tny starts an interactive session by default. Use `tny ask` for one\n"
          "noninteractive request.\n"
          "\n"
          "Usage:\n"
          "  tny [global flags] [command] [...args]\n"
          "\n"
          "Commands:\n"
          "  ask <prompt>           Run one noninteractive request\n"
          "  edit FILE              Exact-match replacement from stdin\n"
          "  ask-user QUESTION      Ask the owning session frontend (socket-bound)\n"
          "  image attach PATH      Attach an image to the next request (socket-bound)\n"
          "  resume [last|<id>]     Resume a session interactively\n"
          "  acp                    Start an ACP server over stdio (native loop)\n"
          "  sessions               List saved sessions for this workspace\n"
          "  session <last|id>      Inspect one saved session\n"
          "  providers | backends   List configured providers and doctor hints\n"
          "  provider setup NAME    Add an OpenAI-compatible provider (interactive on a tty)\n"
          "  models                 List available models for the active provider\n"
          "  cursor COMMAND         Cursor sdk.v1 catalog, agents, runs, artifacts, and raw RPCs\n"
          "  tasks                  List built-in and discovered task presets\n"
          "  task show NAME         Inspect one resolved task preset\n"
          "  permissions            Show the permission mode and rules\n"
          "  workspace list|add|remove|clear\n"
          "                         Manage additional workspace directories\n"
          "  status                 Show configuration and runtime information\n"
          "  doctor                 Run local health and preflight checks\n"
          "  usage                  Show local token usage\n"
          "  mcp [list|call]        List MCP servers, or call one MCP tool\n"
          "  login | logout | setup Provider-specific auth and configuration\n"
          "  help                   Show this help\n"
          "\n"
          "Global flags (leading, before the command):\n"
          "  --ssh TARGET           Run every workspace tool (files, grep, terminal)\n"
          "                         on user@host[:port] over OpenSSH; tny stays local\n"
          "  --ssh-cwd DIR          Remote working directory for --ssh (default: login\n"
          "                         dir); a leading ~ means the remote home, so quote it\n"
          "  --provider NAME        cursor | acp | openai | codex | claude | grok, a\n"
          "                         named API profile, or settings ACP agent acp@NAME\n"
          "                         (--backend also works)\n"
          "  --cwd DIR              Primary workspace (default: current directory)\n"
          "  --model ID             Model for this run\n"
          "  --effort LEVEL         Reasoning effort: " TNY_EFFORT_LEVELS "\n"
          "                         (or any level `tny models` lists for the provider)\n"
          "                         (--reasoning-effort also works)\n"
          "  --system-prompt TEXT   Custom system prompt. openai-compatible providers\n"
          "                         carry it on the system/instructions field; cursor\n"
          "                         and acp have no such field, so it is prepended\n"
          "                         to the session's first user message\n"
          "  --task NAME            Apply a named task preset (review, optimizer,\n"
          "                         document, retro, or a discovered .tny/tasks NAME.md)\n"
          "  --add-dir DIR          Extra workspace directory; repeatable, process-only\n"
          "  --permission-mode M    ask | auto | yolo (default: yolo)\n"
          "  --auto | --yolo        Permission-mode convenience aliases\n"
          "  --max-steps N          Cap the native agent loop at N model calls per turn\n"
          "                         (default: unlimited; 'unlimited' clears a repo cap)\n"
          "  --max-extension-iterations N\n"
          "                         Cap extension-requested follow-up turns (default:\n"
          "                         unlimited; 'unlimited' or 0 clears the cap)\n"
          "  --no-extensions        Do not load ~/.tny/extensions for this process\n"
          "  --fast                 Paid fast tier where the provider has one\n"
          "                         (openai, codex, cursor; higher speed and cost)\n"
          "  --json                 Machine-readable output where listed\n"
          "  --color MODE           auto | always | never (--no-color = never).\n"
          "                         NO_COLOR disables colors only; the status bar keeps\n"
          "                         reverse video. CLICOLOR_FORCE forces color back on\n"
          "  --ephemeral            Keep conversation/session artifacts in memory only\n"
          "                         (--no-save is a compatibility alias)\n"
          "  -r                     Open the saved-session picker\n"
          "  -c, --continue         Resume the latest workspace session\n"
          "  --resume <last|id>     Resume the latest session or an exact id\n"
          "  -h, --help             Show help    -v, --version  Print the version\n"
          "\n"
          "Provider flags:\n"
          "  cursor: --bridge-bin PATH        (env CURSOR_SDK_BRIDGE_BIN, CURSOR_API_KEY)\n"
          "  codex:  ChatGPT login from `codex login` ($CODEX_HOME/auth.json) against\n"
          "          chatgpt.com/backend-api/codex on tny's native loop; --codex-bin PATH\n"
          "          (env TNY_CODEX_BIN) names the CLI `tny login` delegates to\n"
          "  acp:    --agent CMD -- args... for ad-hoc agents; acp@NAME selects an\n"
          "          acp.NAME command + args from ~/.tny/settings.json\n"
          "  openai: --base-url URL --api-key-env NAME (env OPENAI_BASE_URL, OPENAI_API_KEY)\n"
          "          --wire-api responses|chat  Wire protocol (default responses;\n"
          "                         chat for legacy-only providers, docs/adr/0016)\n"
          "  claude: Claude Code OAuth token (env CLAUDE_CODE_OAUTH_TOKEN,\n"
          "          ANTHROPIC_API_KEY, or ~/.claude/.credentials.json)\n"
          "  grok:   xAI session (~/.grok/auth.json via `tny --provider grok login`,\n"
          "          native device auth) or env XAI_API_KEY\n"
          "\n"
          "Python extensions:\n"
          "  ~/.tny/extensions/NAME.py or NAME/index.py (requires external python3).\n"
          "  Extensions are trusted code with your full user permissions; install only\n"
          "  code you trust. See docs/extensions.md.\n"
          "\n"
          "Examples:\n"
          "  tny                          Start a fresh interactive session\n"
          "  tny --ephemeral             Interactive session with no local transcript\n"
          "  tny --ssh dev@box --ssh-cwd '~/app'   TUI whose tools act on the remote box\n"
          "  tny --ssh dev@box:2222 ask \"run the tests\"   One-shot, tools run remotely\n"
          "  tny ask \"explain src/main.c\"  One request, Markdown on stdout\n"
          "  tny --task review ask \"inspect the current diff\"\n"
          "  tny ask --json \"list the public CLI\"\n"
          "  tny --provider codex login   ChatGPT sign-in (runs `codex login`)\n"
          "  tny --provider codex ask \"run the tests\"\n"
          "  tny --effort xhigh ask \"prove this lock-free queue is correct\"\n"
          "  tny --provider codex --fast ask \"quick: run the tests\"\n"
          "  tny --provider acp --agent gemini -- --acp\n"
          "  tny --provider acp@claude-code --model claude-sonnet-4-6\n",
          stdout);
}

static const char *ask_help =
    "Usage: tny ask [options] [prompt]\n"
    "\n"
    "Run one noninteractive request, then exit.\n"
    "\n"
    "Options:\n"
    "  --json               Write one JSON object to stdout\n"
    "  --stdin              Read the prompt from stdin\n"
    "  --image PATH         Attach an image file; repeatable\n"
    "  --output-schema X    Constrain the final answer to a JSON Schema (file\n"
    "                       path or inline JSON; openai provider only)\n"
    "  -B, --background     Detach: print the session id, run the turn in a\n"
    "                       forked child; answer lands in the session `result`\n"
    "                       (native only; the browser build errors, docs/adr/0031)\n"
    "  --resume <last|id>   Continue the latest workspace session or an id\n"
    "                       (fails while a background turn holds the session)\n"
    "  --steer              With --resume on a running session: interrupt it and\n"
    "                       redirect with TEXT (pending tool work is abandoned)\n"
    "  --continue-recovery  Replay the interrupted response before this turn\n"
    "  --ephemeral          Keep conversation/session artifacts in memory only\n"
    "  --no-save            Compatibility alias for --ephemeral\n"
    "  --no-color           Disable SGR styling for this request\n"
    "  --print-usage        Report token usage on stderr (also TNY_PRINT_USAGE=1)\n"
    "  --auto               Auto-review unresolved permissions (native loop)\n"
    "  --yolo               Disable permission checks and sandbox (the default)\n"
    "  --resume-id ID       Compatibility alias for --resume ID\n"
    "  --task NAME          Apply a runtime task preset (global or ask-local)\n"
    "  --                   Treat every following argument as prompt text\n"
    "\n"
    "Stdout is assistant Markdown (or one JSON object with --json).\n"
    "Stderr carries progress and tool lines.\n"
    "Exit codes: 0 finished, 1 startup/config, 2 run failed, 130 interrupted.\n"
    "\n"
    "Examples:\n"
    "  tny ask \"summarize this repository\"\n"
    "  printf 'summarize src/\\n' | tny ask --stdin\n"
    "  tny ask --json --ephemeral \"list the public CLI\"\n"
    "  tny ask --resume last \"now add tests\"\n"
    "  id=$(tny ask -B \"audit the Makefile\")   # detached; `tny session $id` to read\n"
    "  tny ask --resume $id --steer \"drop that — check the tests instead\"\n"
    "  tny ask --output-schema schema.json \"extract the TODOs as JSON\"\n"
    "  tny --provider cursor --model composer-2 ask \"find the login bug\"\n";

static const char *edit_help =
    "Usage: tny edit [--json] [--marker STR] FILE\n"
    "\n"
    "Replace an exact string from stdin. Fence input replaces exactly one match;\n"
    "--json reads {\"old\":...,\"new\":...,\"replace_all\":false} and writes one\n"
    "kind:\"edit\" object. No edit occurs on zero or ambiguous matches.\n"
    "\n"
    "Options:\n"
    "  --json          Read the JSON stdin form and write one JSON object\n"
    "  --marker STR    Fence prefix (default: ***)\n"
    "  -h, --help      Show this help\n"
    "\n"
    "Stdout carries the result. Stderr carries progress and errors.\n"
    "Exit codes: 0 edited, 1 usage/I/O, 2 zero or multiple matches, 130 interrupted.\n"
    "\n"
    "Examples:\n"
    "  printf '*** SEARCH\\nold\\n*** REPLACE\\nnew\\n*** END\\n' | tny edit FILE\n"
    "  printf '%s\\n' '{\"old\":\"old\",\"new\":\"new\",\"replace_all\":false}' | \\\n"
    "    tny edit --json FILE\n"
    "  printf '@@ SEARCH\\nold\\n@@ REPLACE\\nnew\\n@@ END\\n' | \\\n"
    "    tny edit --marker @@ FILE\n";

static const char *sessions_help =
    "Usage: tny sessions [--json] [--all] [--limit N] [--cursor ID]\n"
    "\n"
    "List saved sessions for the current workspace (--all: every workspace).\n"
    "\n"
    "Examples:\n"
    "  tny sessions\n"
    "  tny sessions --json --limit 10\n";

static const char *tasks_help =
    "Usage: tny tasks\n\n"
    "List built-in and discovered task presets. Use --json as a global flag.\n"
    "Inspect one preset with `tny task show NAME`.\n";

static const char *task_help =
    "Usage: tny task show NAME\n\n"
    "Show one resolved task preset, including its instructions. Use --json as a\n"
    "global flag. This is the explicit inspection surface; status and errors never\n"
    "print task instruction bodies.\n";

static const char *session_help =
    "Usage: tny session <last|id> [--json] [--wait] [--timeout SECS]\n"
    "       tny session attach <id>\n"
    "       tny session stop <id> [--kill]\n"
    "       tny session recover <id>\n"
    "\n"
    "Inspect one saved session, attach to a live one, stop a running\n"
    "background task, or copy a recoverable corrupt session. `attach`\n"
    "streams a running turn live (snapshot, then events; docs/adr/0053) —\n"
    "^C detaches and the turn keeps running. `stop` SIGTERMs the task's\n"
    "process group (spawned hosts included) and the session finalizes status\n"
    "\"interrupted\"; --kill escalates to SIGKILL if the task ignores SIGTERM\n"
    "(docs/adr/0031).\n"
    "\n"
    "--wait blocks until a detached background turn has finished, then\n"
    "prints the session; the exit code is the turn's exit_code (0 done, 2 run\n"
    "failed or stale, 130 interrupted). --timeout SECS implies --wait, exit 124\n"
    "if the turn is still running when it elapses (docs/adr/0041).\n"
    "\n"
    "Examples:\n"
    "  tny session last\n"
    "  tny session 4f2a1c90aa317b22 --json\n"
    "  tny session $id --wait --json | jq -r .result.output\n"
    "  tny session $id --wait --timeout 600\n"
    "  tny session attach 4f2a1c90aa317b22\n"
    "  tny session stop 4f2a1c90aa317b22\n";

static const char *workspace_help =
    "Usage: tny workspace list|add|remove|clear [DIR] [--json]\n"
    "\n"
    "Manage additional workspace directories saved for this workspace.\n"
    "Process-only extras: tny --add-dir DIR ...\n"
    "\n"
    "Examples:\n"
    "  tny workspace list\n"
    "  tny workspace add ../shared-lib\n";

static const char *acp_help =
    "Usage: tny acp [--model ID] [--log-file PATH]\n"
    "\n"
    "Serve tny's native OpenAI-compatible loop as an ACP agent over stdio\n"
    "(protocolVersion 1). stdout is protocol-only; logs go to --log-file.\n"
    "Use the leading --ephemeral global flag to disable local session storage.\n"
    "\n"
    "Examples:\n"
    "  tny acp\n"
    "  tny --ephemeral --model gpt-4.1-mini acp\n";

static const char *setup_help =
    "Usage: tny setup [--base-url URL] [--api-key-env NAME] [--model ID]\n"
    "\n"
    "Write the OpenAI-compatible provider profile into ~/.tny/settings.json.\n"
    "Keys stay in the environment; only the variable NAME is stored.\n"
    "\n"
    "Examples:\n"
    "  tny setup --base-url https://openrouter.ai/api/v1 --api-key-env OPENROUTER_API_KEY \\\n"
    "            --model anthropic/claude-sonnet-4.6\n";

static const char *doctor_help =
    "Usage: tny doctor [--json]\n"
    "\n"
    "Run local health checks: config files, session store, host binaries\n"
    "(cursor-sdk-bridge, codex, ACP agents), and provider credentials.\n";

static const char *resume_help =
    "Usage: tny resume [last|<id>]\n"
    "\n"
    "Resume a saved session in the interactive shell. This is incompatible with\n"
    "--ephemeral because ephemeral runs never import stored conversation state.\n"
    "\n"
    "Examples:\n"
    "  tny resume last\n"
    "  tny resume 4f2a1c90aa317b22\n";

static const char *cursor_help =
    "Usage: tny cursor COMMAND [ARGS]\n\n"
    "Manage the complete Cursor SDK Bridge sdk.v1 surface. Catalog and lifecycle\n"
    "aliases cover users, models, repositories, agents, runs, messages, artifacts,\n"
    "and usage. `rpc SERVICE METHOD [JSON|-]` exposes every pinned outbound RPC;\n"
    "DeleteAgent requires --yes. CURSOR_API_KEY and cursor-sdk-bridge are required.\n\n"
    "Options:\n"
    "  -h, --help              Show this help.\n\n"
    "Examples:\n"
    "  tny cursor me\n"
    "  tny cursor agents\n"
    "  tny cursor runs AGENT_ID\n"
    "  tny cursor download AGENT_ID artifact/path > artifact.bin\n"
    "  tny cursor rpc SdkAgentService GetRun '{\"runId\":\"run-…\"}'\n";

bool help_for(const char *command) {
    const char *text = NULL;
    if (strcmp(command, "ask") == 0) text = ask_help;
    else if (strcmp(command, "edit") == 0) text = edit_help;
    else if (strcmp(command, "ask-user") == 0)
        text = "Usage: tny ask-user [--json] QUESTION\n"
               "       printf 'QUESTION' | tny ask-user [--json]\n\n"
               "Ask the owning tny frontend a free-text question. Requires\n"
               "TNY_SESSION_SOCK from a tny terminal tool child.\n\n"
               "Options: --json machine output; -h, --help show this help.\n";
    else if (strcmp(command, "image") == 0)
        text = "Usage: tny image attach [--json] PATH\n\n"
               "Queue a validated image for the next provider request. Requires\n"
               "TNY_SESSION_SOCK from a tny terminal tool child.\n\n"
               "Options: --json machine output; -h, --help show this help.\n";
    else if (strcmp(command, "sessions") == 0) text = sessions_help;
    else if (strcmp(command, "session") == 0) text = session_help;
    else if (strcmp(command, "tasks") == 0) text = tasks_help;
    else if (strcmp(command, "task") == 0) text = task_help;
    else if (strcmp(command, "workspace") == 0) text = workspace_help;
    else if (strcmp(command, "acp") == 0) text = acp_help;
    else if (strcmp(command, "setup") == 0) text = setup_help;
    else if (strcmp(command, "doctor") == 0) text = doctor_help;
    else if (strcmp(command, "resume") == 0) text = resume_help;
    else if (strcmp(command, "cursor") == 0) text = cursor_help;
    else if (strcmp(command, "status") == 0)
        text = "Usage: tny status [--json]\n\nShow provider, model, permissions, workspace, and "
               "session counts.\n";
    else if (strcmp(command, "models") == 0)
        text = "Usage: tny models [--json]\n\nList models for the active provider (codex "
               "model/list, cursor ListModels, GET /models on openai).\nCatalogs that advertise "
               "reasoning-effort levels show them per model; pick one\nwith --effort or /effort.\n";
    else if (strcmp(command, "permissions") == 0)
        text =
            "Usage: tny permissions [--json]\n\nShow the permission mode and persistent rules.\n";
    else if (strcmp(command, "providers") == 0 || strcmp(command, "backends") == 0)
        text = "Usage: tny providers [--json]\n\nList the four providers with a one-line doctor "
               "hint each.\n";
    else if (strcmp(command, "provider") == 0)
        text = "Usage: tny provider [list] [--json]\n"
               "       tny provider setup NAME [--base-url URL]\n"
               "           [--api-key KEY | --api-key-env ENV] [--model M]\n"
               "           [--wire-api responses|chat]\n\n"
               "Write an OpenAI-compatible provider profile to ~/.tny/settings.json and\n"
               "make it the default. On a terminal, missing fields are prompted for (the\n"
               "key with echo off). --api-key stores the key in settings.json (0600);\n"
               "--api-key-env names an env var instead — an env var always wins.\n\n"
               "Examples:\n"
               "  tny provider setup openrouter --base-url https://openrouter.ai/api/v1 \\\n"
               "      --api-key-env OPENROUTER_API_KEY --model anthropic/claude-sonnet-4.6\n"
               "  tny provider setup opencode --base-url https://api.opencode.example/v1 \\\n"
               "      --api-key sk-…\n"
               "  tny provider setup   # interactive\n";
    else if (strcmp(command, "usage") == 0)
        text = "Usage: tny usage [--json]\n\nShow local token usage recorded from native-loop "
               "sessions.\n";
    else if (strcmp(command, "mcp") == 0)
        text = "Usage: tny mcp [list | call SERVER/TOOL] [--json]\n"
               "\n"
               "List MCP servers from ~/.tny/mcp.json plus any sources named in\n"
               "settings.json mcp.import_from (codex, claude, grok, cursor-agent). Import is\n"
               "off by default. Native names win on collision. Remote/SSE entries are\n"
               "skipped with a notice. tny never writes foreign config files.\n"
               "\n"
               "`call` runs one tools/call: the JSON arguments come from stdin (empty means\n"
               "{}), the result goes to stdout, and permissions are checked as\n"
               "mcp:SERVER/TOOL. Exit 1 for usage/config, 2 when the call is refused or\n"
               "fails. A big result is capped and written to a 0600 file under\n"
               "~/.tny/results/ whose path is printed.\n"
               "\n"
               "Examples:\n"
               "  tny mcp\n"
               "  tny mcp list --json\n"
               "  echo '{\"path\":\"README.md\"}' | tny mcp call fs/read_text_file\n"
               "  tny --json mcp call deploy/status < args.json\n";
    else if (strcmp(command, "login") == 0)
        text = "Usage: tny [--provider NAME] login [--device | --device-code]\n"
               "\n"
               "Sign in to the active provider. Tokens live in each provider's own store.\n"
               "\n"
               "  codex   Runs the Codex CLI's sign-in (`codex login`; --device selects its\n"
               "          device-code flow for headless machines). The result lands in\n"
               "          $CODEX_HOME/auth.json, which tny reads directly for the ChatGPT\n"
               "          Responses backend and refreshes itself when it expires.\n"
               "  claude  Reports the credential in use (CLAUDE_CODE_OAUTH_TOKEN,\n"
               "          ANTHROPIC_API_KEY, ~/.claude/.credentials.json), else runs\n"
               "          `claude setup-token` to mint a Claude Code OAuth token.\n"
               "  grok    Native RFC 8628 device-code sign-in against auth.x.ai (no grok\n"
               "          CLI needed): open the printed URL on any device, confirm the\n"
               "          code; the session lands in ~/.grok/auth.json (grok CLI format)\n"
               "          and auto-refreshes.\n"
               "  cursor  Reports whether CURSOR_API_KEY is set.\n"
               "  openai  Reports whether an API key resolved (tny setup configures one).\n"
               "\n"
               "Examples:\n"
               "  tny --provider codex login --device\n"
               "  tny --provider claude login\n"
               "  tny --provider grok login\n";
    else if (strcmp(command, "logout") == 0)
        text = "Usage: tny [--provider NAME] logout\n\nProvider-specific logout (removes "
               "$CODEX_HOME/auth.json for codex, "
               "native removal of the xAI entries in ~/.grok/auth.json for grok, env-var hints "
               "otherwise).\n";
    if (!text) {
        help_root();
        return true;
    }
    fputs(text, stdout);
    return true;
}
