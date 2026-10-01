// Theme state — 'dark' | 'light' | 'system'. The applied theme on
// <html data-theme="..."> is always concrete ('dark' or 'light'); 'system'
// resolves via prefers-color-scheme.

export type ThemeMode = 'dark' | 'light' | 'system';
export type ResolvedTheme = 'dark' | 'light';

interface ThemeState {
  mode: ThemeMode;
  // The OS preference, kept reactive so everything that resolves 'system'
  // (the applied theme, the toolbar's tooltip) follows an OS change.
  systemLight: boolean;
}

export const theme: ThemeState = $state({ mode: 'system', systemLight: queryPrefersLight() });

// The OS colour preference right now.
function queryPrefersLight(): boolean {
  if (typeof window === 'undefined' || !window.matchMedia) return false;
  return window.matchMedia('(prefers-color-scheme: light)').matches;
}

// Follow OS preference changes (the one listener; App.svelte installs it).
// Returns the uninstaller.
export function installSystemThemeListener(): () => void {
  if (typeof window === 'undefined' || !window.matchMedia) return () => undefined;
  const mq = window.matchMedia('(prefers-color-scheme: light)');
  const handler = () => {
    theme.systemLight = mq.matches;
  };
  mq.addEventListener('change', handler);
  return () => mq.removeEventListener('change', handler);
}

export function setThemeMode(mode: ThemeMode): void {
  theme.mode = mode;
}

// Toggle between dark and light (used by the toolbar toggle button). Always
// flips to the opposite of the currently *resolved* theme, so a click from
// 'system' lands on the visible opposite — no silent no-op step. 'system'
// remains the initial mode on first load (when no persisted preference
// exists) but isn't reachable from this button afterward.
export function cycleTheme(): void {
  const current = resolveTheme(theme.mode);
  theme.mode = current === 'dark' ? 'light' : 'dark';
}

export function systemTheme(): ResolvedTheme {
  return theme.systemLight ? 'light' : 'dark';
}

export function resolveTheme(mode: ThemeMode): ResolvedTheme {
  return mode === 'system' ? systemTheme() : mode;
}

// Apply the resolved theme to <html data-theme>. Called from main.ts before
// mount to avoid flash, and from a Svelte $effect to keep in sync afterward.
export function applyThemeToHtml(mode: ThemeMode): void {
  if (typeof document === 'undefined') return;
  document.documentElement.dataset.theme = resolveTheme(mode);
}
