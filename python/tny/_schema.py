"""Input schemas from Python signatures, and argument binding.

When pydantic is importable, the signature becomes a pydantic model, which
both publishes the JSON Schema and validates arguments (FastMCP's approach).
Without pydantic, a small built-in mapper covers the common annotations and
checks JSON types; the published schema has the same shape.
"""

from __future__ import annotations

import dataclasses
import datetime as _dt
import enum
import inspect
import pathlib
import re
import types
import typing
import uuid
from typing import Any, Callable

from tny._registry import Param

try:  # pydantic is optional inside and outside the host
    import pydantic  # type: ignore[import-not-found,unused-ignore]
except Exception:  # pragma: no cover - depends on the environment
    pydantic = None  # type: ignore[assignment,unused-ignore]

_SECTION_RE = re.compile(
    r"^\s*(Args|Arguments|Parameters|Params|Keyword Args|Returns|Return|Yields|Raises|"
    r"Examples?|Notes?|See Also|Attributes)\s*:?\s*$"
)
_UNDERLINE_RE = re.compile(r"^\s*-{3,}\s*$")
_GOOGLE_ARG_RE = re.compile(
    r"^\s*\*{0,2}([A-Za-z_][A-Za-z0-9_]*)\s*(\([^)]*\))?\s*:\s*(.*)$"
)
_SPHINX_RE = re.compile(
    r"^\s*:param\s+(?:[^:]*\s)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$"
)


def parse_docstring(doc: str | None) -> tuple[str | None, dict[str, str]]:
    """(summary, {param: description}) from Google, NumPy or Sphinx style."""
    if not doc:
        return None, {}
    lines = inspect.cleandoc(doc).splitlines()
    summary: list[str] = []
    params: dict[str, str] = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if _SECTION_RE.match(line) or _SPHINX_RE.match(line):
            break
        if i + 1 < len(lines) and _UNDERLINE_RE.match(lines[i + 1]) and line.strip():
            break
        summary.append(line)
        i += 1
    current: str | None = None
    section: str | None = None
    numpy = False
    while i < len(lines):
        line = lines[i]
        m = _SPHINX_RE.match(line)
        if m:
            current = m.group(1)
            params[current] = m.group(2).strip()
            section = "sphinx"
            i += 1
            continue
        header = _SECTION_RE.match(line)
        if header or (
            i + 1 < len(lines) and _UNDERLINE_RE.match(lines[i + 1]) and line.strip()
        ):
            name = (header.group(1) if header else line.strip()).lower()
            section = (
                "args"
                if name in ("args", "arguments", "parameters", "params", "keyword args")
                else "other"
            )
            numpy = bool(i + 1 < len(lines) and _UNDERLINE_RE.match(lines[i + 1]))
            current = None
            i += 2 if numpy else 1
            continue
        if section == "args":
            if numpy:
                m2 = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(:.*)?$", line)
                if m2 and not line.startswith((" ", "\t")):
                    current = m2.group(1)
                    params.setdefault(current, "")
                elif current and line.strip():
                    params[current] = (params[current] + " " + line.strip()).strip()
            else:
                m3 = _GOOGLE_ARG_RE.match(line)
                indent = len(line) - len(line.lstrip())
                if m3 and indent <= 4:
                    current = m3.group(1)
                    params[current] = m3.group(3).strip()
                elif current and line.strip():
                    params[current] = (params[current] + " " + line.strip()).strip()
        elif (
            section == "sphinx"
            and current
            and line.strip()
            and line.startswith((" ", "\t"))
        ):
            params[current] = (params[current] + " " + line.strip()).strip()
        i += 1
    text = "\n".join(summary).strip()
    return (text or None), {k: v for k, v in params.items() if v}


def _is_typeddict(tp: Any) -> bool:
    return isinstance(tp, type) and issubclass(tp, dict) and hasattr(tp, "__total__")


def _is_pydantic_model(tp: Any) -> bool:
    return (
        isinstance(tp, type)
        and hasattr(tp, "model_json_schema")
        and hasattr(tp, "model_validate")
    )


def _param_meta(extras: tuple[Any, ...]) -> tuple[str | None, dict[str, Any]]:
    description = None
    schema: dict[str, Any] = {}
    for extra in extras:
        if isinstance(extra, str):
            description = extra
        elif isinstance(extra, Param):
            description = extra.description or description
            if extra.examples:
                schema["examples"] = list(extra.examples)
            if extra.json_schema:
                schema.update(extra.json_schema)
        else:  # pydantic FieldInfo and similar carry a description attribute
            desc = getattr(extra, "description", None)
            if isinstance(desc, str):
                description = desc
    return description, schema


