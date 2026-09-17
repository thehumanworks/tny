# Independent review disposition

A fresh Fable medium-effort single-pass review approved the production diff,
found no ownership regression, and independently built/reran the source-bound
fixture. Its complete returned report is in independent-review.md. No second
review pass was used to substitute for verification.

1. **Loader leak proof:** the original ASan-only macOS fixture could not catch
   a leaked immutable yyjson document. A separate, non-forking invocation of
   the maintained transaction test was linked without ASan and run under macOS
   `leaks --atExit`. The unmodified source returned0 with0 leaked bytes. The
   same binary construction using only a private `doc.release()` mutant returned1
   with88 leaks/25,408 bytes. The mutation passed its ordinary behavior assertions,
   so the leak oracle specifically detects the missing destruction. Raw report
   hashes, summarized observations and the compiler/source/binary provenance are
   preserved under artifacts/. The full reports remain in the task directory.
2. **Directory-copy allocation failure:** the owning directory copy now calls
   the existing `tny_alloc_strdup` wrapper. The discovery-driven admission sweep
   covers six indices (directory plus JSON), including the previously untested
   NULL directory branch. Cleanup/reuse are unchanged. The combined review additionally makes this
   private directory-OOM path fail fast with ENOMEM and explicit error text;
   the first fault index checks no later JSON allocation occurs. Updated
   fixture and runner mutations verify the integrated implementation.
3. **Unwinding wording:** the scope note explicitly says a fixture exception
   unwinds past the owning transaction; production jobs helpers retain their
   null/status-returning interfaces and do not start throwing exceptions.

Out-of-slice observations about historical commit metadata allocation failures
and parser OOM wording are retained in the report. They are not silently claimed
fixed by this representation-only migration. The concurrent finalization task
received the report, test patch, updated directory allocator line and direct
leak evidence for integration with its separate analyzer/Nix/runtime work.
