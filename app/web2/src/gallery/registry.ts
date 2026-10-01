// The gallery's story list. Each story renders one component (or a small
// composition) in every variant the spec screenshots; a variant is a
// separate page load, so global state set by one never leaks into another.
//
// tests/e2e/gallery/ui-gallery.spec.ts reads this list from the page
// (window.__gsGallery) and screenshots every story × variant × scheme.
export interface StoryProps {
  variant: string;
}

export interface StoryDef {
  // Unique id; also the screenshot name and the ?story= value.
  name: string;
  // The variants, each one page; ['default'] for a single-state story.
  variants: readonly string[];
  // Viewport for the screenshot (the stage fills it).
  width: number;
  height: number;
  // Per-variant selector to hover before the screenshot (forced :hover).
  hover?: Readonly<Record<string, string>>;
  // Per-variant selector to focus by keyboard (forced :focus-visible).
  focus?: Readonly<Record<string, string>>;
  // The story component (a Svelte component taking StoryProps, or none).
  load: () => Promise<{ default: unknown }>;
}

// Shorthand for a story with one variant.
const ONE = ['default'] as const;

export const STORIES: readonly StoryDef[] = [
  {
    name: 'UiButtons',
    variants: ['default', 'hover', 'focus'],
    width: 820,
    height: 300,
    hover: { hover: '.b-primary-sm' },
    focus: { focus: '.b-secondary-lg' },
    load: () => import('./stories/UiButtons.story.svelte'),
  },
  {
    name: 'UiInputs',
    variants: ['default', 'focus'],
    width: 820,
    height: 330,
    focus: { focus: 'input[aria-label="sm"]' },
    load: () => import('./stories/UiInputs.story.svelte'),
  },
  {
    name: 'UiForm',
    variants: ONE,
    width: 560,
    height: 360,
    load: () => import('./stories/UiForm.story.svelte'),
  },
  {
    name: 'UiFeedback',
    variants: ONE,
    width: 640,
    height: 560,
    load: () => import('./stories/UiFeedback.story.svelte'),
  },
  {
    name: 'ForcedStates',
    variants: ONE,
    width: 760,
    height: 260,
    load: () => import('./stories/ForcedStates.story.svelte'),
  },
  {
    name: 'TreeKinds',
    variants: ['default', 'focused'],
    width: 720,
    height: 200,
    focus: { focused: '.cols' },
    load: () => import('./stories/TreeKinds.story.svelte'),
  },
  {
    name: 'DropOverlay',
    variants: ONE,
    width: 600,
    height: 320,
    load: () => import('./stories/DropOverlay.story.svelte'),
  },
  {
    name: 'StatusBar',
    variants: [
      'idle',
      'running',
      'paused',
      'stopped',
      'crashed',
      'activity',
      'idle-activity',
      'idle-error',
    ],
    width: 900,
    height: 60,
    load: () => import('./stories/StatusBar.story.svelte'),
  },
  {
    name: 'DisplayToolbar',
    variants: ['no-machine', 'running', 'paused', 'accel'],
    width: 1000,
    height: 60,
    hover: { running: '.scheduler .sch-btn:nth-child(2)' },
    load: () => import('./stories/DisplayToolbar.story.svelte'),
  },
  {
    name: 'WelcomeHome',
    variants: ONE,
    width: 900,
    height: 560,
    hover: { default: '.card-row' },
    load: () => import('./stories/WelcomeHome.story.svelte'),
  },
  {
    name: 'WelcomeConfig',
    variants: ONE,
    width: 900,
    height: 720,
    load: () => import('./stories/WelcomeConfig.story.svelte'),
  },
  {
    name: 'UrlBoot',
    variants: ['downloading', 'failed'],
    width: 900,
    height: 760,
    load: () => import('./stories/UrlBoot.story.svelte'),
  },
  {
    name: 'PanelHeader',
    variants: ['terminal', 'logs', 'checkpoints', 'debug'],
    width: 1000,
    height: 50,
    hover: { logs: '.action-btn' },
    focus: { terminal: 'button.ptab[data-tab="machine"]' },
    load: () => import('./stories/PanelHeader.story.svelte'),
  },
  {
    name: 'Buttons',
    variants: ONE,
    width: 900,
    height: 260,
    load: () => import('./stories/Buttons.story.svelte'),
  },
  {
    name: 'Dialogs',
    variants: [
      'confirm',
      'confirm-danger',
      'rename',
      'rename-error',
      'prompt',
      'create-hd',
      'create-fd',
    ],
    width: 800,
    height: 520,
    load: () => import('./stories/Dialogs.story.svelte'),
  },
  {
    name: 'NoticeDialogs',
    variants: ['resume', 'preview', 'print'],
    width: 900,
    height: 600,
    load: () => import('./stories/NoticeDialogs.story.svelte'),
  },
  {
    name: 'ContextMenu',
    variants: ['default', 'highlight', 'checked'],
    width: 400,
    height: 260,
    hover: { highlight: '.context-menu .item:nth-child(2)' },
    load: () => import('./stories/ContextMenu.story.svelte'),
  },
  {
    name: 'Toast',
    variants: ['default', 'hover'],
    width: 520,
    height: 200,
    hover: { hover: '.toast.error' },
    load: () => import('./stories/Toast.story.svelte'),
  },
  {
    name: 'TreeRows',
    variants: ['default', 'hover', 'focused'],
    width: 420,
    height: 260,
    hover: { hover: '.tree-row:nth-child(2)' },
    focus: { focused: '.tree' },
    load: () => import('./stories/TreeRows.story.svelte'),
  },
  {
    name: 'Table',
    variants: ['rows', 'empty', 'hover'],
    width: 640,
    height: 200,
    hover: { hover: '.tbody .tr:nth-child(3)' },
    load: () => import('./stories/Table.story.svelte'),
  },
  {
    name: 'Sections',
    variants: ['default', 'hover'],
    width: 420,
    height: 260,
    hover: { hover: '.gs-collapsible:nth-child(2) .header' },
    load: () => import('./stories/Sections.story.svelte'),
  },
  {
    name: 'TabStrip',
    variants: ['default', 'hover', 'focus'],
    width: 420,
    height: 60,
    hover: { hover: '.tab:nth-child(2)' },
    focus: { focus: '.tab:nth-child(1)' },
    load: () => import('./stories/TabStrip.story.svelte'),
  },
  {
    name: 'Logs',
    variants: ['entries', 'empty', 'levels'],
    width: 800,
    height: 360,
    load: () => import('./stories/Logs.story.svelte'),
  },
  {
    name: 'Images',
    variants: ['default', 'hover'],
    width: 520,
    height: 300,
    hover: { hover: '.image-row:nth-child(2)' },
    load: () => import('./stories/Images.story.svelte'),
  },
  {
    name: 'Disassembly',
    variants: ['m68k', 'mmu', 'hover'],
    width: 640,
    height: 420,
    hover: { hover: '.row:nth-child(3)' },
    load: () => import('./stories/Disassembly.story.svelte'),
  },
  {
    name: 'DebugSections',
    variants: ['registers', 'fpu', 'memory', 'mmu', 'breakpoints', 'aux'],
    width: 520,
    height: 520,
    load: () => import('./stories/DebugSections.story.svelte'),
  },
  {
    name: 'Console',
    variants: ['entries', 'find'],
    width: 800,
    height: 480,
    load: () => import('./stories/Console.story.svelte'),
  },
  {
    name: 'ValueEditors',
    variants: ONE,
    width: 520,
    height: 260,
    load: () => import('./stories/ValueEditors.story.svelte'),
  },
  {
    name: 'Splits',
    variants: ['default', 'hover'],
    width: 600,
    height: 300,
    hover: { hover: '.pane-sash' },
    load: () => import('./stories/Splits.story.svelte'),
  },
  {
    name: 'ErrorPage',
    variants: ['webgl', 'startup'],
    width: 900,
    height: 600,
    load: () => import('./stories/ErrorPage.story.svelte'),
  },
  {
    name: 'Icons',
    variants: ONE,
    width: 640,
    height: 200,
    load: () => import('./stories/Icons.story.svelte'),
  },
];

// The story a URL names, or null.
export function findStory(name: string | null): StoryDef | null {
  return STORIES.find((s) => s.name === name) ?? null;
}
