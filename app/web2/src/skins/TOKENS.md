# Token reference

Generated from `src/styles/contract.ts`, `src/styles/scale.css` and
`src/styles/components.css` by `tests/lint/token-reference.test.ts`;
do not edit by hand. See [README.md](README.md) for how a skin uses them.

## Semantic tokens

Defined by every skin in every scheme it has (`skins/<id>/tokens.css`). A token with a default (derived) may be left out.

| Token | Kind | Per scheme | Default | Purpose |
|---|---|---|---|---|
| `--gs-surface-app` | color | yes | (the skin) | page, display letterbox, panel body |
| `--gs-surface-raised` | color | yes | (the skin) | cards, widgets, section headers, panel header |
| `--gs-surface-overlay` | color | yes | (the skin) | modal card, popover, menu, toast |
| `--gs-surface-sunken` | color | yes | (the skin) | code views and the console |
| `--gs-surface-screen` | color | yes | (the skin) | the emulated screen's letterbox |
| `--gs-surface-document` | color | yes | (the skin) | the backing of a framed document (PDF) |
| `--gs-backdrop` | color | yes | (the skin) | the scrim behind a modal or popover |
| `--gs-text` | color | yes | (the skin) | body text |
| `--gs-text-strong` | color | yes | (the skin) | titles, the active tab, emphasis |
| `--gs-text-muted` | color | yes | (the skin) | secondary text, inactive tabs |
| `--gs-text-subtle` | color | yes | (the skin) | tertiary text, idle glyphs |
| `--gs-text-link` | color | yes | (the skin) | links |
| `--gs-text-on-accent` | color | yes | (the skin) | text on the accent, a selection or a status fill |
| `--gs-text-placeholder` | color | yes | `--gs-text-subtle` | input placeholders |
| `--gs-text-disabled` | color | yes | `--gs-text-subtle` | disabled labels |
| `--gs-border` | color | yes | (the skin) | region separators, sash line, control borders |
| `--gs-border-subtle` | color | yes | (the skin) | inner dividers, progress and spinner tracks |
| `--gs-border-card` | color | yes | (the skin) | card outlines |
| `--gs-focus-ring` | color | yes | (the skin) | the focus outline; the active sub-tab indicator |
| `--gs-control-hover` | color | yes | (the skin) | icon or toolbar button hover fill |
| `--gs-control-active` | color | yes | (the skin) | pressed fill; a selected segment |
| `--gs-accent` | color | yes | (the skin) | primary action fill, progress fill |
| `--gs-accent-hover` | color | yes | (the skin) | the accent, hovered |
| `--gs-accent-active` | color | yes | (the skin) | the accent, pressed |
| `--gs-accent-subtle` | color | yes | (the skin) | a tinted accent fill (the active argument) |
| `--gs-row-hover` | color | yes | (the skin) | tree, table, list and menu row hover |
| `--gs-row-selected` | color | yes | (the skin) | the selected row of a focused list |
| `--gs-row-selected-fg` | color | yes | `--gs-text` | text on the selected row |
| `--gs-row-selected-inactive-fg` | color | yes | `--gs-text` | text on the selected row of a list without focus |
| `--gs-row-selected-inactive` | color | yes | (the skin) | the selected row of a list without focus |
| `--gs-row-alt` | color | yes | (the skin) | zebra striping, where a view opts in |
| `--gs-input-bg` | color | yes | (the skin) | text inputs and selects |
| `--gs-input-fg` | color | yes | (the skin) | text in inputs |
| `--gs-input-border` | color | yes | (the skin) | input borders |
| `--gs-input-border-focus` | color | yes | `--gs-focus-ring` | an input border while focused |
| `--gs-control-accent` | color | yes | `--gs-accent` | accent-color of checkboxes, radios, ranges |
| `--gs-info-fg` | color | yes | (the skin) | info: text or icon on a normal surface |
| `--gs-info-bg` | color | yes | (the skin) | info: tinted fill (pill, badge) |
| `--gs-info-border` | color | yes | (the skin) | info: tinted border or rule |
| `--gs-info-solid` | color | yes | (the skin) | info: strong fill (toast badge, button, bar) |
| `--gs-info-on-solid` | color | yes | (the skin) | info: text or icon on the strong fill |
| `--gs-success-fg` | color | yes | (the skin) | success: text or icon on a normal surface |
| `--gs-success-bg` | color | yes | (the skin) | success: tinted fill (pill, badge) |
| `--gs-success-border` | color | yes | (the skin) | success: tinted border or rule |
| `--gs-success-solid` | color | yes | (the skin) | success: strong fill (toast badge, button, bar) |
| `--gs-success-on-solid` | color | yes | (the skin) | success: text or icon on the strong fill |
| `--gs-warning-fg` | color | yes | (the skin) | warning: text or icon on a normal surface |
| `--gs-warning-bg` | color | yes | (the skin) | warning: tinted fill (pill, badge) |
| `--gs-warning-border` | color | yes | (the skin) | warning: tinted border or rule |
| `--gs-warning-solid` | color | yes | (the skin) | warning: strong fill (toast badge, button, bar) |
| `--gs-warning-on-solid` | color | yes | (the skin) | warning: text or icon on the strong fill |
| `--gs-danger-fg` | color | yes | (the skin) | danger: text or icon on a normal surface |
| `--gs-danger-bg` | color | yes | (the skin) | danger: tinted fill (pill, badge) |
| `--gs-danger-border` | color | yes | (the skin) | danger: tinted border or rule |
| `--gs-danger-solid` | color | yes | (the skin) | danger: strong fill (toast badge, button, bar) |
| `--gs-danger-on-solid` | color | yes | (the skin) | danger: text or icon on the strong fill |
| `--gs-state-idle-bg` | color | yes | (the skin) | status bar with no machine |
| `--gs-state-idle-fg` | color | yes | (the skin) | status bar text with no machine |
| `--gs-state-running-bg` | color | yes | (the skin) | status bar, running |
| `--gs-state-paused-bg` | color | yes | (the skin) | status bar, paused |
| `--gs-state-stopped-bg` | color | yes | (the skin) | status bar, stopped |
| `--gs-state-crashed-bg` | color | yes | (the skin) | status bar, crashed |
| `--gs-state-active-fg` | color | yes | (the skin) | text on the running, paused, stopped and crashed bars |
| `--gs-state-hover` | color | yes | (the skin) | status-bar item hover |
| `--gs-syntax-method` | color | yes | (the skin) | syntax: methods |
| `--gs-syntax-attribute` | color | yes | (the skin) | syntax: attributes |
| `--gs-syntax-variable` | color | yes | (the skin) | syntax: shell variables |
| `--gs-syntax-alias` | color | yes | (the skin) | syntax: aliases |
| `--gs-syntax-object` | color | yes | (the skin) | syntax: object path segments |
| `--gs-syntax-operator` | color | yes | (the skin) | syntax: operators |
| `--gs-syntax-decl` | color | yes | (the skin) | syntax: declarations (let, def) |
| `--gs-syntax-interp` | color | yes | (the skin) | syntax: interpolation (${…}) |
| `--gs-syntax-keyword` | color | yes | (the skin) | syntax: statement keywords |
| `--gs-syntax-string` | color | yes | (the skin) | syntax: string literals |
| `--gs-syntax-number` | color | yes | (the skin) | syntax: number literals |
| `--gs-syntax-enum` | color | yes | (the skin) | syntax: enum values |
| `--gs-syntax-type` | color | yes | (the skin) | syntax: type names |
| `--gs-syntax-comment` | color | yes | (the skin) | syntax: comments |
| `--gs-syntax-unknown` | color | yes | (the skin) | syntax: names that resolve to nothing |
| `--gs-syntax-error` | color | yes | (the skin) | syntax: error entries |
| `--gs-syntax-dim` | color | yes | (the skin) | syntax: secondary console text (echo, progress, completion detail) |
| `--gs-code-address` | color | yes | `--gs-text-muted` | address columns |
| `--gs-code-mnemonic` | color | yes | `--gs-text-strong` | opcodes |
| `--gs-code-operand` | color | yes | `--gs-text` | operands; hex bytes |
| `--gs-code-comment` | color | yes | `--gs-syntax-comment` | comments in a listing |
| `--gs-code-symbol` | color | yes | `--gs-text` | symbol names |
| `--gs-code-pc-marker` | color | yes | `--gs-focus-ring` | the PC marker |
| `--gs-code-pc-row-bg` | color | yes | (the skin) | the current-PC row (disassembly, auxiliary cores) |
| `--gs-code-breakpoint` | color | yes | `--gs-danger-solid` | an enabled breakpoint marker |
| `--gs-code-breakpoint-off` | color | yes | `--gs-text-subtle` | a disabled breakpoint or watchpoint marker |
| `--gs-code-reg-name` | color | yes | `--gs-text-muted` | register names |
| `--gs-code-reg-value` | color | yes | `--gs-text` | register values |
| `--gs-code-changed-bg` | color | yes | (the skin) | a value changed since the last step |
| `--gs-code-changed-fg` | color | yes | `--gs-code-reg-value` | text of a changed value |
| `--gs-code-invalid` | color | yes | `--gs-danger-fg` | an invalid entry |
| `--gs-code-banner-rule` | color | yes | `--gs-focus-ring` | the disassembly banner's left rule |
| `--gs-log-fg` | color | yes | `--gs-text` | log lines |
| `--gs-log-high-fg` | color | yes | `--gs-danger-fg` | level 0–1 (the loudest) |
| `--gs-log-mid-fg` | color | yes | `--gs-warning-fg` | level 2–3 |
| `--gs-log-low-fg` | color | yes | `--gs-log-fg` | level 4 and up |
| `--gs-log-meta-fg` | color | yes | `--gs-text-muted` | the category and level columns |
| `--gs-console-bg` | color | yes | (the skin) | the console surface |
| `--gs-console-fg` | color | yes | (the skin) | console text |
| `--gs-console-cursor` | color | yes | (the skin) | the input's caret |
| `--gs-console-selection` | color | yes | (the skin) | selected text in output and input |
| `--gs-console-progress-bg` | color | yes | (the skin) | a running entry |
| `--gs-console-find-hit-bg` | color | yes | (the skin) | a find match |
| `--gs-console-find-current-bg` | color | yes | (the skin) | the current find match |
| `--gs-console-prompt-fg` | color | yes | `--gs-syntax-dim` | the prompt and the command marker |
| `--gs-console-muted-fg` | color | yes | `--gs-syntax-dim` | echo, progress, completion detail |
| `--gs-console-error-fg` | color | yes | `--gs-syntax-error` | stderr and error entries |
| `--gs-console-sig-arg-fg` | color | yes | `--gs-syntax-attribute` | the active argument of a hint |
| `--gs-console-font` | font | yes | `--gs-font-mono` | console text |
| `--gs-console-font-size` | length | yes | `--gs-font-size-base` | console text (CodeMirror too) |
| `--gs-console-line-height` | number | yes | `--gs-line-height-base` | console lines |
| `--gs-console-popup-font-size` | length | yes | `--gs-font-size-sm` | completion popup, signature hint |
| `--gs-drop-border` | color | yes | (the skin) | a drop target outline |
| `--gs-drop-bg` | color | yes | (the skin) | a drop target fill |
| `--gs-drop-label-bg` | color | yes | (the skin) | the drop overlay's label |
| `--gs-drop-label-fg` | color | yes | `--gs-text-on-accent` | the drop overlay's label text |
| `--gs-scrollbar-thumb` | color | yes | (the skin) | scrollbar thumb |
| `--gs-scrollbar-thumb-hover` | color | yes | (the skin) | scrollbar thumb, hovered |
| `--gs-scrollbar-track` | color | yes | (the skin) | scrollbar track |
| `--gs-selection-bg` | color | yes | (the skin) | selected text (::selection) |
| `--gs-shadow-popup` | shadow | yes | (the skin) | menus and popups |
| `--gs-shadow-modal` | shadow | yes | (the skin) | modals and popovers |
| `--gs-shadow-toast` | shadow | yes | (the skin) | toasts |
| `--gs-shadow-screen` | shadow | yes | (the skin) | the emulated screen's frame |

