// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Every localStorage key the page uses, and the two guarded accessors.  The
// keys are users' saved settings: never rename one (a rename loses what is
// stored under it) without a migration.  Their prefixes differ ('gs-' and
// 'gs.') for that reason.
export const KEYS = {
  // state/persist.svelte.ts — the original keys: plain values.
  skin: 'gs-skin',
  panelPos: 'gs-panel-pos',
  panelSize: 'gs-panel-size',
  // state/persist.svelte.ts — view-state keys, each in a `{v, data}` envelope.
  debugSections: 'gs-debug-sections',
  debugSectionsModel: 'gs-debug-sections-model',
  debugMemory: 'gs-debug-memory',
  debugMmu: 'gs-debug-mmu',
  logsAutoscroll: 'gs-logs-autoscroll',
  fsExpanded: 'gs-fs-expanded',
  imagesCollapsed: 'gs-images-collapsed',
  checkpointsSort: 'gs-checkpoints-sort',
  micDevice: 'gs-mic-device',
  recentMachines: 'gs-recent-machines',
  // Modules' own records.
  remoteAfp: 'gs-remote-afp', // bus/remoteAfp.ts settings (JSON)
  checkpointMachine: 'gs.checkpoint.machine', // lib/machineId.ts identity (JSON)
  consoleHistory: 'gs.console.history', // lib/consoleHistory.ts (JSON array)
  confirmPowerOff: 'gs-confirm-power-off', // state/powerConfirm.ts opt-out ('1')
  previewNoticeDismissed: 'gs-preview-notice-dismissed-v1', // PreviewNoticeDialog ('1')
} as const;

// The stored string, or null when absent or storage is unavailable.
export function readLS(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

// Store `value` (null removes the key); a no-op when storage is unavailable
// (private mode, quota).
export function writeLS(key: string, value: string | null): void {
  try {
    if (value === null) localStorage.removeItem(key);
    else localStorage.setItem(key, value);
  } catch {
    // localStorage unavailable (private mode, quota) — silently skip.
  }
}
