# Third-Party Notices

Granny Smith uses the following third-party libraries at runtime. The legacy
`app/web/` loads xterm.js, xterm-addon-fit, and JSZip from the
[jsdelivr](https://www.jsdelivr.com/) CDN. The new UI at `app/web2/` bundles
all runtime dependencies — Svelte 5, JSZip, and (as of the Terminal/Logs
phase) `@xterm/xterm` and `@xterm/addon-fit`.

---

## xterm.js v5.x (`@xterm/xterm`)

- **Website:** <https://xtermjs.org/>
- **Repository:** <https://github.com/xtermjs/xterm.js>
- **License:** MIT
- **Used in:** [app/web/index.html](app/web/index.html) (CDN-loaded, legacy);
  [app/web2/src/components/panel-views/terminal/TerminalPane.svelte](app/web2/src/components/panel-views/terminal/TerminalPane.svelte)
  (bundled, dynamic-imported so the terminal chunk only loads when the Terminal
  tab first mounts).

> MIT License
>
> Copyright (c) 2017-2022, The xterm.js authors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

---

## xterm-addon-fit v0.10.x (`@xterm/addon-fit`)

- **Repository:** <https://github.com/xtermjs/xterm.js> (packages/addon-fit)
- **License:** MIT (same as xterm.js above)
- **Used in:** [app/web/index.html](app/web/index.html) (CDN-loaded, legacy);
  [app/web2/src/components/panel-views/terminal/TerminalPane.svelte](app/web2/src/components/panel-views/terminal/TerminalPane.svelte)
  (bundled).

---

## JSZip v3.10.1

- **Website:** <https://stuk.github.io/jszip/>
- **Repository:** <https://github.com/Stuk/jszip>
- **License:** MIT or GPLv3 (dual-licensed)
- **Used in:** [app/web/js/media.js](app/web/js/media.js), [app/web/js/url-media.js](app/web/js/url-media.js)

> MIT License
>
> Copyright (c) 2009-2016 Stuart Knightley, David Duponchel, Franz Buchinger,
> António Afonso
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

---

## Svelte v5.x

- **Website:** <https://svelte.dev/>
- **Repository:** <https://github.com/sveltejs/svelte>
- **License:** MIT
- **Used in:** [app/web2/](app/web2/) — the new web UI runtime. Bundled at build time.

> Copyright (c) 2016-present, Rich Harris and contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

---

## VS Code Codicons

- **Website:** <https://microsoft.github.io/vscode-codicons/>
- **Repository:** <https://github.com/microsoft/vscode-codicons>
- **License:** Creative Commons Attribution 4.0 International (CC BY 4.0)
- **Used in:** [app/web2/public/icons/sprite.svg](app/web2/public/icons/sprite.svg) — SVG path
  data for the codicon glyphs is reproduced verbatim from the upstream project
  (`src/icons/<name>.svg`). Full icon list, trademark, modification, and
  disclaimer text is in [app/web2/public/NOTICE](app/web2/public/NOTICE).

Copyright (c) Microsoft Corporation.

The Codicons icons (graphical content) are licensed under the Creative Commons
Attribution 4.0 International Public License. License text:
<https://creativecommons.org/licenses/by/4.0/legalcode>.

Note: the upstream codicons repository is dual-licensed — documentation/icons
under CC BY 4.0 (LICENSE), and code under MIT (LICENSE-CODE). Granny Smith uses
only the icons, hence CC BY 4.0 applies here. No modifications: the path data
is reproduced verbatim; sizing and theming (color, scale) are applied via CSS
at render time without altering the path data.