## Scale tokens

Scheme-independent; a skin may override any of them for its `[data-skin]` scope.

| Token | Kind | Per scheme | Default | Purpose |
|---|---|---|---|---|
| `--gs-font-ui` | font |  | `-apple-system, BlinkMacSystemFont, 'Segoe WPC', 'Segoe UI', system-ui, Ubuntu, 'Droid Sans', sans-serif` | the UI font stack |
| `--gs-font-mono` | font |  | `'SF Mono', Monaco, Menlo, Consolas, 'Liberation Mono', 'Courier New', monospace` | the monospace font stack |
| `--gs-font-size-3xs` | length |  | `9px` | type size 3xs |
| `--gs-font-size-2xs` | length |  | `10px` | type size 2xs |
| `--gs-font-size-xs` | length |  | `11px` | type size xs |
| `--gs-font-size-sm` | length |  | `12px` | type size sm |
| `--gs-font-size-base` | length |  | `13px` | type size base |
| `--gs-font-size-md` | length |  | `14px` | type size md |
| `--gs-font-size-lg` | length |  | `15px` | type size lg |
| `--gs-font-size-xl` | length |  | `16px` | type size xl |
| `--gs-font-size-2xl` | length |  | `18px` | type size 2xl |
| `--gs-font-size-3xl` | length |  | `22px` | type size 3xl |
| `--gs-font-size-4xl` | length |  | `28px` | type size 4xl |
| `--gs-font-weight-light` | number |  | `200` | font weight light |
| `--gs-font-weight-regular` | number |  | `400` | font weight regular |
| `--gs-font-weight-medium` | number |  | `500` | font weight medium |
| `--gs-font-weight-semibold` | number |  | `600` | font weight semibold |
| `--gs-font-weight-bold` | number |  | `700` | font weight bold |
| `--gs-line-height-base` | number |  | `1.4` | line height base |
| `--gs-line-height-relaxed` | number |  | `1.5` | line height relaxed |
| `--gs-line-height-code` | number |  | `1.6` | line height code |
| `--gs-caps-transform` | keyword |  | `uppercase` | the transform of micro-headings (uppercase or none) |
| `--gs-caps-tracking` | length |  | `0.04em` | the letter-spacing of micro-headings |
| `--gs-numeric` | keyword |  | `tabular-nums` | numeral style of counters and amounts |
| `--gs-space-0` | length |  | `0` | space step 0 |
| `--gs-space-px` | length |  | `1px` | space step px |
| `--gs-space-0-5` | length |  | `2px` | space step 0-5 |
| `--gs-space-1` | length |  | `4px` | space step 1 |
| `--gs-space-1-5` | length |  | `6px` | space step 1-5 |
| `--gs-space-2` | length |  | `8px` | space step 2 |
| `--gs-space-2-5` | length |  | `10px` | space step 2-5 |
| `--gs-space-3` | length |  | `12px` | space step 3 |
| `--gs-space-3-5` | length |  | `14px` | space step 3-5 |
| `--gs-space-4` | length |  | `16px` | space step 4 |
| `--gs-space-5` | length |  | `20px` | space step 5 |
| `--gs-space-6` | length |  | `24px` | space step 6 |
| `--gs-space-7` | length |  | `28px` | space step 7 |
| `--gs-space-8` | length |  | `32px` | space step 8 |
| `--gs-space-12` | length |  | `48px` | space step 12 |
| `--gs-radius-none` | length |  | `0` | corner radius none |
| `--gs-radius-xs` | length |  | `2px` | corner radius xs |
| `--gs-radius-sm` | length |  | `3px` | corner radius sm |
| `--gs-radius-md` | length |  | `4px` | corner radius md |
| `--gs-radius-lg` | length |  | `6px` | corner radius lg |
| `--gs-radius-pill` | length |  | `9999px` | corner radius pill |
| `--gs-radius-round` | length |  | `50%` | corner radius round |
| `--gs-size-row` | length |  | `22px` | tree, table, list, menu and disassembly rows |
| `--gs-size-control-sm` | length |  | `18px` | small controls (inline inputs, chips) |
| `--gs-size-control` | length |  | `22px` | buttons, inputs, icon buttons |
| `--gs-size-control-md` | length |  | `26px` | selects, sub-tabs, table header |
| `--gs-size-control-lg` | length |  | `30px` | dialog buttons |
| `--gs-size-toolbar` | length |  | `35px` | toolbars and the panel header |
| `--gs-size-tab` | length |  | `31px` | panel tabs |
| `--gs-size-statusbar` | length |  | `22px` | the status bar |
| `--gs-size-icon-xs` | length |  | `12px` | icon size xs |
| `--gs-size-icon-sm` | length |  | `13px` | icon size sm |
| `--gs-size-icon-md` | length |  | `14px` | icon size md |
| `--gs-size-icon` | length |  | `16px` | icon size (base) |
| `--gs-size-indent` | length |  | `14px` | tree indent step |
| `--gs-size-indent-base` | length |  | `8px` | tree first-level indent |
| `--gs-size-sash` | length |  | `4px` | sash hit area |
| `--gs-size-dot` | length |  | `8px` | status and activity dots |
| `--gs-size-progress` | length |  | `6px` | progress bar height |
| `--gs-size-spinner` | length |  | `18px` | spinner diameter |
| `--gs-size-form-label` | length |  | `140px` | form label column |
| `--gs-border-width` | length |  | `1px` | borders |
| `--gs-border-width-strong` | length |  | `2px` | indicator rules |
| `--gs-focus-width` | length |  | `1px` | the focus outline width |
| `--gs-focus-offset` | length |  | `-1px` | the focus outline offset |
| `--gs-opacity-disabled` | number |  | `0.5` | a disabled control |
| `--gs-opacity-drag-source` | number |  | `0.45` | a row being dragged |
| `--gs-opacity-skipped` | number |  | `0.55` | a skipped item |
| `--gs-z-raised` | number |  | `1` | z-order raised |
| `--gs-z-sash` | number |  | `5` | z-order sash |
| `--gs-z-layer` | number |  | `10` | z-order layer |
| `--gs-z-drop` | number |  | `100` | z-order drop |
| `--gs-z-toast` | number |  | `2545` | z-order toast |
| `--gs-z-modal` | number |  | `2600` | z-order modal |
| `--gs-z-popover` | number |  | `2700` | z-order popover |
| `--gs-z-menu` | number |  | `2800` | z-order menu |
| `--gs-duration-instant` | duration |  | `80ms` | duration instant |
| `--gs-duration-fast` | duration |  | `100ms` | duration fast |
| `--gs-duration-quick` | duration |  | `150ms` | duration quick |
| `--gs-duration-base` | duration |  | `200ms` | duration base |
| `--gs-duration-slow` | duration |  | `250ms` | duration slow |
| `--gs-duration-slower` | duration |  | `300ms` | duration slower |
| `--gs-duration-spin` | duration |  | `0.9s` | duration spin |
| `--gs-duration-pulse` | duration |  | `1s` | duration pulse |
| `--gs-duration-indeterminate` | duration |  | `1.3s` | duration indeterminate |
| `--gs-ease-out` | easing |  | `ease-out` | easing out |
| `--gs-ease-in-out` | easing |  | `ease-in-out` | easing in-out |
| `--gs-ease-linear` | easing |  | `linear` | easing linear |

