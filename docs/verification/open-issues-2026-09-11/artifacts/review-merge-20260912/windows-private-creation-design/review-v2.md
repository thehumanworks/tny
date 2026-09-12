# Final independent Windows private-filesystem design disposition

**APPROVE DESIGN.v2.md**, SHA256 `2e10a81a4eb4edad11517f1e81a97cca853940f76abbc9795eed0d5f6db69233`. Its content and the design-v2 manifest were read directly; the exact hash remained unchanged through this review. No product source writes, tests, new probes, or child agents were used.

The revision closes all six prior design findings:

- A20 confined reads explicitly use retained native component authority, including bounded MSYS mount/drive namespace handling; public inputs do not acquire private-ACL requirements.
- The stage object retains its original directory through writes, converter use, readback and cleanup. The six actual C callers and private header changes are now explicit, including core/image_export.
- Existing Windows private-role ownership, type, hard-link and protected-DACL/control rules are concrete and fail closed. Ordinary ancestors/public inputs and existing POSIX behavior are expressly preserved; no automatic ACL migration is authorized.
- Native, SYSTEM-cookie and read-only/implicit .lnk alias forms and filename escape collisions are included, with no authoritative pathname-resolving conversion.
- Created, existing, persistent-log, linked and renamed commit states have distinct deletion authority. A renamed committed object is only closed, never disposed; later errors preserve the committed artifact.
- The independent rights, allocation/fd failure, filename, parent replacement, postcommit and native-platform matrix is mandatory. The old77-check probe is explicitly not evidence for these new cases.

The later normative stage-pinning clause constrains the earlier general share-read/write/delete guidance. During a pathname-consuming converter operation, the implementation must retain the necessary directory/ancestor identities with sharing restrictions that prevent redirection, or safely refuse before spawn. It may not claim that a one-time pathname identity check establishes a stable view. This is an implementation proof obligation, not an unresolved architectural alternative or an assertion already established by probe-v5.

Approval permits the coordinator to allocate the Windows ADR/amendment and begin the stated host-primitive-first implementation/review sequence. It does not approve unreviewed source, expand the caller scope, waive size/ABI/POSIX/wasm behavior, or substitute ARM-emulated guest evidence for hosted native x64 Windows verification. No concrete design blocker remains.
