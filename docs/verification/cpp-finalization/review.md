# Independent review and disposition

One fresh `claude --model fable --effort medium -p` session reviewed the staged
continuation against fdd5aa7. Production verdict: approved, no confirmed memory,
cleanup-order or boundary regression. The review independently compiled and
killed the prefailed-scope OOM and transaction document/lock reset mutants.
The complete report is retained as an evidence artifact.

1. **Non-unique copy-failure mutation anchor (medium): fixed.** Anchor includes
   the adjacent overflow branch, identifying only queue_event's copy-failure
   site. The provider post-processing check is not mutated accidentally.
2. **Duplicate early-scope mutant with wrong oracle (medium): fixed.** Removed
   the duplicate introduced by overlapping work. The retained mutant invokes
   the prefailed-scope regression; the separate copy-failure mutant invokes its
   own regression. Both must compile and fail their intended behavioral oracle.
3. **Duplicate Nix input/assertion (low): fixed.** Each package source and
   inventory assertion appears once after reconciliation.
4. **Directory-copy OOM error (low): fixed.** Stop immediately, reset the
   transaction, return ENOMEM and "out of memory". The first injected admission
   failure checks the exact code/message and verifies no record allocation
   follows the failure. The real state lock is released without persistence.

The development full suite separately caught CI's missing explicit C++ analyzer
pin; restored gcc-14/g++-14 together, without weakening the pin-alignment test.

Review gaps are verification requirements, not assumed passes: current runtime
mutations, Linux raw-allocation leak checks, final hosted matrix and performance
comparisons are tracked in evidence.md. No second reviewer was invoked to rubber
stamp the fixes; executable regression and mutation results provide the proof.
