// The design-token contract: every `--gs-*` token, what it is and who
// defines it.  The single source of truth that the token lints
// (tests/lint/tokens.test.ts), the contrast lint, the gallery's token table
// and the skin-authoring guide (src/skins/README.md) check against.
//
// Layers (see src/skins/README.md):
//   semantic   a role (surface, text, intent…).  Required in every scheme of
//              every skin, unless `derived` names the token its default in
//              styles/components.css points at.
//   scale      a scheme-independent value (type, space, size…), defaulted in
//              styles/scale.css; a skin may override it.
//   component  a knob of one component, defaulted in styles/components.css to
//              semantic or scale tokens; a skin may override it.
// Components read semantic, scale and component tokens, never a skin's
// private `--gs-ref-*` palette.

export type TokenKind =
  | 'color'
  | 'length'
  | 'number'
  | 'font'
  | 'shadow'
  | 'duration'
  | 'easing'
  | 'keyword'
  | 'url';

export type TokenLayer = 'semantic' | 'scale' | 'component';

export type TokenName = `--gs-${string}`;

export interface TokenSpec {
  name: TokenName;
  layer: TokenLayer;
  kind: TokenKind;
  // Has a value per colour scheme (every colour and shadow does).
  perScheme: boolean;
  // A derived semantic token: its default points at this one.
  derived?: TokenName;
  doc: string;
  // Read by JavaScript (lib/tokens.ts) as well as CSS.
  jsRead?: boolean;
}

// A required semantic token: every skin defines it in every scheme.
function sem(name: TokenName, kind: TokenKind, doc: string): TokenSpec {
  return { name, layer: 'semantic', kind, perScheme: true, doc };
}
// A derived semantic token: defaulted to `from`; a skin may set it.
function der(name: TokenName, from: TokenName, doc: string, kind: TokenKind = 'color'): TokenSpec {
  return { name, layer: 'semantic', kind, perScheme: true, derived: from, doc };
}
// A scale token.
function scl(name: TokenName, kind: TokenKind, doc: string, jsRead = false): TokenSpec {
  return { name, layer: 'scale', kind, perScheme: false, doc, ...(jsRead ? { jsRead } : {}) };
}
// A component token.
function cmp(name: TokenName, kind: TokenKind, doc: string, jsRead = false): TokenSpec {
  return { name, layer: 'component', kind, perScheme: false, doc, ...(jsRead ? { jsRead } : {}) };
}

// The scale (styles/scale.css).
const sizes = (prefix: string, kind: TokenKind, steps: string[], doc: string) =>
  steps.map((s) => scl(`--gs-${prefix}-${s}`, kind, `${doc} ${s}`));
const SCALE: TokenSpec[] = [
  scl('--gs-font-ui', 'font', 'the UI font stack'),
  scl('--gs-font-mono', 'font', 'the monospace font stack'),
  ...sizes(
    'font-size',
    'length',
    ['3xs', '2xs', 'xs', 'sm', 'base', 'md', 'lg', 'xl', '2xl', '3xl', '4xl'],
    'type size',
  ),
  ...sizes(
    'font-weight',
    'number',
    ['light', 'regular', 'medium', 'semibold', 'bold'],
    'font weight',
  ),
  ...sizes('line-height', 'number', ['base', 'relaxed', 'code'], 'line height'),
  scl('--gs-caps-transform', 'keyword', 'the transform of micro-headings (uppercase or none)'),
  scl('--gs-caps-tracking', 'length', 'the letter-spacing of micro-headings'),
  scl('--gs-numeric', 'keyword', 'numeral style of counters and amounts'),
  ...sizes(
    'space',
    'length',
    ['0', 'px', '0-5', '1', '1-5', '2', '2-5', '3', '3-5', '4', '5', '6', '7', '8', '12'],
    'space step',
  ),
  ...sizes('radius', 'length', ['none', 'xs', 'sm', 'md', 'lg', 'pill', 'round'], 'corner radius'),
  scl('--gs-size-row', 'length', 'tree, table, list, menu and disassembly rows', true),
  scl('--gs-size-control-sm', 'length', 'small controls (inline inputs, chips)'),
  scl('--gs-size-control', 'length', 'buttons, inputs, icon buttons'),
  scl('--gs-size-control-md', 'length', 'selects, sub-tabs, table header'),
  scl('--gs-size-control-lg', 'length', 'dialog buttons'),
  scl('--gs-size-toolbar', 'length', 'toolbars and the panel header'),
  scl('--gs-size-tab', 'length', 'panel tabs'),
  scl('--gs-size-statusbar', 'length', 'the status bar'),
  ...sizes('size-icon', 'length', ['xs', 'sm', 'md'], 'icon size'),
  scl('--gs-size-icon', 'length', 'icon size (base)'),
  scl('--gs-size-indent', 'length', 'tree indent step'),
  scl('--gs-size-indent-base', 'length', 'tree first-level indent'),
  scl('--gs-size-sash', 'length', 'sash hit area'),
  scl('--gs-size-dot', 'length', 'status and activity dots'),
  scl('--gs-size-progress', 'length', 'progress bar height'),
  scl('--gs-size-spinner', 'length', 'spinner diameter'),
  scl('--gs-size-form-label', 'length', 'form label column'),
  scl('--gs-border-width', 'length', 'borders'),
  scl('--gs-border-width-strong', 'length', 'indicator rules'),
  scl('--gs-focus-width', 'length', 'the focus outline width'),
  scl('--gs-focus-offset', 'length', 'the focus outline offset'),
  scl('--gs-opacity-disabled', 'number', 'a disabled control'),
  scl('--gs-opacity-drag-source', 'number', 'a row being dragged'),
  scl('--gs-opacity-skipped', 'number', 'a skipped item'),
  ...sizes(
    'z',
    'number',
    ['raised', 'sash', 'layer', 'drop', 'toast', 'modal', 'popover', 'menu'],
    'z-order',
  ),
  ...sizes(
    'duration',
    'duration',
    ['instant', 'fast', 'quick', 'base', 'slow', 'slower', 'spin', 'pulse', 'indeterminate'],
    'duration',
  ),
  ...sizes('ease', 'easing', ['out', 'in-out', 'linear'], 'easing'),
];

