#!/usr/bin/env python3
"""A23: uncertain cleanup never silently frees an output through claim/retry/rm."""

import fcntl
import json
import os
import time
import unittest
from pathlib import Path

from test_jobs import JobsFixture, argv_without_runner_binary

_MISSING = object()


class JobsCleanupHold(JobsFixture):
    hold = 0.1

    def seed(
        self, *, state="failed", cleanup="unknown", code="JOB_IO_FAILED", hold=True
    ):
        output = self.workspace / "held.png"
        _, accepted = self.submit(
            "image", "--output-file", str(output), "--prompt", "FAIL hold seed"
        )
        final = self.await_terminal(accepted["id"])
        self.assertEqual(final["state"], "failed", final)
        directory = Path(final["metadata_path"]).parent
        with (directory / "owner.lock").open("r+b") as owner:
            deadline = time.monotonic() + 10
            while True:
                try:
                    fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    fcntl.flock(owner, fcntl.LOCK_UN)
                    break
                except BlockingIOError:
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
        record_path = directory / "job.json"
        record = json.loads(record_path.read_text())
        record.update(state=state, cleanup=cleanup, error_code=code)
        record["items"][0]["state"] = state
        if hold is _MISSING:
            record.pop("cleanup_hold", None)
        else:
            record["cleanup_hold"] = hold
        record_path.write_text(json.dumps(record))
        claim_path = next((self.jobs_root() / "reservations").glob("*.lock"))
        claim_path.write_text(
            json.dumps(
                {
                    "version": 1,
                    "claims": [
                        {
                            "path": os.path.realpath(output),
                            "job": accepted["id"],
                            "item": 0,
                            "attempt": 1,
                        }
                    ],
                }
            )
        )
        return accepted["id"], output, record_path, claim_path

    def denied_unchanged(self, seeded):
        job, output, record, claim = seeded
        before_record, before_claim = record.read_bytes(), claim.read_bytes()
        requests = len(self.image_requests())
        contender, _ = self.submit(
            "image",
            "--output-file",
            str(output),
            "--prompt",
            "must not spend",
            check=False,
        )
        self.assertNotEqual(contender.returncode, 0, contender.stdout)
        for args in [
            ("jobs", "retry", job, "--failed", "--json"),
            ("jobs", "rm", job, "--json"),
        ]:
            result = self.run_tny(*args, check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(record.read_bytes(), before_record)
            self.assertEqual(claim.read_bytes(), before_claim)
        self.assertEqual(len(self.image_requests()), requests)
        self.assertFalse(output.exists())

    def test_observed_unknown_refuses_post_exit_contender_retry_and_rm(self):
        self.denied_unchanged(self.seed())

    def test_root_wait_loss_is_not_an_owner_loss_projection(self):
        self.denied_unchanged(
            self.seed(state="interrupted", code="JOB_IO_FAILED", hold=False)
        )

    def test_missing_latch_does_not_authorize_legacy_observed_unknown(self):
        self.denied_unchanged(self.seed(hold=_MISSING))

    def test_true_latch_survives_a_canonical_projection_tuple(self):
        self.denied_unchanged(
            self.seed(state="interrupted", code="JOB_INTERRUPTED", hold=True)
        )

    def test_true_latch_denies_even_a_complete_record(self):
        self.denied_unchanged(self.seed(cleanup="complete", hold=True))

    def test_malformed_latches_deny(self):
        seeded = self.seed()
        for value in [None, 0, "false", [], {}]:
            with self.subTest(value=value):
                record = json.loads(seeded[2].read_text())
                record["cleanup_hold"] = value
                seeded[2].write_text(json.dumps(record))
                self.denied_unchanged(seeded)

    def test_projection_strings_must_match_their_full_json_length(self):
        seeded = self.seed(state="interrupted", code="JOB_INTERRUPTED", hold=False)
        original = json.loads(seeded[2].read_text())
        for key in ["state", "cleanup", "error_code"]:
            with self.subTest(key=key):
                record = dict(original)
                record[key] += "\0suffix"
                seeded[2].write_text(json.dumps(record))
                self.denied_unchanged(seeded)

    def test_pending_state_nul_cannot_be_normalized_into_a_projection(self):
        self.denied_unchanged(
            self.seed(state="running\0suffix", cleanup="pending", code=None, hold=False)
        )

    def test_malformed_pending_cleanup_cannot_be_normalized_into_a_projection(self):
        seeded = self.seed(state="running", cleanup="pending", code=None, hold=False)
        original = json.loads(seeded[2].read_text())
        for cleanup in [
            "pending\0suffix",
            "unknown",
            "complete",
            "unexpected",
            None,
            0,
        ]:
            with self.subTest(cleanup=cleanup):
                record = dict(original)
                record["cleanup"] = cleanup
                seeded[2].write_text(json.dumps(record))
                self.denied_unchanged(seeded)

    def test_complete_cleanup_releases_the_output(self):
        _job, output, _record, _claim = self.seed(cleanup="complete", hold=False)
        _, accepted = self.submit(
            "image", "--output-file", str(output), "--prompt", "new clean owner"
        )
        final = self.await_terminal(accepted["id"])
        self.assertEqual(final["cleanup"], "complete", final)
        self.assertTrue(output.exists())

    def test_exact_a11_projection_reclaims_after_actual_owner_loss(self):
        _job, output, record, _claim = self.seed(
            state="interrupted", code="JOB_INTERRUPTED", hold=_MISSING
        )
        before = record.read_bytes()
        _, accepted = self.submit(
            "image", "--output-file", str(output), "--prompt", "new projected owner"
        )
        self.await_terminal(accepted["id"])
        self.assertTrue(output.exists())
        self.assertEqual(record.read_bytes(), before)

    def test_abandoned_pending_a11_claim_remains_reclaimable(self):
        _job, output, _record, _claim = self.seed(
            state="queued", cleanup="pending", code=None, hold=False
        )
        _, accepted = self.submit(
            "image", "--output-file", str(output), "--prompt", "new abandoned owner"
        )
        self.await_terminal(accepted["id"])
        self.assertTrue(output.exists())

    def test_state_lock_uncertainty_denies_without_waiting(self):
        seeded = self.seed(cleanup="complete", hold=False)
        owner = self.lock_held(seeded[2].parent / "state.lock")
        try:
            started = time.monotonic()
            result, _ = self.submit(
                "image",
                "--output-file",
                str(seeded[1]),
                "--prompt",
                "blocked",
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertLess(time.monotonic() - started, 1.0)
        finally:
            owner.close()


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
