// Layout state — panel position, sash sizes, active tab, fullscreen.
// Persistence is wired in persist.svelte.ts (keys: gs-panel-pos, gs-panel-size).

export type PanelPos = 'bottom' | 'left' | 'right';
export type PanelTab =
  | 'terminal'
  | 'machine'
  | 'filesystem'
  | 'images'
  | 'checkpoints'
  | 'debug'
  | 'logs';

// Tab order is locked by spec §4 and affects future config serialization.
export const PANEL_TABS: ReadonlyArray<PanelTab> = [
  'terminal',
  'machine',
  'filesystem',
  'images',
  'checkpoints',
  'debug',
  'logs',
];

const DEFAULT_PANEL_SIZE = { bottom: 280, left: 340, right: 340 };
const PANEL_MIN = { bottom: 120, left: 200, right: 200 };

interface LayoutState {
  panelPos: PanelPos;
  panelSize: { bottom: number; left: number; right: number };
  panelCollapsed: boolean;
  fullscreen: boolean;
  activeTab: PanelTab;
}

export const layout: LayoutState = $state({
  panelPos: 'bottom',
  panelSize: { ...DEFAULT_PANEL_SIZE },
  panelCollapsed: false,
  fullscreen: false,
  activeTab: 'terminal',
});

export function setPanelPos(pos: PanelPos): void {
  layout.panelPos = pos;
}

export function setPanelSize(pos: PanelPos, px: number): void {
  layout.panelSize[pos] = Math.max(PANEL_MIN[pos], Math.round(px));
}

export function setPanelCollapsed(collapsed: boolean): void {
  layout.panelCollapsed = collapsed;
}

export function setActiveTab(tab: PanelTab): void {
  layout.activeTab = tab;
  // Auto-uncollapse on tab switch (matches prototype app.js:978).
  if (layout.panelCollapsed) layout.panelCollapsed = false;
}

export function resetPanelSizes(): void {
  layout.panelSize = { ...DEFAULT_PANEL_SIZE };
}

export function getPanelMin(pos: PanelPos): number {
  return PANEL_MIN[pos];
}

// Pick a sensible default Panel position for a fresh load based on viewport
// shape. Mirrors prototype app.js:596-599. On wider-than-3:2 viewports the
// canvas fits comfortably with a right-side panel; on narrower viewports the
// panel goes at the bottom.
export function autoPickPanelPos(width: number, height: number): PanelPos {
  return width - 1.5 * height > 254 ? 'right' : 'bottom';
}