def type_schema(tp: Any) -> dict[str, Any]:
    """JSON Schema for one annotation (the built-in mapper)."""
    if tp is inspect.Parameter.empty or tp is Any or tp is object:
        return {}
    origin = typing.get_origin(tp)
    args = typing.get_args(tp)
    if origin is typing.Annotated:
        description, extra = _param_meta(args[1:])
        annotated: dict[str, Any] = dict(type_schema(args[0]))
        if description:
            annotated["description"] = description
        annotated.update(extra)
        return annotated
    if tp is None or tp is type(None):
        return {"type": "null"}
    if tp is bool:
        return {"type": "boolean"}
    if tp is int:
        return {"type": "integer"}
    if tp is float:
        return {"type": "number"}
    if tp is str:
        return {"type": "string"}
    if tp is bytes:
        return {"type": "string", "contentEncoding": "base64"}
    if tp is _dt.datetime:
        return {"type": "string", "format": "date-time"}
    if tp is _dt.date:
        return {"type": "string", "format": "date"}
    if tp is _dt.time:
        return {"type": "string", "format": "time"}
    if tp is uuid.UUID:
        return {"type": "string", "format": "uuid"}
    if isinstance(tp, type) and issubclass(tp, pathlib.PurePath):
        return {"type": "string", "format": "path"}
    if origin is typing.Literal:
        values = list(args)
        out: dict[str, Any] = {"enum": values}
        kinds = {type(v) for v in values}
        if len(kinds) == 1:
            kind = kinds.pop()
            mapped = {
                str: "string",
                int: "integer",
                float: "number",
                bool: "boolean",
            }.get(kind)
            if mapped:
                out["type"] = mapped
        return out
    if isinstance(tp, type) and issubclass(tp, enum.Enum):
        values = [m.value for m in tp]
        out = {"enum": values}
        if all(isinstance(v, str) for v in values):
            out["type"] = "string"
        elif all(isinstance(v, int) and not isinstance(v, bool) for v in values):
            out["type"] = "integer"
        return out
    if origin in (typing.Union, types.UnionType):
        members = [a for a in args if a is not type(None)]
        nullable = len(members) != len(args)
        if len(members) == 1:
            inner = type_schema(members[0])
            return {"anyOf": [inner, {"type": "null"}]} if nullable else inner
        options = [type_schema(a) for a in members]
        if nullable:
            options.append({"type": "null"})
        return {"anyOf": options}
    if origin in (list, set, frozenset, typing.Sequence) or tp in (
        list,
        set,
        frozenset,
    ):
        out = {"type": "array"}
        if args:
            out["items"] = type_schema(args[0])
        if origin in (set, frozenset) or tp in (set, frozenset):
            out["uniqueItems"] = True
        return out
    if origin is tuple or tp is tuple:
        if args and len(args) == 2 and args[1] is Ellipsis:
            return {"type": "array", "items": type_schema(args[0])}
        if args:
            return {
                "type": "array",
                "prefixItems": [type_schema(a) for a in args],
                "minItems": len(args),
                "maxItems": len(args),
            }
        return {"type": "array"}
    if origin in (dict, typing.Mapping) or tp is dict:
        out = {"type": "object"}
        if len(args) == 2:
            out["additionalProperties"] = type_schema(args[1])
        return out
    if _is_pydantic_model(tp):
        return _inline_refs(tp.model_json_schema())
    if dataclasses.is_dataclass(tp) and isinstance(tp, type):
        return _object_schema(_dataclass_fields(tp))
    if _is_typeddict(tp):
        hints = typing.get_type_hints(tp, include_extras=True)
        required = getattr(
            tp, "__required_keys__", set(hints) if tp.__total__ else set()
        )
        props = {k: type_schema(v) for k, v in hints.items()}
        return {
            "type": "object",
            "properties": props,
            "required": [k for k in hints if k in required],
            "additionalProperties": False,
        }
    return {}


