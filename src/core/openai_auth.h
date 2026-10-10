/* openai_auth.h — Sign in with ChatGPT for the builtin openai provider
 * (docs/adr/0186, docs/backends/openai-chatgpt.md).
 *
 * The ChatGPT-plan access token is a bearer for the public Responses API at
 * api.openai.com/v1 only — never chatgpt.com/backend-api (the legacy codex
 * profile's credential, codex_auth.c). tny's login lives in
 * ~/.tny/openai-auth.json (0600) and this host's stable agent-host id in
 * ~/.tny/openai-host-id. Tokens never print. */
#ifndef TNY_OPENAI_AUTH_H
#define TNY_OPENAI_AUTH_H

#include "core/config.h"

#include <stdbool.h>
#include <stdint.h>

#define TNY_OPENAI_API_BASE_URL    "https://api.openai.com/v1"
#define TNY_OPENAI_SIGNIN_ISSUER   "https://auth.openai.com"
#define TNY_OPENAI_SIGNIN_SCOPE    "chatgpt.tokens.use.direct"
#define TNY_OPENAI_SIGNIN_MODEL    "gpt-6.1-sol"
#define TNY_CHATGPT_USAGE_URL      "https://chatgpt.com/settings/usage"
#define TNY_OPENAI_REFRESH_EARLY_S 180 /* refresh this long before expiry */

char *tny_openai_store_path(void);   /* ~/.tny/openai-auth.json, malloc'd */
char *tny_openai_host_id_path(void); /* ~/.tny/openai-host-id, malloc'd */

/* This host's `ext_agent_host_id` (`urn:uuid:<v4>`), created once and
 * reused for every sign-in; NULL with a message on stderr when the file is
 * unreadable or malformed. malloc'd. */
char *tny_openai_host_id(void);

/* Issuer base (auth.openai.com, or a numeric-loopback TNY_OPENAI_OAUTH_ISSUER
 * for tests); NULL when the override is not a loopback URL. */
const char *tny_openai_issuer(void);
/* Where sign-in tokens go: api.openai.com/v1, or a numeric-loopback
 * TNY_OPENAI_SIGNIN_BASE_URL mock; NULL when the override is not loopback.
 * A ChatGPT-plan token is never sent anywhere else. */
const char *tny_openai_signin_base_url(void);

/* The saved registration and session. Every string is malloc'd or NULL. */
typedef struct {
    char *client_id;    /* issued `oaiapp_…` id; survives logout */
    char *email;        /* from the validated ID token; login_hint on reauth */
    char *subject;      /* ID-token `sub`; reauth must return the same */
    char *access_token; /* bearer for api.openai.com/v1; NULL after logout */
    bool plan_usage;    /* the grant includes chatgpt.tokens.use.direct */
    int64_t expires_at; /* access-token expiry (epoch seconds), 0 unknown */
} tny_openai_signin;

/* 0 usable access token, -1 no sign-in, -2 signed in but ChatGPT plan usage
 * not granted, -3 OOM. Reads the file only; no network, no stderr. */
int tny_openai_signin_read(tny_openai_signin *out);
void tny_openai_signin_free(tny_openai_signin *s);
/* A usable plan-usage token is stored (auto-detection). */
bool tny_openai_signin_present(void);
/* The signed-in account's email (malloc'd), or NULL. */
char *tny_openai_signin_email(void);

/* Refresh-token grant when the stored access token is within
 * TNY_OPENAI_REFRESH_EARLY_S of expiry. Serialized across processes with an
 * flock and re-read under it, because refresh tokens rotate and a reused one
 * is revoked. Terminal grant errors clear the session (stderr says to sign
 * in again); network/server errors leave the file untouched. */
void tny_openai_refresh_if_stale(void);
/* Before each provider request on a ctx that carries tny's sign-in: refresh
 * near expiry and swap the rotated access token into ctx->api_key (access
 * tokens last an hour; sessions outlive that). Network only when the token
 * is stale. Returns -1 when the sign-in this ctx carried has ended (signed
 * out, refresh refused, plan use withdrawn): the key is cleared and the
 * caller should ask for a new login. A replaced key or moved endpoint also
 * drops the sign-in, deliberately, and returns 0. */
int tny_openai_signin_sync(tny_ctx *ctx);

/* Validate an ID token's claims from a direct TLS token-endpoint response
 * (OIDC Core 3.1.3.7): iss, aud ∋ client_id, unexpired, nonce. On success
 * *sub / *email are malloc'd (email may be NULL). *why is static text. */
int tny_openai_id_token_check(const char *id_token, const char *issuer, const char *client_id,
                              const char *nonce, char **sub, char **email, const char **why);

/* Persist a fresh code-exchange result (0600, atomic, under the lock). */
int tny_openai_store_save_login(yyjson_val *token_response, const char *client_id,
                                const char *subject, const char *email);

/* openai_login.c: browser Authorization Code + PKCE sign-in with dynamic
 * client registration. Exit code (130 on Ctrl-C). */
int tny_openai_login(tny_ctx *ctx, bool device);
/* Revoke the refresh token, then drop the tokens; the registration and host
 * id stay for the next sign-in unless `forget`. Exit code. */
int tny_openai_logout(bool forget);

/* True when requests on ctx carry a ChatGPT-plan token to the public
 * Responses API: tny's own sign-in, or a non-`sk-` OPENAI_API_KEY bound for
 * api.openai.com. Shapes the request to the plan-usage contract. */
bool tny_openai_signin_mode(const tny_ctx *ctx);

/* NULL/empty (no override) or an http(s)://127.0.0.1[:port][/path] fixture
 * URL — the only accepted override for endpoints that receive subscription
 * credentials. No userinfo, DNS names, controls or fragments. */
bool tny_loopback_url_valid(const char *url);

#endif