// The four intents, each with five roles.
const INTENTS = ['info', 'success', 'warning', 'danger'] as const;
const intentTokens: TokenSpec[] = INTENTS.flatMap((i) => [
  sem(`--gs-${i}-fg`, 'color', `${i}: text or icon on a normal surface`),
  sem(`--gs-${i}-bg`, 'color', `${i}: tinted fill (pill, badge)`),
  sem(`--gs-${i}-border`, 'color', `${i}: tinted border or rule`),
  sem(`--gs-${i}-solid`, 'color', `${i}: strong fill (toast badge, button, bar)`),
  sem(`--gs-${i}-on-solid`, 'color', `${i}: text or icon on the strong fill`),
]);

// The syntax palette: the shell's highlight classes (styles/syntax.css).
const SYNTAX = [
  ['method', 'methods'],
  ['attribute', 'attributes'],
  ['variable', 'shell variables'],
  ['alias', 'aliases'],
  ['object', 'object path segments'],
  ['operator', 'operators'],
  ['decl', 'declarations (let, def)'],
  ['interp', 'interpolation (${…})'],
  ['keyword', 'statement keywords'],
  ['string', 'string literals'],
  ['number', 'number literals'],
  ['enum', 'enum values'],
  ['type', 'type names'],
  ['comment', 'comments'],
  ['unknown', 'names that resolve to nothing'],
  ['error', 'error entries'],
  ['dim', 'secondary console text (echo, progress, completion detail)'],
] as const;

