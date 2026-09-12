# Exact staging recommendations (not a staging action)

Scope: retained verification evidence, not primary-owned product sources.
All names below are repository-relative, one literal path per line. Hashes and
sizes are in retained-inventory.json. This is a read-time recommendation only.
A path whose bytes changed afterward must be rescanned, not trusted by name.

| List | Recommendation |
| --- | --- |
| stage-text-candidates.txt | 2,946 plain-text files (36,677,317 B), each at most 1 MiB, no scan hit. Include after a hash recheck and primary review of relevance. This retains initial/current contracts, baseline/goal/execution records, reviewed before-images, exact test/mutation scripts, failed and passing raw text logs, and worker/review manifests. It is not proof that all bytes are free of every possible secret format. |
| stage-hold-sensitive.txt | **Do not stage these 13 paths yet.** Two tar archives stay excluded. Eleven fixture/source-copy files have sensitive literal candidates; the token-shaped one is a fixture copy. Validate fake sentinel identity without echoing values before releasing individual paths. Do not redact or rewrite immutable prior evidence here. |
| stage-exclude-scratch.txt | **Exclude these 25 paths.** Includes baseline.bundle, source .tar.gz/.age archives, pycache, compiled probes/dylibs, screenshots, UTF-16 guest/os.txt and one tiny header .bin. This is a conservative binary/scratch rule, not a statement that each is dangerous. Preserve all on disk. The small deterministic header fixture can be separately approved if portable evidence requires it; do not blanket-approve other binaries. |
| stage-reconciliation.txt | Include these new text-only reconciliation outputs after QA/hash recheck. No file exceeds the audit's 1 MiB individual review threshold after inventory compaction. Source/log evidence indexes together are several MiB, not runtime assets. |

Never stage the entire untracked docs/verification directory or entire repository
without exclusions. Do not stage task-private cache `secrets/`, guest VM disks,
source archives, agent-session logs, screenshots, build/, native executables,
private keys, or arbitrary /tmp state. No such external paths are in the include lists.
HANDOFF.md is outside the scan root: primary must review it separately; its content
was read but this list does not implicitly approve future changed bytes.
Product source/tests/SDK/docs/build inputs must use primary's reviewed ownership
manifest and final secret scan, not this evidence-only inventory.

Safe procedure for primary (no commands here were used to modify the index):
1. Read each include list as literal paths, not shell globs. Confirm each expected
   file hash against the inventory and re-scan any changed or newly added file.
2. Resolve held paths individually. Keep real credential material out; if the
   historical environment dump reached other retained locations, report its
   exposure scope without reproducing values and coordinate any required rotation.
3. Stage only approved literal paths with Git's pathspec-from-file mechanism and
   literal pathspec semantics. Review `git diff --cached --stat` and
   `git diff --cached --name-status`; do not print suspected secret-bearing diffs.
4. Verify none of the hold/exclude paths or unrelated work is staged. Re-scan the
   actual staged blobs, not just the worktree. Integration can change file contents.
5. Preserve excluded evidence locally. If the PR needs an excluded archive to
   reproduce a historical snapshot, use a separately approved safe artifact store
   and an integrity pointer, or supply text before-images. No upload is authorized
   or performed by this support assignment.

The 1 MiB review cutoff is this audit's conservative per-file staging heuristic,
not a new repository or product size policy. The Linux executable gate remains
strictly less than 1,048,576 bytes and has not been relaxed.
