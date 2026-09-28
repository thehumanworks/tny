#!/bin/sh
set -eu
python3 - << 'PY'
from pathlib import Path
Path('fixtures').mkdir(exist_ok=True)
Path('fixtures/records.tsv').write_text('scope\tidentifier\trevision\tvalue\n' + ''.join(
    f'tenant-{i % 17}\tbilling:mode\t{i}\tmode-{i % 5}\n' for i in range(2000)))
PY
