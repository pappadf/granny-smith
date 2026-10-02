# Third-Party Notices

Granny Smith uses the following third-party libraries at runtime. The UI at
`app/web2/` bundles all of them — Svelte 5, JSZip, and CodeMirror 6 / Lezer.

---

## CodeMirror 6 (`@codemirror/state`, `view`, `language`, `autocomplete`, `commands`) and Lezer (`@lezer/common`, `@lezer/highlight`)

- **Website:** <https://codemirror.net/>
- **Repository:** <https://github.com/codemirror> and <https://github.com/lezer-parser>
- **License:** MIT
- **Used in:** [app/web2/src/components/panel-views/terminal/ConsoleInput.ts](app/web2/src/components/panel-views/terminal/ConsoleInput.ts)
  (bundled, dynamic-imported so the chunk only loads when the Terminal
  console first mounts). Their dependencies `style-mod`, `w3c-keyname`,
  `crelt` and `@marijn/find-cluster-break`, by the same author and under
  the same license, are bundled with them. The Lezer packages carry the
  copyright line "Copyright (C) 2018 by Marijn Haverbeke".

> MIT License
>
> Copyright (C) 2018-2021 by Marijn Haverbeke <marijn@haverbeke.berlin> and others
>
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
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

---

## JSZip v3.10.1

- **Website:** <https://stuk.github.io/jszip/>
- **Repository:** <https://github.com/Stuk/jszip>
- **License:** MIT or GPLv3 (dual-licensed)
- **Used in:** [app/web2/src/lib/archive.ts](app/web2/src/lib/archive.ts) (bundled as a code-split dynamic-import chunk)

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
- **Used in:** [app/web2/src/icons/sprite.svg](app/web2/src/icons/sprite.svg) (and its copy,
  the Platinum skin's [sprite.svg](app/web2/src/skins/platinum/sprite.svg), where six
  symbols are replaced by original Finder-style drawings) — SVG path
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

---

## Chivo

- **Repository:** <https://github.com/Omnibus-Type/Chivo> (packaged by Fontsource,
  `@fontsource-variable/chivo`)
- **License:** SIL Open Font License 1.1 (OFL-1.1)
- **Used in:** the Platinum skin's UI font,
  [app/web2/public/skins/platinum/fonts/](app/web2/public/skins/platinum/fonts/)
  (the variable Latin subset, unmodified; see
  [app/web2/public/skins/platinum/NOTICE](app/web2/public/skins/platinum/NOTICE)).

Copyright 2019 The Chivo Project Authors.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
License text: <https://openfontlicense.org/open-font-license-official-text/>.

---

## Sora

- **Repository:** <https://github.com/sora-xor/sora-font> (packaged by Fontsource,
  `@fontsource-variable/sora`)
- **License:** SIL Open Font License 1.1 (OFL-1.1)
- **Used in:** the Midnight and Starlight skins' UI font,
  [app/web2/public/skins/glass/fonts/](app/web2/public/skins/glass/fonts/)
  (the variable Latin subset, unmodified; see
  [app/web2/public/skins/glass/NOTICE](app/web2/public/skins/glass/NOTICE)).

Copyright 2019 The Sora Project Authors.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
License text: <https://openfontlicense.org/open-font-license-official-text/>.

---

## JetBrains Mono

- **Repository:** <https://github.com/JetBrains/JetBrainsMono> (packaged by Fontsource,
  `@fontsource-variable/jetbrains-mono`)
- **License:** SIL Open Font License 1.1 (OFL-1.1)
- **Used in:** the Midnight and Starlight skins' code font,
  [app/web2/public/skins/glass/fonts/](app/web2/public/skins/glass/fonts/)
  (the variable Latin subset, unmodified).

Copyright 2020 The JetBrains Mono Project Authors.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
License text: <https://openfontlicense.org/open-font-license-official-text/>.

---

## Hanken Grotesk

- **Repository:** <https://github.com/marcologous/hanken-grotesk> (packaged by Fontsource,
  `@fontsource-variable/hanken-grotesk`)
- **License:** SIL Open Font License 1.1 (OFL-1.1)
- **Used in:** the Aqua skin's UI font,
  [app/web2/public/skins/aqua/fonts/](app/web2/public/skins/aqua/fonts/)
  (the variable Latin subset, unmodified; see
  [app/web2/public/skins/aqua/NOTICE](app/web2/public/skins/aqua/NOTICE)).

Copyright 2021 The Hanken Grotesk Project Authors.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
License text: <https://openfontlicense.org/open-font-license-official-text/>.

---

## Fira Mono

- **Repository:** <https://github.com/mozilla/Fira> (packaged by Fontsource,
  `@fontsource/fira-mono`)
- **License:** SIL Open Font License 1.1 (OFL-1.1)
- **Used in:** the Aqua skin's code font,
  [app/web2/public/skins/aqua/fonts/](app/web2/public/skins/aqua/fonts/)
  (the Regular Latin subset, unmodified).

Copyright 2012-2015 The Mozilla Foundation and Telefonica S.A.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
License text: <https://openfontlicense.org/open-font-license-official-text/>.
