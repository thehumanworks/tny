Read-only verification of fixes for your earlier b739d967df11861c findings.
Fixed commit 9cf38cfd3e33a9685f59a86a30c98c9c461a39fb (parent3105192).
Inspect with git show, not in-flight integration files. Confirm specifically:
observed/late usage under execution/cleanup exceptions and cancellation; whole
source numeric JSON validation (including omitted/overwritten numeric literals);
Python traceback/prompt release on failures; multibyte and mixed selection bounds.
Check that partial accounting semantics and artifact capability docs remain honest.
Reproduce old probes/new tests offline as useful, no live providers. Return
remaining concrete defects or explicit resolution of each finding, with commands
and outcomes. No edits, delegation, commits, publication or issue closure.