export const TOKENS: readonly TokenSpec[] = [
  // --- Surfaces ---------------------------------------------------------------
  sem('--gs-surface-app', 'color', 'page, display letterbox, panel body'),
  sem('--gs-surface-raised', 'color', 'cards, widgets, section headers, panel header'),
  sem('--gs-surface-overlay', 'color', 'modal card, popover, menu, toast'),
  sem('--gs-surface-sunken', 'color', 'code views and the console'),
  sem('--gs-surface-screen', 'color', "the emulated screen's letterbox"),
  sem('--gs-surface-document', 'color', 'the backing of a framed document (PDF)'),
  sem('--gs-backdrop', 'color', 'the scrim behind a modal or popover'),
  // --- Text -------------------------------------------------------------------
  sem('--gs-text', 'color', 'body text'),
  sem('--gs-text-strong', 'color', 'titles, the active tab, emphasis'),
  sem('--gs-text-muted', 'color', 'secondary text, inactive tabs'),
  sem('--gs-text-subtle', 'color', 'tertiary text, idle glyphs'),
  sem('--gs-text-link', 'color', 'links'),
  sem('--gs-text-on-accent', 'color', 'text on the accent, a selection or a status fill'),
  der('--gs-text-placeholder', '--gs-text-subtle', 'input placeholders'),
  der('--gs-text-disabled', '--gs-text-subtle', 'disabled labels'),
  // --- Borders and focus ------------------------------------------------------
  sem('--gs-border', 'color', 'region separators, sash line, control borders'),
  sem('--gs-border-subtle', 'color', 'inner dividers, progress and spinner tracks'),
  sem('--gs-border-card', 'color', 'card outlines'),
  sem('--gs-focus-ring', 'color', 'the focus outline; the active sub-tab indicator'),
  // --- Interaction ------------------------------------------------------------
  sem('--gs-control-hover', 'color', 'icon or toolbar button hover fill'),
  sem('--gs-control-active', 'color', 'pressed fill; a selected segment'),
  sem('--gs-accent', 'color', 'primary action fill, progress fill'),
  sem('--gs-accent-hover', 'color', 'the accent, hovered'),
  sem('--gs-accent-active', 'color', 'the accent, pressed'),
  sem('--gs-accent-subtle', 'color', 'a tinted accent fill (the active argument)'),
  // --- Rows -------------------------------------------------------------------
  sem('--gs-row-hover', 'color', 'tree, table, list and menu row hover'),
  sem('--gs-row-selected', 'color', 'the selected row of a focused list'),
  der('--gs-row-selected-fg', '--gs-text', 'text on the selected row'),
  der(
    '--gs-row-selected-inactive-fg',
    '--gs-text',
    'text on the selected row of a list without focus',
  ),
  sem('--gs-row-selected-inactive', 'color', 'the selected row of a list without focus'),
  sem('--gs-row-alt', 'color', 'zebra striping, where a view opts in'),
  // --- Inputs -----------------------------------------------------------------
  sem('--gs-input-bg', 'color', 'text inputs and selects'),
  sem('--gs-input-fg', 'color', 'text in inputs'),
  sem('--gs-input-border', 'color', 'input borders'),
  der('--gs-input-border-focus', '--gs-focus-ring', 'an input border while focused'),
  der('--gs-control-accent', '--gs-accent', 'accent-color of checkboxes, radios, ranges'),
  // --- Intents ----------------------------------------------------------------
  ...intentTokens,
  // --- Machine state ----------------------------------------------------------
  sem('--gs-state-idle-bg', 'color', 'status bar with no machine'),
  sem('--gs-state-idle-fg', 'color', 'status bar text with no machine'),
  sem('--gs-state-running-bg', 'color', 'status bar, running'),
  sem('--gs-state-paused-bg', 'color', 'status bar, paused'),
  sem('--gs-state-stopped-bg', 'color', 'status bar, stopped'),
  sem('--gs-state-crashed-bg', 'color', 'status bar, crashed'),
  sem('--gs-state-active-fg', 'color', 'text on the running, paused, stopped and crashed bars'),
  sem('--gs-state-hover', 'color', 'status-bar item hover'),
  // --- Syntax -----------------------------------------------------------------
  ...SYNTAX.map(([k, doc]) => sem(`--gs-syntax-${k}`, 'color', `syntax: ${doc}`)),
  // --- Code and data views ----------------------------------------------------
  der('--gs-code-address', '--gs-text-muted', 'address columns'),
  der('--gs-code-mnemonic', '--gs-text-strong', 'opcodes'),
  der('--gs-code-operand', '--gs-text', 'operands; hex bytes'),
  der('--gs-code-comment', '--gs-syntax-comment', 'comments in a listing'),
  der('--gs-code-symbol', '--gs-text', 'symbol names'),
  der('--gs-code-pc-marker', '--gs-focus-ring', 'the PC marker'),
  sem('--gs-code-pc-row-bg', 'color', 'the current-PC row (disassembly, auxiliary cores)'),
  der('--gs-code-breakpoint', '--gs-danger-solid', 'an enabled breakpoint marker'),
  der('--gs-code-breakpoint-off', '--gs-text-subtle', 'a disabled breakpoint or watchpoint marker'),
  der('--gs-code-reg-name', '--gs-text-muted', 'register names'),
  der('--gs-code-reg-value', '--gs-text', 'register values'),
  sem('--gs-code-changed-bg', 'color', 'a value changed since the last step'),
  der('--gs-code-changed-fg', '--gs-code-reg-value', 'text of a changed value'),
  der('--gs-code-invalid', '--gs-danger-fg', 'an invalid entry'),
  der('--gs-code-banner-rule', '--gs-focus-ring', "the disassembly banner's left rule"),
  // --- Logs -------------------------------------------------------------------
  der('--gs-log-fg', '--gs-text', 'log lines'),
  der('--gs-log-high-fg', '--gs-danger-fg', 'level 0–1 (the loudest)'),
  der('--gs-log-mid-fg', '--gs-warning-fg', 'level 2–3'),
  der('--gs-log-low-fg', '--gs-log-fg', 'level 4 and up'),
  der('--gs-log-meta-fg', '--gs-text-muted', 'the category and level columns'),
  // --- Console ----------------------------------------------------------------
  sem('--gs-console-bg', 'color', 'the console surface'),
  sem('--gs-console-fg', 'color', 'console text'),
  sem('--gs-console-cursor', 'color', "the input's caret"),
  sem('--gs-console-selection', 'color', 'selected text in output and input'),
  sem('--gs-console-progress-bg', 'color', 'a running entry'),
  sem('--gs-console-find-hit-bg', 'color', 'a find match'),
  sem('--gs-console-find-current-bg', 'color', 'the current find match'),
  der('--gs-console-prompt-fg', '--gs-syntax-dim', 'the prompt and the command marker'),
  der('--gs-console-muted-fg', '--gs-syntax-dim', 'echo, progress, completion detail'),
  der('--gs-console-error-fg', '--gs-syntax-error', 'stderr and error entries'),
  der('--gs-console-sig-arg-fg', '--gs-syntax-attribute', 'the active argument of a hint'),
  der('--gs-console-font', '--gs-font-mono', 'console text', 'font'),
  der('--gs-console-font-size', '--gs-font-size-base', 'console text (CodeMirror too)', 'length'),
  der('--gs-console-line-height', '--gs-line-height-base', 'console lines', 'number'),
  der(
    '--gs-console-popup-font-size',
    '--gs-font-size-sm',
    'completion popup, signature hint',
    'length',
  ),
  // --- Drag and drop ----------------------------------------------------------
  sem('--gs-drop-border', 'color', 'a drop target outline'),
  sem('--gs-drop-bg', 'color', 'a drop target fill'),
  sem('--gs-drop-label-bg', 'color', "the drop overlay's label"),
  der('--gs-drop-label-fg', '--gs-text-on-accent', "the drop overlay's label text"),
  // --- Scrollbars, selection, elevation ---------------------------------------
  sem('--gs-scrollbar-thumb', 'color', 'scrollbar thumb'),
  sem('--gs-scrollbar-thumb-hover', 'color', 'scrollbar thumb, hovered'),
  sem('--gs-scrollbar-track', 'color', 'scrollbar track'),
  sem('--gs-selection-bg', 'color', 'selected text (::selection)'),
  sem('--gs-shadow-popup', 'shadow', 'menus and popups'),
  sem('--gs-shadow-modal', 'shadow', 'modals and popovers'),
  sem('--gs-shadow-toast', 'shadow', 'toasts'),
  sem('--gs-shadow-screen', 'shadow', "the emulated screen's frame"),

  // --- Scale ------------------------------------------------------------------
  ...SCALE,

  // --- Components -------------------------------------------------------------
  cmp('--gs-selection-fg', 'keyword', 'selected text colour (inherit: unchanged)'),
  cmp('--gs-tab-fg', 'color', 'panel tab: fg'),
  cmp('--gs-tab-fg-selected', 'color', 'panel tab: fg selected'),
  cmp('--gs-icon-button-fg-on', 'color', 'an icon button that is on or live'),
  cmp('--gs-card-bg', 'color', 'cards'),
  cmp('--gs-menu-bg', 'color', 'menus'),
  cmp('--gs-menu-fg', 'color', 'menu items'),
  cmp('--gs-menu-hover-bg', 'color', 'the highlighted menu item'),
  cmp('--gs-menu-hover-fg', 'color', 'the highlighted menu item text'),
  cmp('--gs-toast-bg', 'color', 'toasts'),
  cmp('--gs-toast-fg', 'color', 'toast text'),
  cmp('--gs-popover-offset-top', 'length', 'a popover card below the top edge'),
  cmp('--gs-toast-offset-bottom', 'length', 'the toast stack above the status bar'),
  cmp('--gs-toast-offset-right', 'length', 'the toast stack from the right edge'),
  cmp('--gs-tree-indent', 'length', 'tree indent per level'),
  cmp('--gs-tree-indent-base', 'length', 'tree indent before the first level'),
  cmp('--gs-checkpoints-col-machine', 'length', "the checkpoints table's machine column"),
  cmp('--gs-checkpoints-col-date', 'length', "the checkpoints table's date column"),
  cmp('--gs-checkpoints-col-size', 'length', "the checkpoints table's size column"),
  cmp('--gs-sash-hover', 'color', 'a sash hovered or dragged'),
  cmp('--gs-tree-drop-bg', 'color', 'a tree row under a drag'),
  cmp('--gs-callout-bg', 'color', "a callout's fill"),
  cmp('--gs-switch-knob-on', 'color', "the switch's knob, on"),
  cmp('--gs-statusbar-dot-idle', 'color', 'status dot, idle'),
  cmp('--gs-statusbar-dot-running', 'color', 'status dot, running'),
  cmp('--gs-statusbar-dot-paused', 'color', 'status dot, paused'),
  cmp('--gs-statusbar-dot-stopped', 'color', 'status dot, stopped'),
  cmp('--gs-statusbar-drive-read', 'color', 'a drive light reading (per bar state)'),
  cmp('--gs-statusbar-drive-write', 'color', 'a drive light writing (per bar state)'),
  cmp('--gs-statusbar-chip-on-bg', 'color', 'the caps chip latched (per bar state)'),
  cmp('--gs-statusbar-chip-on-ring', 'color', "the latched chip's ring (per bar state)"),
  cmp('--gs-button-height', 'length', 'button: height'),
  cmp('--gs-button-height-lg', 'length', 'button: height lg'),
  cmp('--gs-button-padding-x', 'length', 'button: padding x'),
  cmp('--gs-button-padding-x-lg', 'length', 'button: padding x lg'),
  cmp('--gs-button-radius', 'length', 'button: radius'),
  cmp('--gs-button-font-size', 'length', 'button: font size'),
  cmp('--gs-button-font-size-lg', 'length', 'button: font size lg'),
  cmp('--gs-button-font-weight', 'number', 'button: font weight'),
  cmp('--gs-button-border-width', 'length', 'button: border width'),
  cmp('--gs-button-primary-bg', 'color', 'button: primary bg'),
  cmp('--gs-button-primary-fg', 'color', 'button: primary fg'),
  cmp('--gs-button-primary-border', 'color', 'button: primary border'),
  cmp('--gs-button-primary-bg-hover', 'color', 'button: primary bg hover'),
  cmp('--gs-button-primary-bg-active', 'color', 'button: primary bg active'),
  cmp('--gs-button-secondary-bg', 'color', 'button: secondary bg'),
  cmp('--gs-button-secondary-fg', 'color', 'button: secondary fg'),
  cmp('--gs-button-secondary-border', 'color', 'button: secondary border'),
  cmp('--gs-button-secondary-bg-hover', 'color', 'button: secondary bg hover'),
  cmp('--gs-button-secondary-bg-active', 'color', 'button: secondary bg active'),
  cmp('--gs-button-danger-bg', 'color', 'button: danger bg'),
  cmp('--gs-button-danger-fg', 'color', 'button: danger fg'),
  cmp('--gs-button-danger-border', 'color', 'button: danger border'),
  cmp('--gs-button-danger-bg-hover', 'color', 'button: danger bg hover'),
  cmp('--gs-button-danger-bg-active', 'color', 'button: danger bg active'),
  cmp('--gs-button-ghost-bg', 'color', 'button: ghost bg'),
  cmp('--gs-button-ghost-fg', 'color', 'button: ghost fg'),
  cmp('--gs-button-ghost-border', 'color', 'button: ghost border'),
  cmp('--gs-button-ghost-bg-hover', 'color', 'button: ghost bg hover'),
  cmp('--gs-button-ghost-bg-active', 'color', 'button: ghost bg active'),
  cmp('--gs-icon-button-size', 'length', 'icon button: size'),
  cmp('--gs-icon-button-size-sm', 'length', 'icon button: size sm'),
  cmp('--gs-icon-button-padding', 'length', 'icon button: padding'),
  cmp('--gs-icon-button-radius', 'length', 'icon button: radius'),
  cmp('--gs-icon-button-radius-panel', 'length', 'icon button: radius panel'),
  cmp('--gs-icon-button-fg', 'color', 'icon button: fg'),
  cmp('--gs-icon-button-bg-hover', 'color', 'icon button: bg hover'),
  cmp('--gs-icon-button-bg-active', 'color', 'icon button: bg active'),
  cmp('--gs-icon-button-bg-pressed', 'color', 'icon button: bg pressed'),
  cmp('--gs-icon-button-rest-opacity', 'number', 'icon button: rest opacity'),
  cmp('--gs-toolbar-height', 'length', 'toolbar: height'),
  cmp('--gs-toolbar-bg', 'color', 'toolbar: bg'),
  cmp('--gs-toolbar-fg', 'color', 'toolbar: fg'),
  cmp('--gs-toolbar-gap', 'length', 'toolbar: gap'),
  cmp('--gs-separator', 'color', 'separator'),
  cmp('--gs-separator-length', 'length', 'separator: length'),
  cmp('--gs-segmented-radius', 'length', 'segmented: radius'),
  cmp('--gs-segmented-font-size', 'length', 'segmented: font size'),
  cmp('--gs-segmented-fg', 'color', 'segmented: fg'),
  cmp('--gs-segmented-fg-hover', 'color', 'segmented: fg hover'),
  cmp('--gs-segmented-bg-hover', 'color', 'segmented: bg hover'),
  cmp('--gs-segmented-fg-selected', 'color', 'segmented: fg selected'),
  cmp('--gs-segmented-bg-selected', 'color', 'segmented: bg selected'),
  cmp('--gs-segmented-border', 'color', 'segmented: border'),
  cmp('--gs-chip-height', 'length', 'chip: height'),
  cmp('--gs-chip-radius', 'length', 'chip: radius'),
  cmp('--gs-chip-padding-x', 'length', 'chip: padding x'),
  cmp('--gs-chip-off-opacity', 'number', 'chip: off opacity'),
  cmp('--gs-chip-on-bg', 'color', 'chip: on bg'),
  cmp('--gs-chip-on-ring', 'color', 'chip: on ring'),
  cmp('--gs-link-fg', 'color', 'link: fg'),
  cmp('--gs-action-row-fg', 'color', 'action row: fg'),
  cmp('--gs-action-row-bg-hover', 'color', 'action row: bg hover'),
  cmp('--gs-action-row-radius', 'length', 'action row: radius'),
  cmp('--gs-input-height', 'length', 'input: height'),
  cmp('--gs-input-height-lg', 'length', 'input: height lg'),
  cmp('--gs-input-radius', 'length', 'input: radius'),
  cmp('--gs-input-padding-x', 'length', 'input: padding x'),
  cmp('--gs-input-font-size', 'length', 'input: font size'),
  cmp('--gs-input-font-size-lg', 'length', 'input: font size lg'),
  cmp('--gs-input-border-invalid', 'color', 'input: border invalid'),
  cmp('--gs-select-height', 'length', 'select: height'),
  cmp('--gs-select-arrow-size', 'length', 'select: arrow size'),
  cmp('--gs-inline-input-height', 'length', 'inline input: height'),
  cmp('--gs-check-size', 'length', 'check: size'),
  cmp('--gs-check-radius', 'length', 'check: radius'),
  cmp('--gs-field-label-fg', 'color', 'field: label fg'),
  cmp('--gs-field-help-fg', 'color', 'field: help fg'),
  cmp('--gs-field-error-fg', 'color', 'field: error fg'),
  cmp('--gs-form-label-width', 'length', 'form: label width'),
  cmp('--gs-form-gap', 'length', 'form: gap'),
  cmp('--gs-tab-height', 'length', 'panel tab: height'),
  cmp('--gs-tab-padding-x', 'length', 'panel tab: padding x'),
  cmp('--gs-tab-line-height', 'length', 'panel tab: line height'),
  cmp('--gs-tab-font-size', 'length', 'panel tab: font size'),
  cmp('--gs-tab-font-weight', 'number', 'panel tab: font weight'),
  cmp('--gs-tab-transform', 'keyword', 'panel tab: transform'),
  cmp('--gs-tab-tracking', 'length', 'panel tab: letter-spacing'),
  cmp('--gs-tab-fg-hover', 'color', 'panel tab: fg hover'),
  cmp('--gs-tab-indicator-color', 'color', 'panel tab: indicator color'),
  cmp('--gs-tab-indicator-width', 'length', 'panel tab: indicator width'),
  cmp('--gs-tab-indicator-inset', 'length', 'panel tab: indicator inset'),
  cmp('--gs-tab-indicator-offset', 'length', 'panel tab: indicator offset'),
  cmp('--gs-tab-sub-height', 'length', 'sub-tab: height'),
  cmp('--gs-tab-sub-padding-x', 'length', 'sub-tab: padding x'),
  cmp('--gs-tab-sub-line-height', 'length', 'sub-tab: line height'),
  cmp('--gs-tab-sub-font-size', 'length', 'sub-tab: font size'),
  cmp('--gs-tab-sub-font-weight', 'number', 'sub-tab: font weight'),
  cmp('--gs-tab-sub-transform', 'keyword', 'sub-tab: transform'),
  cmp('--gs-tab-sub-tracking', 'length', 'sub-tab: letter-spacing'),
  cmp('--gs-tab-sub-fg', 'color', 'sub-tab: fg'),
  cmp('--gs-tab-sub-fg-hover', 'color', 'sub-tab: fg hover'),
  cmp('--gs-tab-sub-fg-selected', 'color', 'sub-tab: fg selected'),
  cmp('--gs-tab-sub-indicator-color', 'color', 'sub-tab: indicator color'),
  cmp('--gs-tab-sub-indicator-width', 'length', 'sub-tab: indicator width'),
  cmp('--gs-tab-sub-indicator-inset', 'length', 'sub-tab: indicator inset'),
  cmp('--gs-tab-sub-indicator-offset', 'length', 'sub-tab: indicator offset'),
  cmp('--gs-tab-sub-strip-bg', 'color', 'sub-tab: strip bg'),
  cmp('--gs-tab-sub-strip-border', 'color', 'sub-tab: strip border'),
  cmp('--gs-toolbar-gap-inline', 'length', 'toolbar: gap inline'),
  cmp('--gs-toolbar-border', 'color', 'toolbar: border'),
  cmp('--gs-row-height', 'length', 'row: height'),
  cmp('--gs-row-height-compact', 'length', 'row: height compact'),
  cmp('--gs-row-padding-x', 'length', 'row: padding x'),
  cmp('--gs-row-padding-y-compact', 'length', 'row: padding y compact'),
  cmp('--gs-row-font-size', 'length', 'row: font size'),
  cmp('--gs-row-gap', 'length', 'row: gap'),
  cmp('--gs-list-row-gap-compact', 'length', 'list row: gap compact'),
  cmp('--gs-list-row-padding-compact', 'length', 'list row: padding compact'),
  cmp('--gs-list-row-font-size-compact', 'length', 'list row: font size compact'),
  cmp('--gs-list-row-indent', 'length', 'list row: indent'),
  cmp('--gs-tree-dim-opacity', 'number', 'tree: dim opacity'),
  cmp('--gs-tree-category-padding-top', 'length', 'tree: category padding top'),
  cmp('--gs-tree-category-weight', 'number', 'tree: category weight'),
  cmp('--gs-tree-category-fg', 'color', 'tree: category fg'),
  cmp('--gs-heading-font-size', 'length', 'heading: font size'),
  cmp('--gs-heading-font-size-sm', 'length', 'heading: font size sm'),
  cmp('--gs-heading-weight', 'number', 'heading: weight'),
  cmp('--gs-heading-rule-weight', 'number', 'heading: rule weight'),
  cmp('--gs-heading-fg', 'color', 'heading: fg'),
  cmp('--gs-heading-transform', 'keyword', 'heading: transform'),
  cmp('--gs-heading-tracking', 'length', 'heading: letter-spacing'),
  cmp('--gs-heading-gap', 'length', 'heading: gap'),
  cmp('--gs-heading-sm-margin-top', 'length', 'h4 heading: margin top'),
  cmp('--gs-heading-sm-margin-bottom', 'length', 'h4 heading: margin bottom'),
  cmp('--gs-section-divider', 'color', 'the rule between sections and above a divider heading'),
  cmp('--gs-section-header-bg', 'color', 'section: header bg'),
  cmp('--gs-section-header-height', 'length', 'section: header height'),
  cmp('--gs-section-title-fg', 'color', 'section: title fg'),
  cmp('--gs-table-header-height', 'length', 'table: header height'),
  cmp('--gs-table-header-bg', 'color', 'table: header bg'),
  cmp('--gs-table-sort-size', 'length', 'table: sort size'),
  cmp('--gs-hint-fg', 'color', 'hint: fg'),
  cmp('--gs-hint-error-fg', 'color', 'hint: error fg'),
  cmp('--gs-hint-font-size', 'length', 'hint: font size'),
  cmp('--gs-hint-font-size-view', 'length', 'hint: font size view'),
  cmp('--gs-hint-padding-section', 'length', 'hint: padding section'),
  cmp('--gs-hint-padding-block', 'length', 'hint: padding block'),
  cmp('--gs-hint-padding-pane', 'length', 'hint: padding pane'),
  cmp('--gs-hint-padding-list', 'length', 'hint: padding list'),
  cmp('--gs-hint-padding-view', 'length', 'hint: padding view'),
  cmp('--gs-disclosure-size', 'length', 'disclosure: size'),
  cmp('--gs-disclosure-rotation-closed', 'keyword', 'disclosure: rotation closed'),
  cmp('--gs-disclosure-duration', 'duration', 'disclosure: duration'),
  cmp('--gs-disclosure-fg', 'color', 'disclosure: fg'),
  cmp('--gs-disclosure-mask', 'url', 'disclosure: the chevron as a mask image (summary markers)'),
  cmp('--gs-spinner-size', 'length', 'spinner: size'),
  cmp('--gs-spinner-size-sm', 'length', 'spinner: size sm'),
  cmp('--gs-spinner-width', 'length', 'spinner: width'),
  cmp('--gs-spinner-track', 'color', 'spinner: track'),
  cmp('--gs-spinner-fg', 'color', 'spinner: fg'),
  cmp('--gs-sash-size', 'length', 'sash: size'),
  cmp('--gs-sash-line', 'color', 'sash: the hairline'),
  cmp('--gs-switch-width', 'length', 'switch: width'),
  cmp('--gs-switch-height', 'length', 'switch: height'),
  cmp('--gs-switch-inset', 'length', 'switch: inset'),
  cmp('--gs-switch-knob-size', 'length', 'switch: knob size'),
  cmp('--gs-switch-border', 'color', 'switch: border'),
  cmp('--gs-switch-track', 'color', 'switch: track'),
  cmp('--gs-switch-track-on', 'color', 'switch: track on'),
  cmp('--gs-switch-knob', 'color', 'switch: knob'),
  cmp('--gs-switch-disabled-opacity', 'number', 'switch: disabled opacity'),
  cmp('--gs-menu-radius', 'length', 'menu: radius'),
  cmp('--gs-menu-item-height', 'length', 'menu: item height'),
  cmp('--gs-menu-item-padding-x', 'length', 'menu: item padding x'),
  cmp('--gs-menu-separator', 'color', 'menu: separator'),
  cmp('--gs-modal-bg', 'color', 'modal: bg'),
  cmp('--gs-modal-radius', 'length', 'modal: radius'),
  cmp('--gs-modal-padding', 'length', 'modal: padding'),
  cmp('--gs-modal-gap', 'length', 'modal: gap'),
  cmp('--gs-modal-min-width', 'length', 'modal: min width'),
  cmp('--gs-modal-max-width', 'length', 'modal: max width'),
  cmp('--gs-modal-title-size', 'length', 'modal: title size'),
  cmp('--gs-modal-title-weight', 'number', 'modal: title weight'),
  cmp('--gs-popover-min-width', 'length', 'popover: min width'),
  cmp('--gs-popover-max-width', 'length', 'popover: max width'),
  cmp('--gs-popover-padding', 'length', 'popover: padding'),
  cmp('--gs-popover-title-size', 'length', 'popover: title size'),
  cmp('--gs-toast-radius', 'length', 'toast: radius'),
  cmp('--gs-toast-padding', 'length', 'toast: padding'),
  cmp('--gs-toast-min-width', 'length', 'toast: min width'),
  cmp('--gs-callout-success-bg', 'color', 'callout: success bg'),
  cmp('--gs-callout-warning-bg', 'color', 'callout: warning bg'),
  cmp('--gs-callout-danger-bg', 'color', 'callout: danger bg'),
  cmp('--gs-callout-border', 'color', 'callout: border'),
  cmp('--gs-pill-height', 'length', 'pill: height'),
  cmp('--gs-pill-padding-x', 'length', 'pill: padding x'),
  cmp('--gs-pill-radius', 'length', 'pill: radius'),
  cmp('--gs-pill-font-size', 'length', 'pill: font size'),
  cmp('--gs-pill-weight', 'number', 'pill: weight'),
  cmp('--gs-pill-tracking', 'length', 'pill: letter-spacing'),
  cmp('--gs-pill-transform', 'keyword', 'pill: transform'),
  cmp('--gs-count-fg', 'color', 'count: fg'),
  cmp('--gs-count-bg', 'color', 'count: bg'),
  cmp('--gs-count-font-size', 'length', 'count: font size'),
  cmp('--gs-count-padding', 'length', 'count: padding'),
  cmp('--gs-count-radius', 'length', 'count: radius'),
  cmp('--gs-progress-height', 'length', 'progress: height'),
  cmp('--gs-progress-radius', 'length', 'progress: radius'),
  cmp('--gs-progress-track', 'color', 'progress: track'),
  cmp('--gs-progress-fill', 'color', 'progress: fill'),
  cmp('--gs-progress-fill-unpacking', 'color', 'progress: fill unpacking'),
  cmp('--gs-progress-fill-done', 'color', 'progress: fill done'),
  cmp('--gs-progress-fill-failed', 'color', 'progress: fill failed'),
  cmp('--gs-progress-stripe', 'color', 'progress: stripe'),
  cmp('--gs-progress-stripe-size', 'length', 'progress: stripe size'),
  cmp('--gs-activity-dot-size', 'length', 'activity dot: size'),
  cmp('--gs-activity-dot-fg', 'color', 'activity dot: fg'),
  cmp('--gs-card-border', 'color', 'card: border'),
  cmp('--gs-card-radius', 'length', 'card: radius'),
  cmp('--gs-card-padding', 'length', 'card: padding'),
  cmp('--gs-hero-title-size', 'length', 'hero: title size'),
  cmp('--gs-hero-title-weight', 'number', 'hero: title weight'),
  cmp('--gs-hero-title-fg', 'color', 'hero: title fg'),
  cmp('--gs-hero-subtitle-fg', 'color', 'hero: subtitle fg'),
  cmp('--gs-hero-subtitle-size', 'length', 'hero: subtitle size'),
  cmp('--gs-statusbar-dot-crashed', 'color', 'statusbar: dot crashed'),
  cmp('--gs-statusbar-height', 'length', 'statusbar: height'),
  cmp('--gs-statusbar-font-size', 'length', 'statusbar: font size'),
  cmp('--gs-statusbar-bg-idle', 'color', 'statusbar: bg idle'),
  cmp('--gs-statusbar-bg-running', 'color', 'statusbar: bg running'),
  cmp('--gs-statusbar-bg-paused', 'color', 'statusbar: bg paused'),
  cmp('--gs-statusbar-bg-stopped', 'color', 'statusbar: bg stopped'),
  cmp('--gs-statusbar-bg-crashed', 'color', 'statusbar: bg crashed'),
  cmp('--gs-statusbar-fg-idle', 'color', 'statusbar: fg idle'),
  cmp('--gs-statusbar-fg-active', 'color', 'statusbar: fg active'),
  cmp('--gs-statusbar-item-hover', 'color', 'statusbar: item hover'),
  cmp('--gs-statusbar-drive-idle-opacity', 'number', 'statusbar: drive idle opacity'),
  cmp('--gs-statusbar-drive-style', 'keyword', 'statusbar: the drive lights as text, icon or led'),
  cmp('--gs-statusbar-printer-error', 'color', 'statusbar: a failed print job (per bar state)'),
  cmp('--gs-segmented-fg-disabled', 'color', 'segmented: fg disabled'),
  cmp('--gs-screen-frame-bg', 'color', 'screen frame: bg'),
  cmp('--gs-screen-frame-shadow', 'shadow', 'screen frame: shadow'),
  cmp('--gs-screen-frame-padding', 'length', 'screen frame: padding'),
  cmp('--gs-screen-frame-radius', 'length', 'screen frame: radius'),
  cmp('--gs-statusline-bg', 'color', 'statusline: bg'),
  cmp('--gs-statusline-padding', 'length', 'statusline: padding'),
  cmp('--gs-tree-guide', 'color', 'tree: the indent guide lines (transparent: none)'),
  cmp('--gs-tree-guide-width', 'length', 'tree: the indent guide width'),
  cmp('--gs-console-stderr-opacity', 'number', 'console: stderr entries'),
  cmp(
    '--gs-progress-indeterminate-opacity',
    'number',
    'progress: indeterminate bar under reduced motion',
  ),
  cmp('--gs-activity-dot-opacity-max', 'number', 'activity dot: opacity at rest'),
  cmp('--gs-activity-dot-opacity-min', 'number', 'activity dot: low point of the pulse'),
  cmp('--gs-statusbar-icon-opacity', 'number', 'statusbar: the speed icon'),
  cmp('--gs-statusbar-meta-opacity', 'number', 'statusbar: the MIPS readout'),
];

