"""Immutable, forward-compatible extension capability views."""

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping, Optional, Tuple

KNOWN_STATES = ("supported", "unsupported", "unavailable")


def _freeze(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType(
            {str(key): _freeze(item) for key, item in value.items()}
        )
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(item) for item in value)
    return value


def _text(value: Any, default: str = "") -> str:
    return value if isinstance(value, str) else default


@dataclass(frozen=True)
class CapabilityEntry:
    """One stable capability key and its current provider/runtime truth.

    Unknown keys, state strings, and optional fields are preserved.  Callers
    should compare ``state`` with a known value and retain a fallback path.
    """

    name: str
    state: str
    reason: str = ""
    extra: Mapping[str, Any] = MappingProxyType({})

    @property
    def supported(self) -> bool:
        return self.state == "supported"

    @property
    def known_state(self) -> bool:
        return self.state in KNOWN_STATES


@dataclass(frozen=True)
class ProviderCapabilities:
    provider: str
    runtime: str
    entries: Tuple[CapabilityEntry, ...]
    extra: Mapping[str, Any] = MappingProxyType({})

    def get(self, name: str) -> Optional[CapabilityEntry]:
        for entry in self.entries:
            if entry.name == name:
                return entry
        return None

    def state(self, name: str, default: str = "unknown") -> str:
        entry = self.get(name)
        return entry.state if entry is not None else default

    def supports(self, name: str) -> bool:
        entry = self.get(name)
        return entry is not None and entry.supported


@dataclass(frozen=True)
class CapabilityView:
    """All provider matrices plus the provider selected during host setup.

    The model is immutable.  If a long-lived TUI later changes provider,
    ``event.provider`` is authoritative and ``for_provider(event.provider)``
    selects the matching matrix without mutating the setup snapshot.
    """

    schema_version: int
    selected_provider: str
    extension_enabled: bool
    python: str
    providers: Tuple[ProviderCapabilities, ...]
    extra: Mapping[str, Any] = MappingProxyType({})

    @classmethod
    def empty(cls) -> "CapabilityView":
        return cls(1, "unknown", False, "unknown", ())

    def for_provider(self, provider: str) -> Optional[ProviderCapabilities]:
        for value in self.providers:
            if value.provider == provider:
                return value
        return None

    @property
    def selected(self) -> Optional[ProviderCapabilities]:
        return self.for_provider(self.selected_provider)


def _entry(name: str, value: Any) -> CapabilityEntry:
    if not isinstance(value, Mapping):
        return CapabilityEntry(
            name=name, state="unknown", extra=_freeze({"value": value})
        )
    known = {"state", "reason"}
    return CapabilityEntry(
        name=name,
        state=_text(value.get("state"), "unknown"),
        reason=_text(value.get("reason")),
        extra=_freeze({key: item for key, item in value.items() if key not in known}),
    )


def _provider(name: str, value: Any) -> ProviderCapabilities:
    if not isinstance(value, Mapping):
        return ProviderCapabilities(name, "unknown", (), _freeze({"value": value}))
    entries_value = value.get("entries")
    entries = ()
    if isinstance(entries_value, Mapping):
        entries = tuple(_entry(str(key), item) for key, item in entries_value.items())
    known = {"provider", "runtime", "entries"}
    return ProviderCapabilities(
        provider=_text(value.get("provider"), name),
        runtime=_text(value.get("runtime"), "unknown"),
        entries=entries,
        extra=_freeze({key: item for key, item in value.items() if key not in known}),
    )


def capability_view_from_dict(value: Any) -> CapabilityView:
    """Parse a wire mapping without discarding future keys or state values."""

    if value is None:
        return CapabilityView.empty()
    if not isinstance(value, Mapping):
        raise TypeError("capabilities must be an object")
    schema = value.get("schema_version", 1)
    if not isinstance(schema, int) or isinstance(schema, bool) or schema < 1:
        raise ValueError("capabilities.schema_version must be a positive integer")
    runtime = value.get("extension_runtime")
    if not isinstance(runtime, Mapping):
        runtime = {}
    providers_value = value.get("providers")
    providers = ()
    if isinstance(providers_value, Mapping):
        providers = tuple(
            _provider(str(key), item) for key, item in providers_value.items()
        )
    known = {"schema_version", "selected_provider", "extension_runtime", "providers"}
    return CapabilityView(
        schema_version=schema,
        selected_provider=_text(value.get("selected_provider"), "unknown"),
        extension_enabled=runtime.get("enabled") is True,
        python=_text(runtime.get("python"), "unknown"),
        providers=providers,
        extra=_freeze({key: item for key, item in value.items() if key not in known}),
    )


__all__ = [
    "CapabilityEntry",
    "CapabilityView",
    "KNOWN_STATES",
    "ProviderCapabilities",
    "capability_view_from_dict",
]
