// localStorage adapter — mirrors hard-coded state slices under the
// `gs-*` namespace. Same keys as the prototype so existing user
// settings carry across.
//
// Phase 7 extends the original (theme + panelPos + panelSize) with
// view-state for Debug sections, MMU subtab, Memory address/mode,
// Logs autoscroll, Filesystem expansion, Images collapsed map,
// Checkpoints sort. Each Phase 7 key uses a `{v, data}` envelope so
// future migrations are tractable. Reads tolerate missing/malformed
// values silently.

import { theme } from './theme.svelte';
import { layout, type PanelPos } from './layout.svelte';
import { debug, type MmuSubtab, type MemoryMode } from './debug.svelte';
import { logs } from './logs.svelte';
import { filesystem } from './filesystem.svelte';
import { images } from './images.svelte';
import { checkpoints, type CheckpointSortColumn, type SortDirection } from './checkpoints.svelte';
import type { ImageCategory } from '@/bus/types';

const KEYS = {
  // Phase 3
  theme: 'gs-theme',
  panelPos: 'gs-panel-pos',
  panelSize: 'gs-panel-size',
  // Phase 7
  debugSections: 'gs-debug-sections',
  debugMemory: 'gs-debug-memory',
  debugMmu: 'gs-debug-mmu',
  logsAutoscroll: 'gs-logs-autoscroll',
  fsExpanded: 'gs-fs-expanded',
  imagesCollapsed: 'gs-images-collapsed',
  checkpointsSort: 'gs-checkpoints-sort',
} as const;

const VERSION = 1;

interface Envelope<T> {
  v: number;
  data: T;
}

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

// Read a {v, data} envelope. Returns null on missing / malformed /
// wrong-version keys so callers can fall back to defaults.
function readEnvelope<T>(key: string): T | null {
  const raw = readLS(key);
  if (!raw) return null;
  try {
    const parsed = JSON.parse(raw) as Partial<Envelope<T>>;
    if (parsed.v !== VERSION) return null;
    return (parsed.data ?? null) as T | null;
  } catch {
    return null;
  }
}

function writeEnvelope<T>(key: string, data: T): void {
  writeLS(key, JSON.stringify({ v: VERSION, data }));
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

  // Phase 7 keys — all best-effort.
  const sections = readEnvelope<Record<string, boolean>>(KEYS.debugSections);
  if (sections) Object.assign(debug.sections, sections);

  const mem = readEnvelope<{ address?: number; mode?: MemoryMode }>(KEYS.debugMemory);
  if (mem) {
    if (typeof mem.address === 'number') debug.memoryAddress = mem.address;
    if (mem.mode === 'logical' || mem.mode === 'physical') debug.memoryMode = mem.mode;
  }

  const mmu = readEnvelope<{ subtab?: MmuSubtab; supervisor?: boolean }>(KEYS.debugMmu);
  if (mmu) {
    if (
      mmu.subtab === 'state' ||
      mmu.subtab === 'translate' ||
      mmu.subtab === 'map' ||
      mmu.subtab === 'descriptors'
    ) {
      debug.mmuSubtab = mmu.subtab;
    }
    if (typeof mmu.supervisor === 'boolean') debug.mmuSupervisor = mmu.supervisor;
  }

  const autoscroll = readEnvelope<boolean>(KEYS.logsAutoscroll);
  if (typeof autoscroll === 'boolean') logs.autoscroll = autoscroll;

  const fsExp = readEnvelope<Record<string, boolean>>(KEYS.fsExpanded);
  if (fsExp) filesystem.expanded = fsExp;

  const imgsColl = readEnvelope<Record<ImageCategory, boolean>>(KEYS.imagesCollapsed);
  if (imgsColl) Object.assign(images.collapsed, imgsColl);

  const cpSort = readEnvelope<{ column?: CheckpointSortColumn; dir?: SortDirection }>(
    KEYS.checkpointsSort,
  );
  if (cpSort) {
    if (
      cpSort.column === 'name' ||
      cpSort.column === 'machine' ||
      cpSort.column === 'date' ||
      cpSort.column === 'size'
    ) {
      checkpoints.sortColumn = cpSort.column;
    }
    if (cpSort.dir === 'asc' || cpSort.dir === 'desc') checkpoints.sortDir = cpSort.dir;
  }
}

// Wire up effects that mirror state changes back to localStorage. Must be
// called from a root-effect context (or from a component's $effect).
export function startPersistEffects(): void {
  $effect(() => {
    if (theme.mode === 'system') writeLS(KEYS.theme, null);
    else writeLS(KEYS.theme, theme.mode);
  });
  $effect(() => {
    writeLS(KEYS.panelPos, layout.panelPos);
  });
  $effect(() => {
    writeLS(KEYS.panelSize, JSON.stringify(layout.panelSize));
  });

  // Phase 7 effects.
  $effect(() => writeEnvelope(KEYS.debugSections, { ...debug.sections }));
  $effect(() =>
    writeEnvelope(KEYS.debugMemory, { address: debug.memoryAddress, mode: debug.memoryMode }),
  );
  $effect(() =>
    writeEnvelope(KEYS.debugMmu, { subtab: debug.mmuSubtab, supervisor: debug.mmuSupervisor }),
  );
  $effect(() => writeEnvelope(KEYS.logsAutoscroll, logs.autoscroll));
  $effect(() => writeEnvelope(KEYS.fsExpanded, { ...filesystem.expanded }));
  $effect(() => writeEnvelope(KEYS.imagesCollapsed, { ...images.collapsed }));
  $effect(() =>
    writeEnvelope(KEYS.checkpointsSort, {
      column: checkpoints.sortColumn,
      dir: checkpoints.sortDir,
    }),
  );
}
