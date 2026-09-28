# Third-party notices

The `libtny` shared library includes the following vendored components. This
file records their notices independently of tny's project-license status.

- **yyjson** — Copyright (c) 2020 YaoYuan <ibireme@gmail.com>.
- **picohttpparser** — Copyright (c) 2009–2014 Kazuho Oku, Tokuhiro
  Matsuno, Daisuke Murase, and Shigeo Mitsunari. Distributed by tny under
  picohttpparser's MIT option.
- **Wslay** — Copyright (c) 2011, 2012 Tatsuhiro Tsujikawa. Its event parser
  also carries Copyright (c) 2008–2010 Bjoern Hoehrmann.

Each component listed above permits distribution under the following MIT
license terms:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The authoritative notices are also retained at the top of the corresponding
vendored source and header files under `third_party/`.

## The tny executable

The `tny` executable (not `libtny`) additionally embeds **CPython 3.14.7**
for Python code cells (docs/adr/0179, docs/adr/0180): Copyright (c) 2001 Python
Software Foundation; All Rights Reserved. It is distributed under the Python
Software Foundation License Version 2; the complete license and the notices
CPython ships are in `third_party/cpython/LICENSE`, installed as
`share/doc/tny/CPython-LICENSE`. The interpreter, its statically linked stdlib
extension modules and its frozen pure-Python standard library are built from the
unmodified hash-pinned python.org source. CPython's own incorporated software
(including expat, libmpdec, SipHash and dtoa) is listed in
`third_party/cpython/LICENSE-incorporated.rst` (CPython's `Doc/license.rst`),
installed as `share/doc/tny/CPython-incorporated-software.rst`; its HACL*
hash implementations are MIT-licensed (`third_party/cpython/HACL-LICENSE`).

The same executable statically links these unmodified, hash-pinned libraries
for the standard library's native modules (`third_party/<name>/`, installed as
`share/doc/tny/<name>-<file>` by `scripts/install_licenses.sh`):

| Library | Version | License | Used by |
| --- | --- | --- | --- |
| OpenSSL | 3.5.8 | Apache-2.0 (`openssl/LICENSE.txt`) | `ssl`, `hashlib` |
| zlib | 1.3.2 | Zlib (`zlib/LICENSE`) | `zlib`, `gzip`, `zipfile` |
| bzip2 | 1.0.8 | bzip2-1.0.6 (`bzip2/LICENSE`) | `bz2` |
| xz (liblzma) | 5.8.4 | 0BSD (`xz/COPYING.0BSD`) | `lzma` |
| zstd | 1.5.7 | BSD-3-Clause (`zstd/LICENSE`) | `compression.zstd` |
| SQLite | 3.53.4 | public domain (`sqlite/LICENSE`) | `sqlite3` |
| libffi | 3.8.0 | MIT (`libffi/LICENSE`) | `ctypes` |
