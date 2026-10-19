import dataclasses
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from tny_ext.capabilities import capability_view_from_dict
from tny_ext.development import check_extensions, extension_catalog
from tny_ext.events import event_from_dict, event_types

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


class CatalogTests(unittest.TestCase):
    def test_catalog_matches_real_types_without_alias_duplicates(self):
        catalog = extension_catalog()
        self.assertEqual(catalog["schema_version"], 1)
        names = [item["name"] for item in catalog["events"]]
        self.assertEqual(names, sorted(set(names)))
        self.assertNotIn("permission", names)
        self.assertIn("permission_request", names)
        self.assertIn("custom_message", names)
        self.assertIn("tool_progress", names)
        self.assertEqual(names, [cls.EVENT_NAME for cls in event_types()])
        common = {item["name"] for item in catalog["envelope"]}
        for item in catalog["events"]:
            value = event_from_dict({"type": item["name"]})
            self.assertEqual(type(value).__name__, item["python_type"])
            self.assertEqual(
                {field["name"] for field in item["fields"]} | common,
                {field.name for field in dataclasses.fields(value)},
            )
        actions = {item["kind"]: item for item in catalog["actions"]}
        self.assertEqual(len(actions), 11)
        self.assertEqual(actions["continue"]["python_type"], "ContinueAction")
        self.assertEqual(actions["none"]["fields"], [])
        self.assertIn(
            {"name": "arguments", "type": "Mapping[str, Any]"},
            actions["tool_rewrite"]["fields"],
        )


class CheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def extension(self, text, name="plugin.py"):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_uses_runtime_loader_with_async_setup_and_no_handler_execution(self):
        path = self.extension(
            "async def setup(api):\n"
            "    @api.on('agent_end')\n"
            "    async def handle(event):\n"
            "        raise AssertionError('must not invoke handlers')\n"
        )
        report = check_extensions([path, str(FIXTURES / "package_extension")])
        self.assertTrue(report.ok)
        self.assertEqual(report.diagnostics, ())
        self.assertEqual(
            [extension.events for extension in report.extensions],
            [("agent_end",), ("session_start",)],
        )
        self.assertEqual(report.extensions[1].name, "package_extension")

    def test_bad_setup_does_not_suppress_later_entries(self):
        paths = [
            self.extension("value = 1\n", "missing_setup.py"),
            self.extension(
                "def setup(api):\n    raise SystemExit('exit setup')\n", "exits.py"
            ),
            self.extension("def setup(:\n", "syntax.py"),
            str(self.root / "missing.py"),
            str(FIXTURES / "package_extension" / "index.py"),
        ]
        report = check_extensions(paths)
        self.assertFalse(report.ok)
        self.assertEqual(len(report.extensions), 1)
        self.assertEqual([item.code for item in report.diagnostics], ["load_error"] * 4)
        self.assertEqual(report.diagnostics[1].message, "exit setup")
        self.assertEqual(report.extensions[0].name, "package_extension")

    def test_signatures_checked_without_invoking_and_future_events_only_warn(self):
        path = self.extension(
            "class Opaque:\n"
            "    __signature__ = 'unavailable'\n"
            "    def __call__(self, event):\n"
            "        raise AssertionError('must not invoke')\n"
            "def setup(api):\n"
            "    api.on('agent_end', lambda: None)\n"
            "    api.on('agent_end', lambda event, required: None)\n"
            "    api.on('agent_end', lambda *, event: None)\n"
            "    api.on('agent_end', lambda event, optional=None: None)\n"
            "    api.on('future_event', lambda event: None)\n"
            "    api.on('*', lambda *args: None)\n"
            "    api.on('permission', lambda event: None)\n"
            "    api.on('status', Opaque())\n"
        )
        report = check_extensions([path])
        self.assertFalse(report.ok)
        self.assertEqual(report.extensions[0].handlers, 8)
        self.assertEqual(
            [item.code for item in report.diagnostics],
            ["invalid_handler_signature"] * 3 + ["unknown_event", "unknown_signature"],
        )
        self.assertEqual(report.diagnostics[3].handler_index, 4)
        self.assertEqual(report.diagnostics[3].severity, "warning")
        self.assertEqual(report.diagnostics[3].event, "future_event")

    def test_future_event_and_no_handlers_are_forward_compatible_warnings(self):
        path = self.extension(
            "def setup(api):\n    api.on('future_event', lambda event: None)\n"
        )
        empty = self.extension("def setup(api):\n    pass\n", "empty.py")
        report = check_extensions([path, empty])
        self.assertTrue(report.ok)
        self.assertEqual(
            [item.code for item in report.diagnostics], ["unknown_event", "no_handlers"]
        )
        self.assertEqual(report.to_dict()["ok"], True)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            report.extensions = ()

    def test_capability_snapshot_is_supplied_to_same_setup(self):
        path = self.extension(
            "def setup(api):\n"
            "    if api.capabilities.selected_provider == 'openai':\n"
            "        api.on('status', lambda event: None)\n"
        )
        self.assertEqual(check_extensions([path]).extensions[0].handlers, 0)
        capabilities = capability_view_from_dict({"selected_provider": "openai"})
        report = check_extensions([path], capabilities=capabilities)
        self.assertEqual(report.extensions[0].handlers, 1)
        self.assertTrue(report.ok)

    def test_relative_paths_resolve_against_explicit_cwd(self):
        self.extension("def setup(api):\n    api.on('*', lambda event: None)\n")
        report = check_extensions(["plugin.py"], cwd=str(self.root))
        self.assertTrue(report.ok)
        self.assertEqual(report.extensions[0].entry, str(self.root / "plugin.py"))

    def test_repeated_checks_do_not_reuse_another_packages_relative_imports(self):
        for parent, event in (("one", "agent_end"), ("two", "agent_start")):
            package = self.root / parent / "same_name"
            package.mkdir(parents=True)
            (package / "helper.py").write_text("EVENT = %r\n" % event, encoding="utf-8")
            (package / "index.py").write_text(
                "from .helper import EVENT\n"
                "def setup(api):\n"
                "    api.on(EVENT, lambda event: None)\n",
                encoding="utf-8",
            )
            report = check_extensions([str(package)])
            self.assertTrue(report.ok)
            self.assertEqual(report.extensions[0].events, (event,))


class DevelopmentCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.env = dict(
            os.environ,
            HOME=str(self.root),
            PYTHONPATH=str(ROOT / "python"),
            PYTHONDONTWRITEBYTECODE="1",
        )
        # Neither checkout-local nor global entries may run implicitly.
        for relative in (".tny/extensions", "extensions"):
            extension_dir = self.root / relative
            extension_dir.mkdir(parents=True)
            (extension_dir / "surprise.py").write_text(
                "from pathlib import Path\n"
                "Path('unexpected-execution').touch()\n"
                "raise RuntimeError('must not discover me')\n",
                encoding="utf-8",
            )

    def run_cli(self, *args):
        result = subprocess.run(
            [sys.executable, "-m", "tny_ext", *args],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertFalse((self.root / "unexpected-execution").exists())
        return result

    def test_help_and_catalog_do_not_load_extensions(self):
        for args in [("--help",), ("check", "--help"), ("catalog",)]:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cli("catalog", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), extension_catalog())
        self.assertEqual(result.stderr, "")

    def test_installed_python_layout_supports_catalog_and_runtime_host(self):
        library = self.root / "prefix" / "lib" / "tny"
        shutil.copytree(ROOT / "python" / "tny_ext", library / "tny_ext")
        shutil.copy2(ROOT / "python" / "tny_extension_host.py", library)
        self.env["PYTHONPATH"] = str(library)
        result = self.run_cli("catalog", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), extension_catalog())
        result = self.run_cli("check", str(FIXTURES / "package_extension"), "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["ok"])
        result = subprocess.run(
            [sys.executable, str(library / "tny_extension_host.py")],
            input=json.dumps(
                {
                    "id": 1,
                    "op": "initialize",
                    "entries": [str(FIXTURES / "package_extension")],
                }
            )
            + "\n",
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        response = json.loads(result.stdout)
        self.assertTrue(response["ok"])
        self.assertEqual(response["load_errors"], [])
        self.assertEqual(response["extensions"][0]["name"], "package_extension")

    def test_check_requires_explicit_paths(self):
        result = self.run_cli("check", "--json")
        self.assertEqual(result.returncode, 2)
        self.assertIn("required", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_check_json_is_one_report_with_setup_noise_on_stderr(self):
        result = self.run_cli(
            "check", str(FIXTURES / "lifecycle_extension.py"), "--json"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["ok"])
        self.assertEqual(report["extensions"][0]["handlers"], 6)
        self.assertEqual(report["diagnostics"][0]["code"], "unknown_event")
        self.assertIn("full user permissions", result.stderr)
        self.assertIn("fixture setup noise", result.stderr)
        self.assertNotIn("fixture handler noise", result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 1)

    def test_broken_setup_returns_nonzero_json_and_keeps_valid_package(self):
        result = self.run_cli(
            "check",
            str(FIXTURES / "broken_setup.py"),
            str(FIXTURES / "package_extension"),
            "--json",
        )
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertFalse(report["ok"])
        self.assertEqual(report["diagnostics"][0]["code"], "load_error")
        self.assertEqual(report["extensions"][0]["name"], "package_extension")

    def test_doctor_capabilities_and_invalid_input(self):
        snapshot = self.root / "doctor.json"
        snapshot.write_text(
            json.dumps(
                {
                    "extensions": {
                        "capabilities": {
                            "schema_version": 1,
                            "selected_provider": "openai",
                            "providers": {},
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        plugin = self.root / "selected.py"
        plugin.write_text(
            "def setup(api):\n"
            "    assert api.capabilities.selected_provider == 'openai'\n"
            "    api.on('status', lambda event: None)\n",
            encoding="utf-8",
        )
        result = self.run_cli(
            "check", str(plugin), "--capabilities", str(snapshot), "--json"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["ok"])
        for value in (
            [],
            {},
            {"extensions": None},
            {"schema_version": 2},
            {"schema_version": True},
            {"schema_version": "1"},
            "bad JSON",
        ):
            snapshot.write_text(json.dumps(value), encoding="utf-8")
            result = self.run_cli("check", str(plugin), "--capabilities", str(snapshot))
            self.assertEqual(result.returncode, 2)
            self.assertIn("cannot read capabilities", result.stderr)


if __name__ == "__main__":
    unittest.main()
