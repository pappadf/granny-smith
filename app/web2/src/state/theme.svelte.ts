// Theme state — 'dark' | 'light' | 'system'. The applied theme on
// <html data-theme="..."> is always concrete ('dark' or 'light'); 'system'
// resolves via prefers-color-scheme.

export type ThemeMode = 'dark' | 'light' | 'system';
export type ResolvedTheme = 'dark' | 'light';

interface ThemeState {
  mode: ThemeMode;
}

export const theme: ThemeState = $state({ mode: 'system' });

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
  if (typeof window === 'undefined' || !window.matchMedia) return 'dark';
  return window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
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