// Layout variables components set inline at run time: allowed reads, not
// tokens.
export const LAYOUT_VARS: readonly string[] = ['--gs-panel-size'];

// Foreground/background pairs the contrast lint checks per skin and scheme,
// with the minimum WCAG contrast ratio.  A background with alpha is
// composited over `over` (default: the app surface).
export interface ContrastPair {
  fg: TokenName;
  bg: TokenName;
  min: number;
  over?: TokenName;
  // The status bar's contrast pairs depend on its state.
  scope?: string;
}

const surfaces: TokenName[] = ['--gs-surface-app', '--gs-surface-raised'];
export const CONTRAST_PAIRS: readonly ContrastPair[] = [
  ...surfaces.flatMap((bg) => [
    { fg: '--gs-text' as TokenName, bg, min: 4.5 },
    { fg: '--gs-text-muted' as TokenName, bg, min: 4.5 },
    { fg: '--gs-text-subtle' as TokenName, bg, min: 3 },
    { fg: '--gs-focus-ring' as TokenName, bg, min: 3 },
    ...INTENTS.map((i) => ({ fg: `--gs-${i}-fg` as TokenName, bg, min: 4.5 })),
  ]),
  { fg: '--gs-text-on-accent', bg: '--gs-accent', min: 4.5 },
  ...INTENTS.map((i) => ({
    fg: `--gs-${i}-on-solid` as TokenName,
    bg: `--gs-${i}-solid` as TokenName,
    min: 4.5,
  })),
  ...(['running', 'paused', 'stopped', 'crashed'] as const).map((s) => ({
    fg: '--gs-state-active-fg' as TokenName,
    bg: `--gs-state-${s}-bg` as TokenName,
    min: 4.5,
  })),
  { fg: '--gs-state-idle-fg', bg: '--gs-state-idle-bg', min: 4.5 },
  { fg: '--gs-text', bg: '--gs-row-selected', min: 4.5 },
  { fg: '--gs-row-selected-fg', bg: '--gs-row-selected', min: 4.5 },
  ...INTENTS.map((i) => ({
    fg: `--gs-${i}-fg` as TokenName,
    bg: `--gs-${i}-bg` as TokenName,
    min: 4.5,
  })),
  // The status bar's drive lights and caps chip, on the idle and active bars.
  {
    fg: '--gs-statusbar-drive-read',
    bg: '--gs-state-idle-bg',
    min: 3,
    scope: "[data-state='idle']",
  },
  {
    fg: '--gs-statusbar-drive-write',
    bg: '--gs-state-idle-bg',
    min: 3,
    scope: "[data-state='idle']",
  },
  {
    fg: '--gs-statusbar-drive-read',
    bg: '--gs-state-running-bg',
    min: 3,
    scope: ":not([data-state='idle'])",
  },
  {
    fg: '--gs-statusbar-drive-write',
    bg: '--gs-state-running-bg',
    min: 3,
    scope: ":not([data-state='idle'])",
  },
  { fg: '--gs-log-mid-fg', bg: '--gs-surface-app', min: 4.5 },
  { fg: '--gs-log-high-fg', bg: '--gs-surface-app', min: 4.5 },
];

// The contract by name.
export const TOKEN_BY_NAME: ReadonlyMap<string, TokenSpec> = new Map(
  TOKENS.map((t) => [t.name, t]),
);
