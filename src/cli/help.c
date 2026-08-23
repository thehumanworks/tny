/* help.c — static help text; layered per command with examples (docs/cli.md). */
#include "cli/cli.h"
#include <stdio.h>
#include <string.h>

void help_root(void) {
    fputs(
"tny v" TNY_VERSION "\n"
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
"  resume [last|<id>]     Resume a session interactively\n"
"  acp                    Start an ACP server over stdio (native loop)\n"
"  sessions               List saved sessions for this workspace\n"
"  session <last|id>      Inspect one saved session\n"
"  providers              List configured providers and doctor hints\n"
"  models                 List available models for the active provider\n"
"  permissions            Show the permission mode and rules\n"
"  workspace list|add|remove|clear\n"
"                         Manage additional workspace directories\n"
"  status                 Show configuration and runtime information\n"
"  doctor                 Run local health and preflight checks\n"
"  usage                  Show local token usage\n"
"  login | logout | setup Provider-specific auth and configuration\n"
"  help                   Show this help\n"
"\n"
"Global flags (leading, before the command):\n"
"  --provider NAME        cursor | codex | acp | openai | claude | grok | a\n"
"                         named settings.json profile with a base_url\n"
"                         (--backend also works)\n"
"  --cwd DIR              Primary workspace (default: current directory)\n"
"  --model ID             Model for this run\n"
"  --effort LEVEL         Reasoning effort: " TNY_EFFORT_LEVELS "\n"
"                         (or any level `tny models` lists for the provider)\n"
"  --add-dir DIR          Extra workspace directory; repeatable, process-only\n"
"  --permission-mode M    ask | auto | yolo (default: yolo)\n"
"  --fast                 Paid fast tier where the provider has one\n"
"                         (openai, cursor, codex; higher speed and cost)\n"
"  --json                 Machine-readable output where listed\n"
"  --ephemeral            Keep conversation/session artifacts in memory only\n"
"                         (--no-save is a compatibility alias)\n"
"  -r                     Open the saved-session picker\n"
"  -c, --continue         Resume the latest workspace session\n"
"  --resume <last|id>     Resume the latest session or an exact id\n"
"  -h, --help             Show help    -v, --version  Print the version\n"
"\n"
"Provider flags:\n"
"  cursor: --bridge-bin PATH        (env CURSOR_SDK_BRIDGE_BIN, CURSOR_API_KEY)\n"
"  codex:  --codex-ws URL --codex-bin PATH --ws-token-file PATH\n"
"  acp:    --agent CMD -- args...   e.g. tny --provider acp --agent gemini -- --acp\n"
"  openai: --base-url URL --api-key-env NAME (env OPENAI_BASE_URL, OPENAI_API_KEY)\n"
"          --wire-api responses|chat  Wire protocol (default responses;\n"
"                         chat for legacy-only providers, docs/adr/0016)\n"
"  claude: Claude Code OAuth token (env CLAUDE_CODE_OAUTH_TOKEN,\n"
"          ANTHROPIC_API_KEY, or ~/.claude/.credentials.json)\n"
"  grok:   grok CLI session (~/.grok/auth.json via `tny --provider grok\n"
"          login`, device auth) or env XAI_API_KEY\n"
"\n"
"Examples:\n"
"  tny                          Start a fresh interactive session\n"
"  tny --ephemeral             Interactive session with no local transcript\n"
"  tny ask \"explain src/main.c\"  One request, Markdown on stdout\n"
"  tny ask --json \"list the public CLI\"\n"
"  tny --provider codex login   ChatGPT sign-in over the app-server\n"
"  tny --provider codex ask \"run the tests\"\n"
"  tny --effort xhigh ask \"prove this lock-free queue is correct\"\n"
"  tny --provider codex --fast ask \"quick: run the tests\"\n"
"  tny --provider acp --agent gemini -- --acp\n",
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
"  --resume <last|id>   Continue the latest workspace session or an id\n"
"  --continue-recovery  Replay the interrupted response before this turn\n"
"  --ephemeral          Keep conversation/session artifacts in memory only\n"
"  --no-save            Compatibility alias for --ephemeral\n"
"  --auto               Auto-review unresolved permissions (native loop)\n"
"  --yolo               Disable permission checks and sandbox (the default)\n"
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
"  tny ask --output-schema schema.json \"extract the TODOs as JSON\"\n"
"  tny --provider cursor ask --model composer-2 \"find the login bug\"\n";

static const char *sessions_help =
"Usage: tny sessions [--json] [--all] [--limit N] [--cursor ID]\n"
"\n"
"List saved sessions for the current workspace (--all: every workspace).\n"
"\n"
"Examples:\n"
"  tny sessions\n"
"  tny sessions --json --limit 10\n";

static const char *session_help =
"Usage: tny session <last|id> [--json]\n"
"       tny session recover <id>\n"
"\n"
"Inspect one saved session, or copy a recoverable corrupt session.\n"
"\n"
"Examples:\n"
"  tny session last\n"
"  tny session 4f2a1c90aa317b22 --json\n";

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

bool help_for(const char *command) {
    const char *text = NULL;
    if (strcmp(command, "ask") == 0) text = ask_help;
    else if (strcmp(command, "sessions") == 0) text = sessions_help;
    else if (strcmp(command, "session") == 0) text = session_help;
    else if (strcmp(command, "workspace") == 0) text = workspace_help;
    else if (strcmp(command, "acp") == 0) text = acp_help;
    else if (strcmp(command, "setup") == 0) text = setup_help;
    else if (strcmp(command, "doctor") == 0) text = doctor_help;
    else if (strcmp(command, "resume") == 0) text = resume_help;
    else if (strcmp(command, "status") == 0)
        text = "Usage: tny status [--json]\n\nShow provider, model, permissions, workspace, and session counts.\n";
    else if (strcmp(command, "models") == 0)
        text = "Usage: tny models [--json]\n\nList models for the active provider (codex model/list, cursor ListModels, GET /models on openai).\nCatalogs that advertise reasoning-effort levels show them per model; pick one\nwith --effort or /effort.\n";
    else if (strcmp(command, "permissions") == 0)
        text = "Usage: tny permissions [--json]\n\nShow the permission mode and persistent rules.\n";
    else if (strcmp(command, "providers") == 0 || strcmp(command, "backends") == 0)
        text = "Usage: tny providers [--json]\n\nList the four providers with a one-line doctor hint each.\n";
    else if (strcmp(command, "usage") == 0)
        text = "Usage: tny usage [--json]\n\nShow local token usage recorded from native-loop sessions.\n";
    else if (strcmp(command, "login") == 0)
        text =
"Usage: tny [--provider NAME] login [--device]\n"
"\n"
"Sign in to the active provider. tny never stores tokens itself.\n"
"\n"
"  codex   ChatGPT sign-in over `codex app-server` (account/login/start):\n"
"          prints the auth URL (browser flow), or a verification URL plus\n"
"          user code with --device (headless machines). The result lands in\n"
"          $CODEX_HOME/auth.json, which tny auto-detects.\n"
"  claude  Reports the credential in use (CLAUDE_CODE_OAUTH_TOKEN,\n"
"          ANTHROPIC_API_KEY, ~/.claude/.credentials.json), else runs\n"
"          `claude setup-token` to mint a Claude Code OAuth token.\n"
"  grok    Runs `grok login --device-auth` (RFC 8628 device code); the\n"
"          session token in ~/.grok/auth.json then drives the provider.\n"
"  cursor  Reports whether CURSOR_API_KEY is set.\n"
"  openai  Reports whether an API key resolved (tny setup configures one).\n"
"\n"
"Examples:\n"
"  tny --provider codex login --device\n"
"  tny --provider claude login\n"
"  tny --provider grok login\n";
    else if (strcmp(command, "logout") == 0)
        text = "Usage: tny [--provider NAME] logout\n\nProvider-specific logout (codex/grok CLI logout, env-var hints). tny stores no secrets itself.\n";
    if (!text) { help_root(); return true; }
    fputs(text, stdout);
    return true;
}
