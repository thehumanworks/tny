# Third-party notices

Bundled `libtny` platform wheels include the following vendored components.
This file records their notices independently of tny's project-license status.

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

The authoritative notices are retained in the corresponding vendored source
and header files in the tny repository. Pure discovery wheels do not bundle
these components, but include this notice so source-install and platform-wheel
metadata remain identical.
