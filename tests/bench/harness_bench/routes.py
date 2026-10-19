"""Explicit benchmark routes. Credentials never select or change a destination."""

import json
import os
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


@dataclass(frozen=True)
class Route:
    name: str
    endpoint: str
    wire: str
    credential: str
    key_env: str = ""

    @property
    def paths(self):
        if self.name == "legacy-codex":
            return {
                "/responses",
                "/v1/responses",
                "/codex/responses",
                "/backend-api/codex/responses",
            }
        return {"/v1/" + self.wire, "/" + self.wire}

    def headers(self, auth_file=None):
        """Read credentials only after a fixed route has been selected; no refresh."""
        headers = {"Content-Type": "application/json", "Accept": "text/event-stream"}
        if self.credential == "key":
            token = os.environ.get(self.key_env)
        else:
            if auth_file is None:
                raise ValueError("auth store required")
            auth = json.loads(Path(auth_file).read_text())
            if not isinstance(auth, dict):
                raise ValueError("invalid auth store")
            if self.credential == "siwc":
                if (
                    auth.get("issuer") != "https://auth.openai.com"
                    or auth.get("plan_usage") is not True
                    or auth.get("token_type") != "Bearer"
                    or not isinstance(auth.get("scopes"), list)
                    or "chatgpt.tokens.use.direct" not in auth.get("scopes", [])
                    or not isinstance(auth.get("expires_at"), str)
                ):
                    raise ValueError("SIWC grant unavailable")
                expires = datetime.fromisoformat(
                    auth["expires_at"].replace("Z", "+00:00")
                )
                if expires.tzinfo is None or expires <= datetime.now(timezone.utc):
                    raise ValueError(
                        "SIWC grant expired; refresh with tny before benchmarking"
                    )
                token = auth.get("access_token")
            else:
                tokens = auth.get("tokens")
                if not isinstance(tokens, dict):
                    raise ValueError("invalid legacy auth store")
                token = tokens.get("access_token")
                account = tokens.get("account_id")
                if (
                    not isinstance(account, str)
                    or not account
                    or any(c in account for c in "\r\n")
                ):
                    raise ValueError("invalid legacy account")
                headers["chatgpt-account-id"] = account
        if not isinstance(token, str) or not token or any(c in token for c in "\r\n"):
            raise ValueError("benchmark credential unavailable")
        headers["Authorization"] = "Bearer " + token
        return headers

    def validate(self, body, model=None):
        if not isinstance(body, dict) or (model and body.get("model") != model):
            raise ValueError("unexpected model or body")
        if body.get("stream") is not True:
            raise ValueError("benchmark requires streaming")
        field = "messages" if self.wire == "chat/completions" else "input"
        wrong_field = "input" if field == "messages" else "messages"
        types = (list,) if field == "messages" else (list, str)
        if (
            field not in body
            or wrong_field in body
            or not isinstance(body[field], types)
        ):
            raise ValueError("body does not match selected wire")
        if self.credential == "siwc" and (
            body.get("store") is not False
            or "max_output_tokens" in body
            or any(
                isinstance(item, dict) and item.get("role") == "system"
                for item in body.get("input", [])
            )
        ):
            raise ValueError("body does not match SIWC contract")


ROUTES = {
    r.name: r
    for r in (
        Route(
            "openai-siwc", "https://api.openai.com/v1/responses", "responses", "siwc"
        ),
        Route(
            "openai-responses",
            "https://api.openai.com/v1/responses",
            "responses",
            "key",
            "OPENAI_API_KEY",
        ),
        Route(
            "openai-chat",
            "https://api.openai.com/v1/chat/completions",
            "chat/completions",
            "key",
            "OPENAI_API_KEY",
        ),
        Route(
            "inception-chat",
            "https://api.inceptionlabs.ai/v1/chat/completions",
            "chat/completions",
            "key",
            "INCEPTION_API_KEY",
        ),
        Route(
            "legacy-codex",
            "https://chatgpt.com/backend-api/codex/responses",
            "responses",
            "codex",
        ),
    )
}