def _dataclass_fields(tp: type) -> list[tuple[str, Any, bool, Any]]:
    hints = typing.get_type_hints(tp, include_extras=True)
    out = []
    for f in dataclasses.fields(tp):
        has_default = (
            f.default is not dataclasses.MISSING
            or f.default_factory is not dataclasses.MISSING
        )
        default = (
            f.default
            if f.default is not dataclasses.MISSING
            else inspect.Parameter.empty
        )
        out.append((f.name, hints.get(f.name, Any), not has_default, default))
    return out


def _object_schema(fields: list[tuple[str, Any, bool, Any]]) -> dict[str, Any]:
    props: dict[str, Any] = {}
    required: list[str] = []
    for name, tp, req, default in fields:
        s = dict(type_schema(tp))
        if default is not inspect.Parameter.empty and _json_safe(default):
            s["default"] = default
        props[name] = s
        if req:
            required.append(name)
    return {
        "type": "object",
        "properties": props,
        "required": required,
        "additionalProperties": False,
    }


def _json_safe(value: Any) -> bool:
    if value is None or isinstance(value, (bool, int, float, str)):
        return True
    if isinstance(value, enum.Enum):
        return _json_safe(value.value)
    if isinstance(value, (list, tuple)):
        return all(_json_safe(v) for v in value)
    if isinstance(value, dict):
        return all(isinstance(k, str) and _json_safe(v) for k, v in value.items())
    return False


def _json_default(value: Any) -> Any:
    return (
        value.value
        if isinstance(value, enum.Enum)
        else list(value)
        if isinstance(value, tuple)
        else value
    )


def _inline_refs(schema: dict[str, Any]) -> dict[str, Any]:
    """Inline local ``$defs`` so every tool schema is self-contained."""
    defs = schema.get("$defs") or schema.get("definitions") or {}

    def resolve(node: Any, seen: tuple[str, ...]) -> Any:
        if isinstance(node, dict):
            ref = node.get("$ref")
            if isinstance(ref, str) and ref.startswith(("#/$defs/", "#/definitions/")):
                key = ref.rsplit("/", 1)[1]
                if key in defs and key not in seen:
                    merged = dict(resolve(defs[key], seen + (key,)))
                    merged.update(
                        {k: resolve(v, seen) for k, v in node.items() if k != "$ref"}
                    )
                    return merged
                return {k: v for k, v in node.items() if k != "$ref"}
            return {
                k: resolve(v, seen)
                for k, v in node.items()
                if k not in ("$defs", "definitions")
            }
        if isinstance(node, list):
            return [resolve(v, seen) for v in node]
        return node

    return typing.cast(dict[str, Any], resolve(schema, ()))


def _strip_titles(node: Any) -> Any:
    """Drop pydantic's generated ``title`` keywords; a property named title stays."""
    if isinstance(node, list):
        return [_strip_titles(v) for v in node]
    if not isinstance(node, dict):
        return node
    out = {}
    for k, v in node.items():
        if k == "title" and isinstance(v, str):
            continue
        if k in ("properties", "patternProperties") and isinstance(v, dict):
            out[k] = {name: _strip_titles(sub) for name, sub in v.items()}
        else:
            out[k] = _strip_titles(v)
    return out


def _hints_one_by_one(fn: Callable[..., Any]) -> dict[str, Any]:
    """Resolve string annotations separately; an unresolvable one is Any."""
    out: dict[str, Any] = {}
    namespace = getattr(fn, "__globals__", {})
    for name, ann in getattr(fn, "__annotations__", {}).items():
        if isinstance(ann, str):
            try:
                ann = eval(ann, namespace)  # noqa: S307 - the extension's own annotation
            except Exception:
                ann = Any
        out[name] = ann
    return out


