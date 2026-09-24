"""The deprecated unversioned record lookup."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Record:
    scope: str
    identifier: str
    value: object
    revision: int
    expires_at: int | None = None
    deleted: bool = False


class RecordStore:
    def __init__(self, records=()):
        self.records = list(records)

    def lookup(self, scope, identifier, default=None):
        """Return the newest payload, or the fallback for a missing record."""
        matching = [
            record
            for record in self.records
            if record.scope == scope and record.identifier == identifier
        ]
        if not matching:
            return default
        latest = max(matching, key=lambda record: record.revision)
        return latest.value

    def append(self, record):
        self.records.append(record)
