"""Stable Python exception mapping for libtny status categories."""
from __future__ import annotations

STATUS_NAMES = {
    -1: "invalid_argument",
    -2: "bad_state",
    -3: "busy",
    -4: "out_of_memory",
    -5: "configuration",
    -6: "authentication",
    -7: "io",
    -8: "timeout",
    -9: "unsupported",
    -10: "protocol",
    -11: "backpressure",
    -12: "cancelled",
    -13: "internal",
}


class TnyError(RuntimeError):
    """A synchronous libtny failure.

    Native diagnostic bytes are available via ``message`` but deliberately do
    not enter ``str`` or ``repr``. This keeps credentials and provider payloads
    out of logs and tracebacks even if a future native diagnostic regresses.
    """

    def __init__(self, code: int, message: bytes = b"") -> None:
        self.code = int(code)
        self.message = bytes(message)
        super().__init__(self.code)

    @property
    def category(self) -> str:
        return STATUS_NAMES.get(self.code, "unknown")

    def message_text(self, *, errors: str = "strict") -> str:
        """Decode native UTF-8 explicitly; strict decoding is the default."""
        return self.message.decode("utf-8", errors=errors)

    def __str__(self) -> str:
        return f"tny operation failed ({self.category}, code {self.code})"

    def __repr__(self) -> str:
        return f"{type(self).__name__}(code={self.code})"


class InvalidArgumentError(TnyError): pass
class BadStateError(TnyError): pass
class BusyError(TnyError): pass
class OutOfMemoryError(TnyError): pass
class ConfigurationError(TnyError): pass
class AuthenticationError(TnyError): pass
class IOError(TnyError): pass
class TimeoutError(TnyError): pass
class UnsupportedError(TnyError): pass
class ProtocolError(TnyError): pass
class BackpressureError(TnyError): pass
class CancelledError(TnyError): pass
class InternalError(TnyError): pass

# Unambiguous aliases for the two names that also exist in builtins.
TnyIOError = IOError
TnyTimeoutError = TimeoutError


ERROR_TYPES: dict[int, type[TnyError]] = {
    -1: InvalidArgumentError,
    -2: BadStateError,
    -3: BusyError,
    -4: OutOfMemoryError,
    -5: ConfigurationError,
    -6: AuthenticationError,
    -7: IOError,
    -8: TimeoutError,
    -9: UnsupportedError,
    -10: ProtocolError,
    -11: BackpressureError,
    -12: CancelledError,
    -13: InternalError,
}


def error_from_status(code: int, message: bytes = b"") -> TnyError:
    return ERROR_TYPES.get(code, TnyError)(code, message)
