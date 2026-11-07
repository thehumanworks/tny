# 0219 — MIT license

Date: 2026-11-07
Status: accepted. Supersedes the licensing notes in
[0034](0034-native-python-and-node-sdks.md) and [0035](0035-nix-flake-packaging.md).

## Context

The repository granted no license. Packages carried `LicenseRef-UNLICENSED`,
the Python SDK carried the `Private :: Do Not Upload` classifier, and the
release workflow's PyPI and npm jobs failed closed in
`scripts/validate_sdk_release.py` until the owner chose a license. The owner
wants tny open source and the `tny` Python package on PyPI.

## Decision

tny is licensed under the MIT License (`LICENSE` at the repository root).

- **Python SDK.** `license = "MIT"` and `LICENSE` is listed in `license-files`. `sdk/python/LICENSE` is a symlink to the root file, so the sdist carries the same bytes. `setup.py` copies the root license into `tny/LICENSE` in every wheel, and falls back to the sdist copy. The `Private :: Do Not Upload` classifier is gone. There is no license classifier, because PEP 639 expressions replace them.
- **npm packages.** `"license": "MIT"`. `package-release.mjs` already copies the root `LICENSE` into the meta and native packages.
- **Metadata files.** Each `LICENSE-METADATA.json` records `MIT`.
- **Install.** `make install` puts `LICENSE` in `share/doc/tny`, next to the third-party texts.
- **Nix.** `tny`, `libtny` and `python-tny` set `meta.license = lib.licenses.mit`.

Third-party components keep their own licenses, which `THIRD_PARTY_NOTICES.md`
lists.

Publishing stays a separate, owner-controlled step. The registry jobs still
need `TNY_REGISTRY_PUBLICATION_ENABLED=true`, the protected `pypi` and `npm`
environments, a trusted publisher on npm, and a PyPI API token in the
`TWINE_PASSWORD` secret: the owner chose token uploads with `twine` for PyPI.
`TNY_PYPI_PUBLICATION_ENABLED=true` enables the PyPI job without npm. The
validator still requires the packaged license bytes to equal the root file.

## Consequences

Anyone may reuse tny under MIT terms. Contributions are MIT too. A wheel or
npm package built from this tree passes `--require-publishable-license`.

## Verification

`tests/packaging/test_make_install.py` checks that `make install` ships
`LICENSE`. A wheel built from `sdk/python` contains `tny/LICENSE` byte-equal
to the root file and MIT metadata with no `Private ::` classifier. `twine check`
passes for the wheel and the sdist. `scripts/validate_sdk_release.py
--require-publishable-license` accepts the repository.
