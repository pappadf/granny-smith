// Layout state — panel position, sash sizes, active tab, fullscreen.
// Persistence is wired in persist.svelte.ts (keys: gs-panel-pos, gs-panel-size).

export type PanelPos = 'bottom' | 'left' | 'right';
export type PanelTab =
  'terminal' | 'machine' | 'filesystem' | 'images' | 'checkpoints' | 'debug' | 'logs';

// Tab order is fixed and affects future config serialization.
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

export type WelcomeSlide = 'home' | 'configuration';

interface LayoutState {
  panelPos: PanelPos;
  panelSize: { bottom: number; left: number; right: number };
  panelCollapsed: boolean;
  // Mirrors the browser's native fullscreen state (synced in App.svelte).
  fullscreen: boolean;
  // While fullscreen, also hide the display toolbar and status bar.
  hideChrome: boolean;
  activeTab: PanelTab;
  welcomeSlide: WelcomeSlide;
}

export const layout: LayoutState = $state({
  panelPos: 'bottom',
  panelSize: { ...DEFAULT_PANEL_SIZE },
  panelCollapsed: false,
  fullscreen: false,
  hideChrome: false,
  activeTab: 'terminal',
  welcomeSlide: 'home',
});

export function setWelcomeSlide(slide: WelcomeSlide): void {
  layout.welcomeSlide = slide;
}

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
  // Auto-uncollapse on tab switch.
  if (layout.panelCollapsed) layout.panelCollapsed = false;
}

// The three screen modes: everything visible; browser fullscreen with the
// toolbar and status bar kept; browser fullscreen with them hidden.
export type ScreenMode = 'normal' | 'fullscreen' | 'fullscreen-bare';

export function screenMode(): ScreenMode {
  if (!layout.fullscreen) return 'normal';
  return layout.hideChrome ? 'fullscreen-bare' : 'fullscreen';
}

// True when the toolbar and status bar are hidden (mode 'fullscreen-bare').
export function chromeHidden(): boolean {
  return layout.fullscreen && layout.hideChrome;
}

// Switch screen mode. Entering fullscreen must run inside a user gesture
// (a click handler); `onBlocked` is called if the browser refuses.
export function setScreenMode(mode: ScreenMode, onBlocked?: () => void): void {
  if (mode === 'normal') {
    if (document.fullscreenElement) void document.exitFullscreen().catch(() => undefined);
    return;
  }
  layout.hideChrome = mode === 'fullscreen-bare';
  if (!document.fullscreenElement) {
    document.documentElement.requestFullscreen().catch(() => onBlocked?.());
  }
}

export function resetPanelSizes(): void {
  layout.panelSize = { ...DEFAULT_PANEL_SIZE };
}

export function getPanelMin(pos: PanelPos): number {
  return PANEL_MIN[pos];
}

// Pick a sensible default Panel position for a fresh load based on viewport
// shape. On wider-than-3:2 viewports the
// canvas fits comfortably with a right-side panel; on narrower viewports the
// panel goes at the bottom.
export function autoPickPanelPos(width: number, height: number): PanelPos {
  return width - 1.5 * height > 254 ? 'right' : 'bottom';
}
