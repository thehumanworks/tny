"""Shipping policy access for the tenant control plane.

This module owns the interpretation of the shipping setting. Callers use the
public reader and summary helpers so lookup semantics remain consistent.
"""

DOMAIN = "shipping"
DEFAULT = "enabled"


def setting_key(name: str) -> str:
    """Build the stable key used by old and new persisted records."""
    if not name or ":" in name:
        raise ValueError("setting names must be nonempty and contain no colon")
    return f"{DOMAIN}:{name}"


def read_shipping(
    store, tenant: str, name: str, *, as_of: int | None = None, fallback=DEFAULT
):
    """Read a tenant setting at a revision, preserving explicit null payloads."""
    key = setting_key(name)
    if not tenant:
        raise ValueError("tenant is required")
    result = store.resolve(tenant, key, as_of=as_of)
    return result.value if result.found else fallback


def current(store, tenant: str, name: str, *, fallback=DEFAULT):
    """Read the head revision for a normal request."""
    return read_shipping(store, tenant, name, fallback=fallback)


def snapshot(store, tenant: str, name: str, revision: int, *, fallback=DEFAULT):
    """Read the state that applied to a historical request."""
    return read_shipping(store, tenant, name, as_of=revision, fallback=fallback)


def summarize(store, tenant: str, names: list[str], *, as_of: int | None = None):
    """Assemble a compact, stable policy view for diagnostics."""
    return {name: read_shipping(store, tenant, name, as_of=as_of) for name in names}


def changed(store, tenant: str, name: str, left: int, right: int) -> bool:
    """Tell audit tooling whether the visible payload changed."""
    earlier = snapshot(store, tenant, name, left)
    later = snapshot(store, tenant, name, right)
    return earlier != later


def select(store, tenant: str, candidates: list[str], *, as_of: int | None = None):
    """Return the first configured candidate and its value."""
    for name in candidates:
        value = read_shipping(store, tenant, name, as_of=as_of, fallback=None)
        if value is not None:
            return name, value
    return None, None


def describe(store, tenant: str, name: str, *, as_of: int | None = None) -> dict:
    """Provide a JSON-friendly view for support tooling."""
    return {
        "domain": DOMAIN,
        "name": name,
        "value": read_shipping(store, tenant, name, as_of=as_of),
    }


def audit_diff(store, tenant: str, names: list[str], left: int, right: int) -> dict:
    """Return before-and-after values for settings that changed."""
    differences = {}
    for name in names:
        before = snapshot(store, tenant, name, left)
        after = snapshot(store, tenant, name, right)
        if before != after:
            differences[name] = {"before": before, "after": after}
    return differences


def first_override(store, tenant: str, names: list[str], *, as_of: int | None = None):
    """Pick the first setting that differs from the domain default."""
    for name in names:
        value = read_shipping(store, tenant, name, as_of=as_of)
        if value != DEFAULT:
            return name, value
    return None, DEFAULT


def export_view(
    store, tenant: str, names: list[str], *, as_of: int | None = None
) -> list[dict]:
    """Build a deterministic ordered view for support exports."""
    rows = []
    for name in sorted(set(names)):
        rows.append(describe(store, tenant, name, as_of=as_of))
    return rows


def has_change(store, tenant: str, names: list[str], left: int, right: int) -> bool:
    """Cheap summary for the audit notification path."""
    return bool(audit_diff(store, tenant, names, left, right))