class Compiled:
    """A tool's published schema plus how to bind JSON arguments to it."""

    def __init__(
        self,
        fn: Callable[..., Any],
        description: str | None,
        schema: dict[str, Any] | None,
    ) -> None:
        self.fn = fn
        target = inspect.unwrap(fn)
        summary, docs = parse_docstring(getattr(target, "__doc__", None))
        self.description = description or summary or ""
        sig = inspect.signature(fn)
        try:
            hints = typing.get_type_hints(target, include_extras=True)
        except Exception:
            hints = _hints_one_by_one(target)
        self.params: list[tuple[str, Any, Any]] = []
        self.var_keyword = False
        for p in sig.parameters.values():
            if p.kind is p.VAR_KEYWORD:
                self.var_keyword = True
                continue
            if p.kind is p.VAR_POSITIONAL:
                continue
            self.params.append((p.name, hints.get(p.name, p.annotation), p.default))
        self.docs = docs
        self.model: Any = None
        self.explicit = schema is not None
        if schema is not None:
            self.schema = schema
        elif pydantic is not None:
            self.schema = self._pydantic_schema()
        else:
            self.schema = self._builtin_schema()

    def _builtin_schema(self) -> dict[str, Any]:
        props: dict[str, Any] = {}
        required: list[str] = []
        for name, tp, default in self.params:
            s = dict(type_schema(tp))
            if name in self.docs and "description" not in s:
                s["description"] = self.docs[name]
            if default is inspect.Parameter.empty:
                required.append(name)
            elif _json_safe(default):
                s["default"] = (
                    _json_default(default)
                    if isinstance(default, (enum.Enum, tuple))
                    else default
                )
            props[name] = s
        out: dict[str, Any] = {
            "type": "object",
            "properties": props,
            "additionalProperties": self.var_keyword,
        }
        if required:
            out["required"] = required
        return out

    def _pydantic_schema(self) -> dict[str, Any]:
        fields: dict[str, Any] = {}
        for name, tp, default in self.params:
            annotation = Any if tp is inspect.Parameter.empty else tp
            info_kwargs: dict[str, Any] = {}
            if name in self.docs:
                info_kwargs["description"] = self.docs[name]
            # Annotated[T, "text"] / tny.Param descriptions become Field metadata.
            if typing.get_origin(annotation) is typing.Annotated:
                base, *extras = typing.get_args(annotation)
                description, extra = _param_meta(tuple(extras))
                if description:
                    info_kwargs["description"] = description
                if extra:
                    info_kwargs["json_schema_extra"] = extra
                kept = [e for e in extras if not isinstance(e, (str, Param))]
                annotation = typing.Annotated[(base, *kept)] if kept else base
            default_value = ... if default is inspect.Parameter.empty else default
            fields[name] = (annotation, pydantic.Field(default_value, **info_kwargs))
        config = pydantic.ConfigDict(
            extra="allow" if self.var_keyword else "forbid",
            arbitrary_types_allowed=True,
        )
        self.model = pydantic.create_model("Arguments", __config__=config, **fields)
        schema: dict[str, Any] = _strip_titles(
            _inline_refs(self.model.model_json_schema())
        )
        schema.setdefault("properties", {})
        schema["additionalProperties"] = self.var_keyword
        return schema

    def bind(self, arguments: dict[str, Any]) -> dict[str, Any]:
        """Validated keyword arguments for ``fn``. Raises ToolError on bad input."""
        from tny._registry import ToolError

        if not isinstance(arguments, dict):
            raise ToolError("arguments must be a JSON object")
        if self.model is not None and not self.explicit:
            try:
                parsed = self.model.model_validate(arguments)
            except pydantic.ValidationError as exc:
                raise ToolError(_pydantic_message(exc)) from None
            kwargs = {name: getattr(parsed, name) for name, _, _ in self.params}
            if self.var_keyword and parsed.model_extra:
                kwargs.update(parsed.model_extra)
            return kwargs
        names = {name for name, _, _ in self.params}
        unknown = [k for k in arguments if k not in names]
        if unknown and not self.var_keyword and not self.explicit:
            raise ToolError(
                "unknown argument"
                + ("s " if len(unknown) > 1 else " ")
                + ", ".join(sorted(unknown))
            )
        kwargs = {}
        for name, tp, default in self.params:
            if name in arguments:
                kwargs[name] = coerce(arguments[name], tp, name)
            elif default is inspect.Parameter.empty:
                raise ToolError(f"missing required argument {name}")
        for k in unknown:
            kwargs[k] = arguments[k]
        return kwargs


def _pydantic_message(exc: Any) -> str:
    parts = []
    for err in exc.errors()[:8]:
        where = ".".join(str(p) for p in err.get("loc", ())) or "arguments"
        parts.append(f"{where}: {err.get('msg', 'invalid')}")
    return "invalid arguments: " + "; ".join(parts)


def _type_name(tp: Any) -> str:
    return getattr(tp, "__name__", None) or str(tp)