## Component tokens

One component's knobs, defaulting to semantic or scale tokens; a skin may override any of them.

| Token | Kind | Per scheme | Default | Purpose |
|---|---|---|---|---|
| `--gs-selection-fg` | keyword |  | `inherit` | selected text colour (inherit: unchanged) |
| `--gs-tab-fg` | color |  | `var(--gs-text-muted)` | panel tab: fg |
| `--gs-tab-fg-selected` | color |  | `var(--gs-text-strong)` | panel tab: fg selected |
| `--gs-icon-button-fg-on` | color |  | `var(--gs-text-link)` | an icon button that is on or live |
| `--gs-card-bg` | color |  | `var(--gs-surface-raised)` | cards |
| `--gs-menu-bg` | color |  | `var(--gs-surface-overlay)` | menus |
| `--gs-menu-fg` | color |  | `var(--gs-text)` | menu items |
| `--gs-menu-hover-bg` | color |  | `var(--gs-accent)` | the highlighted menu item |
| `--gs-menu-hover-fg` | color |  | `var(--gs-text-on-accent)` | the highlighted menu item text |
| `--gs-toast-bg` | color |  | `var(--gs-surface-overlay)` | toasts |
| `--gs-toast-fg` | color |  | `var(--gs-text)` | toast text |
| `--gs-popover-offset-top` | length |  | `80px` | a popover card below the top edge |
| `--gs-toast-offset-bottom` | length |  | `calc(var(--gs-size-statusbar) + 3px)` | the toast stack above the status bar |
| `--gs-toast-offset-right` | length |  | `3px` | the toast stack from the right edge |
| `--gs-tree-indent` | length |  | `var(--gs-size-indent)` | tree indent per level |
| `--gs-tree-indent-base` | length |  | `var(--gs-size-indent-base)` | tree indent before the first level |
| `--gs-checkpoints-col-machine` | length |  | `110px` | the checkpoints table's machine column |
| `--gs-checkpoints-col-date` | length |  | `160px` | the checkpoints table's date column |
| `--gs-checkpoints-col-size` | length |  | `80px` | the checkpoints table's size column |
| `--gs-sash-hover` | color |  | `color-mix(in srgb, var(--gs-focus-ring) 30%, transparent)` | a sash hovered or dragged |
| `--gs-tree-drop-bg` | color |  | `color-mix(in srgb, var(--gs-drop-border) 15%, transparent)` | a tree row under a drag |
| `--gs-callout-bg` | color |  | `color-mix(in srgb, var(--gs-info-bg) 32%, transparent)` | a callout's fill |
| `--gs-switch-knob-on` | color |  | `var(--gs-text-on-accent)` | the switch's knob, on |
| `--gs-statusbar-dot-idle` | color |  | `var(--gs-text-subtle)` | status dot, idle |
| `--gs-statusbar-dot-running` | color |  | `#89d185` | status dot, running |
| `--gs-statusbar-dot-paused` | color |  | `#cca700` | status dot, paused |
| `--gs-statusbar-dot-stopped` | color |  | `#f14c4c` | status dot, stopped |
| `--gs-statusbar-drive-read` | color |  | `` | a drive light reading (per bar state) |
| `--gs-statusbar-drive-write` | color |  | `` | a drive light writing (per bar state) |
| `--gs-statusbar-chip-on-bg` | color |  | `` | the caps chip latched (per bar state) |
| `--gs-statusbar-chip-on-ring` | color |  | `` | the latched chip's ring (per bar state) |
| `--gs-button-height` | length |  | `var(--gs-size-control)` | button: height |
| `--gs-button-height-lg` | length |  | `var(--gs-size-control-lg)` | button: height lg |
| `--gs-button-padding-x` | length |  | `var(--gs-space-2)` | button: padding x |
| `--gs-button-padding-x-lg` | length |  | `var(--gs-space-3-5)` | button: padding x lg |
| `--gs-button-radius` | length |  | `var(--gs-radius-xs)` | button: radius |
| `--gs-button-font-size` | length |  | `var(--gs-font-size-xs)` | button: font size |
| `--gs-button-font-size-lg` | length |  | `var(--gs-font-size-base)` | button: font size lg |
| `--gs-button-font-weight` | number |  | `var(--gs-font-weight-regular)` | button: font weight |
| `--gs-button-border-width` | length |  | `var(--gs-border-width)` | button: border width |
| `--gs-button-primary-bg` | color |  | `var(--gs-accent)` | button: primary bg |
| `--gs-button-primary-fg` | color |  | `var(--gs-text-on-accent)` | button: primary fg |
| `--gs-button-primary-border` | color |  | `transparent` | button: primary border |
| `--gs-button-primary-bg-hover` | color |  | `var(--gs-accent-hover)` | button: primary bg hover |
| `--gs-button-primary-bg-active` | color |  | `var(--gs-accent-active)` | button: primary bg active |
| `--gs-button-secondary-bg` | color |  | `transparent` | button: secondary bg |
| `--gs-button-secondary-fg` | color |  | `var(--gs-text)` | button: secondary fg |
| `--gs-button-secondary-border` | color |  | `var(--gs-border)` | button: secondary border |
| `--gs-button-secondary-bg-hover` | color |  | `var(--gs-control-hover)` | button: secondary bg hover |
| `--gs-button-secondary-bg-active` | color |  | `var(--gs-control-active)` | button: secondary bg active |
| `--gs-button-danger-bg` | color |  | `var(--gs-danger-solid)` | button: danger bg |
| `--gs-button-danger-fg` | color |  | `var(--gs-danger-on-solid)` | button: danger fg |
| `--gs-button-danger-border` | color |  | `transparent` | button: danger border |
| `--gs-button-danger-bg-hover` | color |  | `color-mix( in srgb, var(--gs-danger-solid), var(--gs-danger-on-solid) 10% )` | button: danger bg hover |
| `--gs-button-danger-bg-active` | color |  | `color-mix( in srgb, var(--gs-danger-solid), var(--gs-surface-app) 15% )` | button: danger bg active |
| `--gs-button-ghost-bg` | color |  | `transparent` | button: ghost bg |
| `--gs-button-ghost-fg` | color |  | `var(--gs-text)` | button: ghost fg |
| `--gs-button-ghost-border` | color |  | `transparent` | button: ghost border |
| `--gs-button-ghost-bg-hover` | color |  | `var(--gs-control-hover)` | button: ghost bg hover |
| `--gs-button-ghost-bg-active` | color |  | `var(--gs-control-active)` | button: ghost bg active |
| `--gs-icon-button-size` | length |  | `var(--gs-size-control)` | icon button: size |
| `--gs-icon-button-size-sm` | length |  | `var(--gs-size-control-sm)` | icon button: size sm |
| `--gs-icon-button-padding` | length |  | `3px` | icon button: padding |
| `--gs-icon-button-radius` | length |  | `var(--gs-radius-lg)` | icon button: radius |
| `--gs-icon-button-radius-panel` | length |  | `var(--gs-radius-xs)` | icon button: radius panel |
| `--gs-icon-button-fg` | color |  | `inherit` | icon button: fg |
| `--gs-icon-button-bg-hover` | color |  | `var(--gs-control-hover)` | icon button: bg hover |
| `--gs-icon-button-bg-active` | color |  | `var(--gs-control-active)` | icon button: bg active |
| `--gs-icon-button-bg-pressed` | color |  | `var(--gs-control-active)` | icon button: bg pressed |
| `--gs-icon-button-rest-opacity` | number |  | `0.6` | icon button: rest opacity |
| `--gs-toolbar-height` | length |  | `var(--gs-size-toolbar)` | toolbar: height |
| `--gs-toolbar-bg` | color |  | `var(--gs-surface-app)` | toolbar: bg |
| `--gs-toolbar-fg` | color |  | `var(--gs-text-strong)` | toolbar: fg |
| `--gs-toolbar-gap` | length |  | `var(--gs-space-1)` | toolbar: gap |
| `--gs-separator` | color |  | `var(--gs-border)` | separator |
| `--gs-separator-length` | length |  | `var(--gs-size-icon)` | separator: length |
| `--gs-segmented-radius` | length |  | `var(--gs-radius-sm)` | segmented: radius |
| `--gs-segmented-font-size` | length |  | `var(--gs-font-size-xs)` | segmented: font size |
| `--gs-segmented-fg` | color |  | `var(--gs-text-muted)` | segmented: fg |
| `--gs-segmented-fg-hover` | color |  | `var(--gs-text-strong)` | segmented: fg hover |
| `--gs-segmented-bg-hover` | color |  | `var(--gs-control-hover)` | segmented: bg hover |
| `--gs-segmented-fg-selected` | color |  | `var(--gs-text-strong)` | segmented: fg selected |
| `--gs-segmented-bg-selected` | color |  | `var(--gs-control-active)` | segmented: bg selected |
| `--gs-segmented-border` | color |  | `transparent` | segmented: border |
| `--gs-chip-height` | length |  | `var(--gs-size-control-sm)` | chip: height |
| `--gs-chip-radius` | length |  | `var(--gs-radius-sm)` | chip: radius |
| `--gs-chip-padding-x` | length |  | `var(--gs-space-2)` | chip: padding x |
| `--gs-chip-off-opacity` | number |  | `0.4` | chip: off opacity |
| `--gs-chip-on-bg` | color |  | `var(--gs-control-active)` | chip: on bg |
| `--gs-chip-on-ring` | color |  | `var(--gs-border)` | chip: on ring |
| `--gs-link-fg` | color |  | `var(--gs-text-link)` | link: fg |
| `--gs-action-row-fg` | color |  | `var(--gs-text-link)` | action row: fg |
| `--gs-action-row-bg-hover` | color |  | `var(--gs-row-hover)` | action row: bg hover |
| `--gs-action-row-radius` | length |  | `var(--gs-radius-sm)` | action row: radius |
| `--gs-input-height` | length |  | `var(--gs-size-control)` | input: height |
| `--gs-input-height-lg` | length |  | `28px` | input: height lg |
| `--gs-input-radius` | length |  | `var(--gs-radius-xs)` | input: radius |
| `--gs-input-padding-x` | length |  | `var(--gs-space-1-5)` | input: padding x |
| `--gs-input-font-size` | length |  | `var(--gs-font-size-xs)` | input: font size |
| `--gs-input-font-size-lg` | length |  | `var(--gs-font-size-base)` | input: font size lg |
| `--gs-input-border-invalid` | color |  | `var(--gs-danger-fg)` | input: border invalid |
| `--gs-select-height` | length |  | `var(--gs-size-control-md)` | select: height |
| `--gs-select-arrow-size` | length |  | `var(--gs-size-icon-xs)` | select: arrow size |
| `--gs-inline-input-height` | length |  | `var(--gs-size-control-sm)` | inline input: height |
| `--gs-check-size` | length |  | `13px` | check: size |
| `--gs-check-radius` | length |  | `var(--gs-radius-xs)` | check: radius |
| `--gs-field-label-fg` | color |  | `var(--gs-text)` | field: label fg |
| `--gs-field-help-fg` | color |  | `var(--gs-text-muted)` | field: help fg |
| `--gs-field-error-fg` | color |  | `var(--gs-danger-fg)` | field: error fg |
| `--gs-form-label-width` | length |  | `var(--gs-size-form-label)` | form: label width |
| `--gs-form-gap` | length |  | `var(--gs-space-2-5)` | form: gap |
| `--gs-tab-height` | length |  | `var(--gs-size-tab)` | panel tab: height |
| `--gs-tab-padding-x` | length |  | `var(--gs-space-2-5)` | panel tab: padding x |
| `--gs-tab-line-height` | length |  | `18px` | panel tab: line height |
| `--gs-tab-font-size` | length |  | `var(--gs-font-size-xs)` | panel tab: font size |
| `--gs-tab-font-weight` | number |  | `var(--gs-font-weight-regular)` | panel tab: font weight |
| `--gs-tab-transform` | keyword |  | `var(--gs-caps-transform)` | panel tab: transform |
| `--gs-tab-tracking` | length |  | `normal` | panel tab: letter-spacing |
| `--gs-tab-fg-hover` | color |  | `var(--gs-text-strong)` | panel tab: fg hover |
| `--gs-tab-indicator-color` | color |  | `var(--gs-text-strong)` | panel tab: indicator color |
| `--gs-tab-indicator-width` | length |  | `var(--gs-border-width)` | panel tab: indicator width |
| `--gs-tab-indicator-inset` | length |  | `var(--gs-space-2-5)` | panel tab: indicator inset |
| `--gs-tab-indicator-offset` | length |  | `var(--gs-space-1)` | panel tab: indicator offset |
| `--gs-tab-sub-height` | length |  | `var(--gs-size-control-md)` | sub-tab: height |
| `--gs-tab-sub-padding-x` | length |  | `var(--gs-space-3)` | sub-tab: padding x |
| `--gs-tab-sub-line-height` | length |  | `var(--gs-line-height-base)` | sub-tab: line height |
| `--gs-tab-sub-font-size` | length |  | `var(--gs-font-size-xs)` | sub-tab: font size |
| `--gs-tab-sub-font-weight` | number |  | `var(--gs-font-weight-semibold)` | sub-tab: font weight |
| `--gs-tab-sub-transform` | keyword |  | `var(--gs-caps-transform)` | sub-tab: transform |
| `--gs-tab-sub-tracking` | length |  | `var(--gs-caps-tracking)` | sub-tab: letter-spacing |
| `--gs-tab-sub-fg` | color |  | `var(--gs-text-muted)` | sub-tab: fg |
| `--gs-tab-sub-fg-hover` | color |  | `var(--gs-text)` | sub-tab: fg hover |
| `--gs-tab-sub-fg-selected` | color |  | `var(--gs-text-strong)` | sub-tab: fg selected |
| `--gs-tab-sub-indicator-color` | color |  | `var(--gs-focus-ring)` | sub-tab: indicator color |
| `--gs-tab-sub-indicator-width` | length |  | `var(--gs-border-width-strong)` | sub-tab: indicator width |
| `--gs-tab-sub-indicator-inset` | length |  | `0px` | sub-tab: indicator inset |
| `--gs-tab-sub-indicator-offset` | length |  | `0px` | sub-tab: indicator offset |
| `--gs-tab-sub-strip-bg` | color |  | `var(--gs-surface-app)` | sub-tab: strip bg |
| `--gs-tab-sub-strip-border` | color |  | `var(--gs-border)` | sub-tab: strip border |
| `--gs-toolbar-gap-inline` | length |  | `var(--gs-space-0-5)` | toolbar: gap inline |
| `--gs-toolbar-border` | color |  | `var(--gs-border)` | toolbar: border |
| `--gs-row-height` | length |  | `var(--gs-size-row)` | row: height |
| `--gs-row-height-compact` | length |  | `20px` | row: height compact |
| `--gs-row-padding-x` | length |  | `var(--gs-space-2)` | row: padding x |
| `--gs-row-padding-y-compact` | length |  | `var(--gs-space-px)` | row: padding y compact |
| `--gs-row-font-size` | length |  | `var(--gs-font-size-base)` | row: font size |
| `--gs-row-gap` | length |  | `var(--gs-space-1-5)` | row: gap |
| `--gs-list-row-gap-compact` | length |  | `var(--gs-space-3)` | list row: gap compact |
| `--gs-list-row-padding-compact` | length |  | `var(--gs-space-0-5) var(--gs-space-3)` | list row: padding compact |
| `--gs-list-row-font-size-compact` | length |  | `var(--gs-font-size-xs)` | list row: font size compact |
| `--gs-list-row-indent` | length |  | `var(--gs-space-7)` | list row: indent |
| `--gs-tree-dim-opacity` | number |  | `0.45` | tree: dim opacity |
| `--gs-tree-category-padding-top` | length |  | `var(--gs-space-1-5)` | tree: category padding top |
| `--gs-tree-category-weight` | number |  | `var(--gs-font-weight-semibold)` | tree: category weight |
| `--gs-tree-category-fg` | color |  | `var(--gs-text-strong)` | tree: category fg |
| `--gs-heading-font-size` | length |  | `var(--gs-font-size-xs)` | heading: font size |
| `--gs-heading-font-size-sm` | length |  | `var(--gs-font-size-2xs)` | heading: font size sm |
| `--gs-heading-weight` | number |  | `var(--gs-font-weight-semibold)` | heading: weight |
| `--gs-heading-rule-weight` | number |  | `var(--gs-font-weight-regular)` | heading: rule weight |
| `--gs-heading-fg` | color |  | `var(--gs-text-muted)` | heading: fg |
| `--gs-heading-transform` | keyword |  | `var(--gs-caps-transform)` | heading: transform |
| `--gs-heading-tracking` | length |  | `var(--gs-caps-tracking)` | heading: letter-spacing |
| `--gs-heading-gap` | length |  | `var(--gs-space-2)` | heading: gap |
| `--gs-heading-sm-margin-top` | length |  | `var(--gs-space-1-5)` | h4 heading: margin top |
| `--gs-heading-sm-margin-bottom` | length |  | `var(--gs-space-1)` | h4 heading: margin bottom |
| `--gs-section-divider` | color |  | `var(--gs-border)` | the rule between sections and above a divider heading |
| `--gs-section-header-bg` | color |  | `var(--gs-surface-app)` | section: header bg |
| `--gs-section-header-height` | length |  | `var(--gs-size-row)` | section: header height |
| `--gs-section-title-fg` | color |  | `var(--gs-text-strong)` | section: title fg |
| `--gs-table-header-height` | length |  | `var(--gs-size-control-md)` | table: header height |
| `--gs-table-header-bg` | color |  | `var(--gs-surface-app)` | table: header bg |
| `--gs-table-sort-size` | length |  | `var(--gs-size-icon-xs)` | table: sort size |
| `--gs-hint-fg` | color |  | `var(--gs-text-muted)` | hint: fg |
| `--gs-hint-error-fg` | color |  | `var(--gs-danger-fg)` | hint: error fg |
| `--gs-hint-font-size` | length |  | `var(--gs-font-size-xs)` | hint: font size |
| `--gs-hint-font-size-view` | length |  | `var(--gs-font-size-sm)` | hint: font size view |
| `--gs-hint-padding-section` | length |  | `var(--gs-space-1-5) var(--gs-space-3)` | hint: padding section |
| `--gs-hint-padding-block` | length |  | `var(--gs-space-2) var(--gs-space-4)` | hint: padding block |
| `--gs-hint-padding-pane` | length |  | `var(--gs-space-3)` | hint: padding pane |
| `--gs-hint-padding-list` | length |  | `var(--gs-space-1-5) var(--gs-list-row-indent)` | hint: padding list |
| `--gs-hint-padding-view` | length |  | `var(--gs-space-4)` | hint: padding view |
| `--gs-disclosure-size` | length |  | `var(--gs-size-icon-md)` | disclosure: size |
| `--gs-disclosure-rotation-closed` | keyword |  | `-90deg` | disclosure: rotation closed |
| `--gs-disclosure-duration` | duration |  | `var(--gs-duration-instant)` | disclosure: duration |
| `--gs-disclosure-fg` | color |  | `var(--gs-text-muted)` | disclosure: fg |
| `--gs-disclosure-mask` | url |  | `url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Cpath d='M3.146 5.854l4.5 4.5a.5.5 0 0 0 .708 0l4.5-4.5a.5.5 0 0 0-.708-.708L8 9.293 3.854 5.146a.5.5 0 1 0-.708.708z'/%3E%3C/svg%3E")` | disclosure: the chevron as a mask image (summary markers) |
| `--gs-spinner-size` | length |  | `var(--gs-size-spinner)` | spinner: size |
| `--gs-spinner-size-sm` | length |  | `var(--gs-size-icon-xs)` | spinner: size sm |
| `--gs-spinner-width` | length |  | `var(--gs-border-width-strong)` | spinner: width |
| `--gs-spinner-track` | color |  | `var(--gs-border-subtle)` | spinner: track |
| `--gs-spinner-fg` | color |  | `var(--gs-accent)` | spinner: fg |
| `--gs-sash-size` | length |  | `var(--gs-size-sash)` | sash: size |
| `--gs-sash-line` | color |  | `var(--gs-border)` | sash: the hairline |
| `--gs-switch-width` | length |  | `26px` | switch: width |
| `--gs-switch-height` | length |  | `14px` | switch: height |
| `--gs-switch-inset` | length |  | `var(--gs-space-px)` | switch: inset |
| `--gs-switch-knob-size` | length |  | `10px` | switch: knob size |
| `--gs-switch-border` | color |  | `var(--gs-border)` | switch: border |
| `--gs-switch-track` | color |  | `var(--gs-surface-app)` | switch: track |
| `--gs-switch-track-on` | color |  | `var(--gs-focus-ring)` | switch: track on |
| `--gs-switch-knob` | color |  | `var(--gs-text-muted)` | switch: knob |
| `--gs-switch-disabled-opacity` | number |  | `0.6` | switch: disabled opacity |
| `--gs-menu-radius` | length |  | `var(--gs-radius-md)` | menu: radius |
| `--gs-menu-item-height` | length |  | `var(--gs-size-row)` | menu: item height |
| `--gs-menu-item-padding-x` | length |  | `var(--gs-space-3)` | menu: item padding x |
| `--gs-menu-separator` | color |  | `var(--gs-border)` | menu: separator |
| `--gs-modal-bg` | color |  | `var(--gs-surface-raised)` | modal: bg |
| `--gs-modal-radius` | length |  | `var(--gs-radius-lg)` | modal: radius |
| `--gs-modal-padding` | length |  | `var(--gs-space-5) var(--gs-space-6)` | modal: padding |
| `--gs-modal-gap` | length |  | `var(--gs-space-3-5)` | modal: gap |
| `--gs-modal-min-width` | length |  | `320px` | modal: min width |
| `--gs-modal-max-width` | length |  | `520px` | modal: max width |
| `--gs-modal-title-size` | length |  | `var(--gs-font-size-xl)` | modal: title size |
| `--gs-modal-title-weight` | number |  | `var(--gs-font-weight-medium)` | modal: title weight |
| `--gs-popover-min-width` | length |  | `280px` | popover: min width |
| `--gs-popover-max-width` | length |  | `360px` | popover: max width |
| `--gs-popover-padding` | length |  | `var(--gs-space-3) var(--gs-space-3-5)` | popover: padding |
| `--gs-popover-title-size` | length |  | `var(--gs-font-size-base)` | popover: title size |
| `--gs-toast-radius` | length |  | `var(--gs-radius-md)` | toast: radius |
| `--gs-toast-padding` | length |  | `var(--gs-space-2-5) var(--gs-space-3)` | toast: padding |
| `--gs-toast-min-width` | length |  | `260px` | toast: min width |
| `--gs-callout-success-bg` | color |  | `color-mix(in srgb, var(--gs-success-bg) 32%, transparent)` | callout: success bg |
| `--gs-callout-warning-bg` | color |  | `color-mix(in srgb, var(--gs-warning-bg) 32%, transparent)` | callout: warning bg |
| `--gs-callout-danger-bg` | color |  | `color-mix(in srgb, var(--gs-danger-bg) 32%, transparent)` | callout: danger bg |
| `--gs-callout-border` | color |  | `var(--gs-border)` | callout: border |
| `--gs-pill-height` | length |  | `16px` | pill: height |
| `--gs-pill-padding-x` | length |  | `var(--gs-space-1-5)` | pill: padding x |
| `--gs-pill-radius` | length |  | `var(--gs-radius-pill)` | pill: radius |
| `--gs-pill-font-size` | length |  | `var(--gs-font-size-2xs)` | pill: font size |
| `--gs-pill-weight` | number |  | `var(--gs-font-weight-semibold)` | pill: weight |
| `--gs-pill-tracking` | length |  | `var(--gs-caps-tracking)` | pill: letter-spacing |
| `--gs-pill-transform` | keyword |  | `var(--gs-caps-transform)` | pill: transform |
| `--gs-count-fg` | color |  | `var(--gs-text-muted)` | count: fg |
| `--gs-count-bg` | color |  | `transparent` | count: bg |
| `--gs-count-font-size` | length |  | `var(--gs-font-size-xs)` | count: font size |
| `--gs-count-padding` | length |  | `0` | count: padding |
| `--gs-count-radius` | length |  | `0` | count: radius |
| `--gs-progress-height` | length |  | `var(--gs-size-progress)` | progress: height |
| `--gs-progress-radius` | length |  | `var(--gs-radius-sm)` | progress: radius |
| `--gs-progress-track` | color |  | `var(--gs-border-subtle)` | progress: track |
| `--gs-progress-fill` | color |  | `var(--gs-accent)` | progress: fill |
| `--gs-progress-fill-unpacking` | color |  | `var(--gs-accent)` | progress: fill unpacking |
| `--gs-progress-fill-done` | color |  | `var(--gs-success-solid)` | progress: fill done |
| `--gs-progress-fill-failed` | color |  | `var(--gs-danger-solid)` | progress: fill failed |
| `--gs-progress-stripe` | color |  | `color-mix(in srgb, var(--gs-text-on-accent) 25%, transparent)` | progress: stripe |
| `--gs-progress-stripe-size` | length |  | `12px` | progress: stripe size |
| `--gs-activity-dot-size` | length |  | `var(--gs-size-dot)` | activity dot: size |
| `--gs-activity-dot-fg` | color |  | `currentColor` | activity dot: fg |
| `--gs-card-border` | color |  | `var(--gs-border-card)` | card: border |
| `--gs-card-radius` | length |  | `var(--gs-radius-lg)` | card: radius |
| `--gs-card-padding` | length |  | `var(--gs-space-3-5) var(--gs-space-4)` | card: padding |
| `--gs-hero-title-size` | length |  | `var(--gs-font-size-4xl)` | hero: title size |
| `--gs-hero-title-weight` | number |  | `var(--gs-font-weight-light)` | hero: title weight |
| `--gs-hero-title-fg` | color |  | `var(--gs-text-strong)` | hero: title fg |
| `--gs-hero-subtitle-fg` | color |  | `var(--gs-text-muted)` | hero: subtitle fg |
| `--gs-hero-subtitle-size` | length |  | `var(--gs-font-size-md)` | hero: subtitle size |
| `--gs-statusbar-dot-crashed` | color |  | `#ff8a80` | statusbar: dot crashed |
| `--gs-statusbar-height` | length |  | `var(--gs-size-statusbar)` | statusbar: height |
| `--gs-statusbar-font-size` | length |  | `var(--gs-font-size-sm)` | statusbar: font size |
| `--gs-statusbar-bg-idle` | color |  | `var(--gs-state-idle-bg)` | statusbar: bg idle |
| `--gs-statusbar-bg-running` | color |  | `var(--gs-state-running-bg)` | statusbar: bg running |
| `--gs-statusbar-bg-paused` | color |  | `var(--gs-state-paused-bg)` | statusbar: bg paused |
| `--gs-statusbar-bg-stopped` | color |  | `var(--gs-state-stopped-bg)` | statusbar: bg stopped |
| `--gs-statusbar-bg-crashed` | color |  | `var(--gs-state-crashed-bg)` | statusbar: bg crashed |
| `--gs-statusbar-fg-idle` | color |  | `var(--gs-state-idle-fg)` | statusbar: fg idle |
| `--gs-statusbar-fg-active` | color |  | `var(--gs-state-active-fg)` | statusbar: fg active |
| `--gs-statusbar-item-hover` | color |  | `var(--gs-state-hover)` | statusbar: item hover |
| `--gs-statusbar-drive-idle-opacity` | number |  | `0.55` | statusbar: drive idle opacity |
| `--gs-statusbar-drive-style` | keyword |  | `text` | statusbar: the drive lights as text, icon or led |
| `--gs-statusbar-printer-error` | color |  | `` | statusbar: a failed print job (per bar state) |
| `--gs-segmented-fg-disabled` | color |  | `color-mix(in srgb, var(--gs-segmented-fg) 50%, transparent)` | segmented: fg disabled |
| `--gs-screen-frame-bg` | color |  | `var(--gs-surface-screen)` | screen frame: bg |
| `--gs-screen-frame-shadow` | shadow |  | `var(--gs-shadow-screen)` | screen frame: shadow |
| `--gs-screen-frame-padding` | length |  | `0px` | screen frame: padding |
| `--gs-screen-frame-radius` | length |  | `0px` | screen frame: radius |
| `--gs-statusline-bg` | color |  | `var(--gs-surface-raised)` | statusline: bg |
| `--gs-statusline-padding` | length |  | `var(--gs-space-1) var(--gs-space-3)` | statusline: padding |
| `--gs-tree-guide` | color |  | `transparent` | tree: the indent guide lines (transparent: none) |
| `--gs-tree-guide-width` | length |  | `var(--gs-border-width)` | tree: the indent guide width |
| `--gs-console-stderr-opacity` | number |  | `0.75` | console: stderr entries |
| `--gs-progress-indeterminate-opacity` | number |  | `0.45` | progress: indeterminate bar under reduced motion |
| `--gs-activity-dot-opacity-max` | number |  | `0.6` | activity dot: opacity at rest |
| `--gs-activity-dot-opacity-min` | number |  | `0.3` | activity dot: low point of the pulse |
| `--gs-statusbar-icon-opacity` | number |  | `0.85` | statusbar: the speed icon |
| `--gs-statusbar-meta-opacity` | number |  | `0.75` | statusbar: the MIPS readout |
