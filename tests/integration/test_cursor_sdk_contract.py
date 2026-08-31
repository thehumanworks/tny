#!/usr/bin/env python3
"""Focused tests for the pinned Cursor SDK Bridge sdk.v1 contract."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from check_cursor_sdk_v1 import check  # noqa: E402
from update_cursor_sdk_v1 import (  # noqa: E402
    COVERAGE_REL,
    VENDOR_REL,
    build_contract,
    build_coverage,
)


class CursorSdkContractTest(unittest.TestCase):
    def test_generated_coverage_is_only_an_unreviewed_skeleton(self) -> None:
        skeleton = build_coverage(build_contract(ROOT / VENDOR_REL))
        self.assertFalse(skeleton["reviewed"])
        entries = list(skeleton["rpcs"]) + list(skeleton["messages"])
        entries.extend(
            field for message in skeleton["messages"] for field in message["fields"]
        )
        self.assertTrue(entries)
        self.assertTrue(all(entry["reviewed"] is False for entry in entries))
        self.assertTrue(all(entry["state"] == "needs-review" for entry in entries))
        send = next(
            message
            for message in skeleton["messages"]
            if message["name"] == "SendRequest"
        )
        self.assertEqual(send["wire_roles"], ["outbound-server-stream-request"])

    def test_checked_in_contract_is_current(self) -> None:
        self.assertEqual(check(ROOT), [])
        contract = json.loads(
            (ROOT / VENDOR_REL / "contract.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            {
                key: contract["counts"][key]
                for key in ("services", "rpcs", "messages", "fields")
            },
            {"services": 5, "rpcs": 29, "messages": 114, "fields": 285},
        )

    def _copy_vendor(self, temporary_root: Path) -> Path:
        destination = temporary_root / VENDOR_REL
        destination.parent.mkdir(parents=True)
        shutil.copytree(ROOT / VENDOR_REL, destination)
        inputs = [
            COVERAGE_REL,
            Path("src/backends/cursor/sdk_client.c"),
            Path("src/backends/cursor/callbacks.c"),
            Path("src/backends/cursor/cursor.c"),
            Path("src/backends/cursor/management.c"),
            Path("src/backends/cursor/map.c"),
            Path("src/backends/cursor/options.c"),
            Path("src/backends/cursor/sdk_error.c"),
            Path("src/cli/cmd_cursor.c"),
            Path("tests/integration/test_cursor_management.py"),
            Path("tests/integration/test_mock_bridge_v130.py"),
            Path("tests/test_cursor.c"),
            Path("tests/test_cursor_callbacks.c"),
            Path("tests/test_cursor_management.c"),
            Path("tests/test_cursor_options.c"),
            Path("tests/test_cursor_sdk.c"),
            Path("docs/backends/cursor-bridge.md"),
        ]
        for relative in inputs:
            target = temporary_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        return destination

    def _coverage(self, root: Path) -> tuple[Path, dict[str, object]]:
        path = root / COVERAGE_REL
        return path, json.loads(path.read_text(encoding="utf-8"))

    @staticmethod
    def _write_json(path: Path, value: dict[str, object]) -> None:
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def test_tampered_proto_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            vendor = self._copy_vendor(root)
            proto = vendor / "proto/sdk/v1/sdk_messages.proto"
            proto.write_bytes(proto.read_bytes() + b"\n")
            errors = check(root)
            self.assertEqual(len(errors), 1)
            self.assertIn("expected sha256", errors[0])

    def test_stale_contract_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            vendor = self._copy_vendor(root)
            contract_path = vendor / "contract.json"
            contract = json.loads(contract_path.read_text(encoding="utf-8"))
            contract["counts"]["fields"] -= 1
            contract_path.write_text(
                json.dumps(contract, indent=2) + "\n", encoding="utf-8"
            )
            errors = check(root)
            self.assertEqual(len(errors), 1)
            self.assertIn("stale or tampered", errors[0])

    def test_unexpected_proto_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            vendor = self._copy_vendor(root)
            (vendor / "proto/sdk/v1/unpinned.proto").write_text(
                'syntax = "proto3";\n', encoding="utf-8"
            )
            errors = check(root)
            self.assertEqual(len(errors), 1)
            self.assertIn("unexpected vendored files", errors[0])

    def test_missing_rpc_and_field_coverage_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            path, coverage = self._coverage(root)
            coverage["rpcs"].pop()
            coverage["messages"][0]["fields"].pop()
            self._write_json(path, coverage)
            errors = check(root)
            self.assertTrue(any("RPC set" in error for error in errors))
            self.assertTrue(
                any("fields do not exactly match" in error for error in errors)
            )

    def test_incomplete_state_and_placeholder_strategy_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            path, coverage = self._coverage(root)
            coverage["rpcs"][0]["state"] = "partial"
            coverage["messages"][0]["fields"][0]["strategy"] = "TODO later"
            self._write_json(path, coverage)
            errors = check(root)
            self.assertTrue(any("is not complete" in error for error in errors))
            self.assertTrue(any("incomplete strategy" in error for error in errors))

    def test_every_claim_requires_explicit_review_and_exact_wire_roles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            path, coverage = self._coverage(root)
            coverage["messages"][0]["reviewed"] = False
            coverage["messages"][1]["fields"][0]["reviewed"] = False
            send = next(
                message
                for message in coverage["messages"]
                if message["name"] == "SendRequest"
            )
            send["wire_roles"] = ["outbound-unary-request"]
            self._write_json(path, coverage)
            errors = check(root)
            self.assertTrue(any("explicitly reviewed" in error for error in errors))
            self.assertTrue(
                any("wire roles do not match SendRequest" in error for error in errors)
            )

    def test_stream_and_image_semantic_mappings_are_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            path, coverage = self._coverage(root)
            messages = {message["name"]: message for message in coverage["messages"]}
            messages["SendRequest"]["strategy"] = "bounded unary pass-through"
            messages["SendRequest"]["symbols"] = [
                "src/backends/cursor/sdk_client.c:cursor_sdk_invoke_unary"
            ]
            messages["ObserveRunRequest"]["fields"][0]["strategy"] = (
                "bounded unary pass-through"
            )
            images = next(
                field
                for field in messages["UserMessage"]["fields"]
                if field["name"] == "images"
            )
            images["symbols"] = ["src/backends/cursor/cursor.c:cu_send"]
            self._write_json(path, coverage)
            errors = check(root)
            self.assertTrue(
                any(
                    "server-stream message SendRequest claims unary" in error
                    for error in errors
                )
            )
            self.assertTrue(
                any(
                    "server-stream field ObserveRunRequest" in error for error in errors
                )
            )
            self.assertTrue(
                any(
                    "UserMessage.images lacks native image" in error for error in errors
                )
            )

    def test_stale_symbol_test_and_product_references_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            path, coverage = self._coverage(root)
            coverage["rpcs"][0]["symbols"] = [
                "src/backends/cursor/sdk_client.c:no_such_symbol"
            ]
            coverage["rpcs"][1]["tests"] = ["tests/test_cursor_sdk.c:no_such_test"]
            coverage["rpcs"][2]["product_surfaces"] = [
                "docs/backends/cursor-bridge.md#no-such-section"
            ]
            self._write_json(path, coverage)
            errors = check(root)
            self.assertTrue(any("stale symbol symbol" in error for error in errors))
            self.assertTrue(any("stale test symbol" in error for error in errors))
            self.assertTrue(any("stale product anchor" in error for error in errors))

    def test_c_route_service_method_and_stream_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            route_source = root / "src/backends/cursor/sdk_client.c"
            route_source.write_text(
                route_source.read_text(encoding="utf-8").replace(
                    '"GetUsage", CURSOR_SDK_UNARY',
                    '"GetUsageChanged", CURSOR_SDK_SERVER_STREAM',
                ),
                encoding="utf-8",
            )
            errors = check(root)
            self.assertTrue(
                any("27 outbound contract RPCs" in error for error in errors)
            )

    def test_callback_path_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_vendor(root)
            callback_source = root / "src/backends/cursor/callbacks.c"
            callback_source.write_text(
                callback_source.read_text(encoding="utf-8").replace(
                    "/sdk.v1.SdkStoreCallbackService/CallStore",
                    "/sdk.v1.SdkStoreCallbackService/CallStoreChanged",
                ),
                encoding="utf-8",
            )
            errors = check(root)
            self.assertTrue(
                any("two inbound contract RPCs" in error for error in errors)
            )


if __name__ == "__main__":
    # tests/integration/run.sh supplies the built tny path to every Python
    # integration fixture. This unittest validates checked-in source only.
    sys.argv[1:] = []
    unittest.main()
