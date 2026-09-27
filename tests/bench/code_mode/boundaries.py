"""Candidate parity probes; not additional model trials or model-failure scores."""

from __future__ import annotations

import json

from cases import LANGUAGES
from execute import BUILD, run_program
from run import atomic_json


def main() -> None:
    probes = {
        "signed_integer_json_roundtrip": {
            "lua": 'print(json.encode(json.decode("{\\"n\\":9007199254740993}")))',
            "javascript": 'print(JSON.stringify(JSON.parse("{\\"n\\":9007199254740993}")));',
            "python": 'print(json.dumps(json.loads("{\\"n\\":9007199254740993}")))',
        },
        "reject_20_mib_cell_allocation": {
            "lua": 'local x=string.rep("x",20*1024*1024); print(#x)',
            "javascript": 'const x="x".repeat(20*1024*1024); print(x.length);',
            "python": 'x="x"*(20*1024*1024)\nprint(len(x))',
        },
        "reject_output_over_64_kib": {
            "lua": 'print(string.rep("x",70000))',
            "javascript": 'print("x".repeat(70000));',
            "python": 'print("x"*70000)',
        },
    }
    results = {}
    for name, programs in probes.items():
        results[name] = {}
        for language in LANGUAGES:
            observed = run_program(language, programs[language], {"files": {}})
            if name == "signed_integer_json_roundtrip":
                try:
                    meets = (
                        observed["runtime_ok"] is True
                        and json.loads(observed["stdout"])["n"] == 9007199254740993
                    )
                except (ValueError, KeyError, TypeError):
                    meets = False
            else:
                meets = observed["runtime_ok"] is False
            results[name][language] = {
                "meets_probed_requirement": meets,
                "observed": observed,
            }
    report = {
        "results": results,
        "limits": "Diagnostic probes of these exact adapters only. Not a general impossibility claim about any language and not a full sandbox proof. Never mixed into the 108 model trials.",
    }
    atomic_json(BUILD / "boundaries.json", report)
    print(
        json.dumps(
            {
                name: {lang: r["meets_probed_requirement"] for lang, r in arms.items()}
                for name, arms in results.items()
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
