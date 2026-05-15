// localStorage adapter — mirrors a hard-coded subset of state slices under
// the `gs-*` namespace. Same keys as the prototype so existing user settings
// carry across.

import { theme } from './theme.svelte';
import { layout, type PanelPos } from './layout.svelte';

const KEYS = {
  theme: 'gs-theme', // null = system, 'dark' | 'light' = forced
  panelPos: 'gs-panel-pos',
  panelSize: 'gs-panel-size',
} as const;

function readLS(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeLS(key: string, value: string | null): void {
  try {
    if (value === null) localStorage.removeItem(key);
    else localStorage.setItem(key, value);
  } catch {
    // localStorage unavailable (private mode, quota) — silently skip.
  }
}

// Read persisted values once, before mount, and apply them to the state
// modules. Called from main.ts.
export function loadPersistedState(): void {
  const savedTheme = readLS(KEYS.theme);
  if (savedTheme === 'dark' || savedTheme === 'light') {
    theme.mode = savedTheme;
  } else {
    theme.mode = 'system';
  }

  const savedPos = readLS(KEYS.panelPos);
  if (savedPos === 'bottom' || savedPos === 'left' || savedPos === 'right') {
    layout.panelPos = savedPos;
  }

  const savedSize = readLS(KEYS.panelSize);
  if (savedSize) {
    try {
      const parsed = JSON.parse(savedSize) as Partial<Record<PanelPos, number>>;
      if (typeof parsed.bottom === 'number') layout.panelSize.bottom = parsed.bottom;
      if (typeof parsed.left === 'number') layout.panelSize.left = parsed.left;
      if (typeof parsed.right === 'number') layout.panelSize.right = parsed.right;
    } catch {
      // Malformed JSON — ignore.
    }
  }
}

// Wire up effects that mirror state changes back to localStorage. Must be
// called from a root-effect context (or from a component's $effect).
export function startPersistEffects(): void {
  $effect(() => {
    // Persist theme as 'dark'/'light' explicit, or remove the key for system.
    if (theme.mode === 'system') writeLS(KEYS.theme, null);
    else writeLS(KEYS.theme, theme.mode);
  });

  $effect(() => {
    writeLS(KEYS.panelPos, layout.panelPos);
  });

  $effect(() => {
    writeLS(KEYS.panelSize, JSON.stringify(layout.panelSize));
  });
}
