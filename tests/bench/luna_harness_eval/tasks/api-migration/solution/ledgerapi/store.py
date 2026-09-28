"""Versioned record resolution with explicit missing states."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Record:
    scope: str
    identifier: str
    value: object
    revision: int
    expires_at: int | None = None
    deleted: bool = False


@dataclass(frozen=True)
class Resolution:
    found: bool
    value: object = None
    revision: int | None = None
    reason: str = "absent"


class RecordStore:
    def __init__(self, records=()):
        self.records = list(records)

    def resolve(
        self, scope: str, identifier: str, *, as_of: int | None = None
    ) -> Resolution:
        """Resolve one key at a revision; tombstones and expiry mask older values."""
        if as_of is not None and as_of < 0:
            raise ValueError("as_of must be nonnegative")
        matching = [
            record
            for record in self.records
            if record.scope == scope
            and record.identifier == identifier
            and (as_of is None or record.revision <= as_of)
        ]
        if not matching:
            return Resolution(False, reason="absent")
        latest = max(matching, key=lambda record: record.revision)
        cutoff = (
            as_of
            if as_of is not None
            else max(record.revision for record in self.records)
        )
        if latest.deleted:
            return Resolution(False, revision=latest.revision, reason="deleted")
        if latest.expires_at is not None and cutoff >= latest.expires_at:
            return Resolution(False, revision=latest.revision, reason="expired")
        return Resolution(True, latest.value, latest.revision, "found")

    def append(self, record: Record) -> None:
        self.records.append(record)