def coerce(value: Any, tp: Any, where: str) -> Any:
    """Check one JSON value against an annotation; build enums, dataclasses, paths."""
    from tny._registry import ToolError

    def fail(expected: str) -> ToolError:
        return ToolError(f"{where}: expected {expected}, got {type(value).__name__}")

    if tp is inspect.Parameter.empty or tp is Any or tp is object:
        return value
    origin = typing.get_origin(tp)
    args = typing.get_args(tp)
    if origin is typing.Annotated:
        return coerce(value, args[0], where)
    if origin in (typing.Union, types.UnionType):
        if value is None and type(None) in args:
            return None
        errors = []
        for member in args:
            if member is type(None):
                continue
            try:
                return coerce(value, member, where)
            except ToolError as exc:
                errors.append(str(exc))
        raise ToolError(
            errors[0] if len(errors) == 1 else f"{where}: no union member matched"
        )
    if tp is None or tp is type(None):
        if value is not None:
            raise fail("null")
        return None
    if tp is bool:
        if not isinstance(value, bool):
            raise fail("boolean")
        return value
    if tp is int:
        if isinstance(value, bool) or not isinstance(value, int):
            if isinstance(value, float) and value.is_integer():
                return int(value)
            raise fail("integer")
        return value
    if tp is float:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise fail("number")
        return float(value)
    if tp is str:
        if not isinstance(value, str):
            raise fail("string")
        return value
    if isinstance(tp, type) and issubclass(tp, pathlib.PurePath):
        if not isinstance(value, str):
            raise fail("string path")
        return tp(value)
    if origin is typing.Literal:
        if value not in args:
            raise ToolError(
                f"{where}: expected one of {', '.join(repr(a) for a in args)}"
            )
        return value
    if isinstance(tp, type) and issubclass(tp, enum.Enum):
        try:
            return tp(value)
        except ValueError:
            raise ToolError(
                f"{where}: expected one of {', '.join(repr(m.value) for m in tp)}"
            ) from None
    if origin in (list, set, frozenset, tuple, typing.Sequence) or tp in (
        list,
        set,
        frozenset,
        tuple,
    ):
        if not isinstance(value, list):
            raise fail("array")
        if origin is tuple and args and not (len(args) == 2 and args[1] is Ellipsis):
            if len(value) != len(args):
                raise ToolError(f"{where}: expected {len(args)} items")
            return tuple(
                coerce(v, a, f"{where}[{i}]")
                for i, (v, a) in enumerate(zip(value, args))  # noqa: B905 (lengths checked above)
            )
        inner = args[0] if args else Any
        items = [coerce(v, inner, f"{where}[{i}]") for i, v in enumerate(value)]
        if origin in (set, frozenset) or tp in (set, frozenset):
            return (origin or tp)(items)
        if origin is tuple or tp is tuple:
            return tuple(items)
        return items
    if origin in (dict, typing.Mapping) or tp is dict:
        if not isinstance(value, dict):
            raise fail("object")
        if len(args) == 2:
            return {k: coerce(v, args[1], f"{where}.{k}") for k, v in value.items()}
        return value
    if _is_pydantic_model(tp):
        try:
            return tp.model_validate(value)
        except Exception as exc:  # pydantic.ValidationError
            raise ToolError(f"{where}: {exc}") from None
    if dataclasses.is_dataclass(tp) and isinstance(tp, type):
        if not isinstance(value, dict):
            raise fail("object")
        fields = _dataclass_fields(tp)
        known = {f[0] for f in fields}
        extra = [k for k in value if k not in known]
        if extra:
            raise ToolError(f"{where}: unknown field {extra[0]}")
        kwargs = {}
        for name, ftp, req, _ in fields:
            if name in value:
                kwargs[name] = coerce(value[name], ftp, f"{where}.{name}")
            elif req:
                raise ToolError(f"{where}: missing field {name}")
        return tp(**kwargs)
    if _is_typeddict(tp):
        if not isinstance(value, dict):
            raise fail("object")
        hints = typing.get_type_hints(tp)
        return {
            k: coerce(v, hints.get(k, Any), f"{where}.{k}") for k, v in value.items()
        }
    if tp in (_dt.datetime, _dt.date, _dt.time):
        if not isinstance(value, str):
            raise fail("ISO string")
        try:
            return tp.fromisoformat(value)
        except ValueError:
            raise ToolError(f"{where}: expected an ISO {_type_name(tp)}") from None
    if tp is uuid.UUID:
        try:
            return uuid.UUID(str(value))
        except ValueError:
            raise ToolError(f"{where}: expected a UUID") from None
    return value
