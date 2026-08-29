# tnytty — the tiny terminal

A C11 terminal emulator in the tny monorepo: headless VT core, real pty
sessions, kitty graphics with a bundled `icat`, and a REST HTTP API that
makes every session scriptable and shareable.

```sh
make -C tnytty            # release binary at tnytty/build/tnytty
make -C tnytty test       # unit suite (ASan/UBSan)
tnytty                    # passthrough terminal running $SHELL
tnytty run --listen 127.0.0.1:7681 -- htop
curl -s 127.0.0.1:7681/v1/sessions/<id>/screen   # read the live screen
tnytty icat photo.png     # inline image in any kitty-protocol terminal
```

Docs are the contract: start at [docs/README.md](docs/README.md).
Monorepo rules: root [`AGENTS.md`](../AGENTS.md) and
[ADR 0045](../docs/adr/0045-monorepo-and-tnytty.md).
