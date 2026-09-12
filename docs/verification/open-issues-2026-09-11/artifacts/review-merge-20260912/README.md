# Review and merge verification

The active scope remains issues #122–#127, 39 requirements and 49 invariants.
Implementation is integrated; final delivery is not yet complete. See the
[current delivery checkpoint](../../DELIVERY.md) and [evidence ledger](../../evidence.md).

This directory's published records contain compact source manifests, selected
check results, and bounded summaries. The complete raw logs, disposable mutant
sources/binaries, package probes, before-images, and private live-work directories
are retained in the original task workspace. Those generated C/Python probes are
not product sources and are not part of the curated publication tree. They are
not reformatted or rewritten to make a source-quality check pass.

Important records:

- `final-inputs-v4/manifest.json`: reviewed product/test/configuration/dependency
  snapshot including the reviewed test-fixture portability corrections.
- `live-final/evidence.json`: real generate/edit/captured-preview/vision flow;
  four requests, bounded structural evidence, no credentials or raw provider text.
- `final-preview-mutations-summary.json`: thirteen compiled behavioral faults,
  with original/restored checks and explicit exclusion of invalid attempts.
- `adr-preservation-final.json`: all 88 original ADRs and initial contract intact;
  no new collisions. The two baseline duplicate prefixes await user disposition.
- `mac-final-reconciliation.json`: failed full run and its exact remaining
  shared-fixture issue, followed by separately recorded corrected checks.

Passing rows are scoped to their recorded inputs. A partial or superseded run is
not an all-scope pass. Final hosted CI must run on the actual pushed PR head before
conditional merge. No release, deployment or issue administration is included.
