**APPROVE for integration/reuse of the A15 captured-image queue prerequisite.** No actionable defects found in the focused current-source inspection.

| Work completed | Checks and results | Blockers |
|---|---|---|
| Read A15 and applicable #126 requirements against the original issue snapshot. | Approval is limited to the queue, shared image builder, receiver and reusable control primitive. | None identified in this slice. |
| Inspected capture and control paths. | Local and SSH admissions retain bytes. Opened-file reads are bounded. Flush does not reread paths. Full decoded control strings and reply correlation are validated. Admission requires a continuable owning tool batch. | — |
| Inspected failure disposition and relevant test source. | Mixed preview failures preserve the batch before owner cleanup. Pending entries and constructed-but-unsent transcript messages have terminal disposition. Inspected hard cancellation, persistence failure, extension stops, denial, step exhaustion and next-turn recovery. | Tests were **not run**, as requested. |

This does **not** approve full generation-preview integration or complete #126. Generation CLI/tool surfaces, derived preview sizing and artifact lineage remain outside this checkpoint. Queue acceptance is not evidence of visual perception.

No edits, test execution, children, goals or commits were performed.

### Inspected implementation and test hashes

SHA-256; paths are relative to the current workspace.

```text
e82cb10be7f48cd3d982520d1ee81b2e3c4d9efd0558a08a763b26318f3c46c1  src/core/image.c
f40565d5dd4be2112c5df6fbc4cc5fab33b57ab46d1071cc737dd422fcb4c2ae  src/core/image_preview.c
a89cb85bff068b28aa810c6b48e1b8b9c0bddbb30688e5e99afbefcaaba7fb65  src/core/tools.c
aa512443db806e753209469133bbd053da3907f77eb865571a27c41b4944353a  src/core/tools_ssh.c
13407be64dc1aca2944645d489b5877783f47aa509a8e9ea9bd6c6c7ef8a0038  src/core/runner.c
5155dc383591654f4f4fac6350a5c2604546b36056cd20ce7effa0006a54fb60  src/core/runtime.c
eb9089b532bb289543bde52de6099aaa9c93101c4061067fff5e259686791174  src/backends/openai/openai.c
d682ad1c11025c2cdd2e645dcbd03c94dd8f6b3b8106e5898ce03eee6e6f1ec6  src/cli/cmd_control.c
90ce2962cc79ed9d28b7575c7fb35aa5ba683840269625f4fb996ee68e129bff  tests/test_core.c
8c13dab48883fe77c1cf0fbcfc463796d2e4681db65d2bbf6bb353cdf247577b  tests/test_openai.c
9c48f458dd94505a613aa76a879b2cb602d869b14b5749840cc2b45ba52f5567  tests/test_runner.c
a0a5405989fe857ec9a5719925a93a9b33cbcfa8c3a7fef342f8d728a34ad34a  tests/test_runtime.c
9327670f39f8c1fd33e2d9e1313bb4d4baf5c3afdd9bb4d8e6990ea1de3c626d  tests/integration/test_image_preview_queue.py
```
