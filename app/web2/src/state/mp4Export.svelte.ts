// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export, as the page shows it: whether this browser can export at
// all (the probe, which "Export MP4…" follows), and the export dialog --
// the recordings it is for, what the worker found in them, the size
// chosen, and an export's progress.  Driven by recorder/mp4Export.ts; the
// dialog is components/dialogs/ExportMp4Dialog.svelte.

import type { ExportFileInfo } from '@/recorder/exportProtocol';
import type { ExportSupport } from '@/recorder/probe';

// loading: the worker reads the recordings and probes the sizes; choose:
// the size choice; running: encoding; failed: the reason is in `error`.
export type ExportPhase = 'idle' | 'loading' | 'choose' | 'running' | 'failed';

export interface Mp4ExportState {
  // The browser's answer, null until the probe has run.
  support: ExportSupport | null;
  open: boolean;
  phase: ExportPhase;
  paths: string[]; // the recordings, one .mp4 each
  files: ExportFileInfo[];
  scale: number; // 1, 2, 3 or 4
  file: number; // the recording being encoded (0-based)
  done: number; // its frames encoded
  total: number; // its frames
  error: string;
}

export const mp4Export: Mp4ExportState = $state({
  support: null,
  open: false,
  phase: 'idle',
  paths: [],
  files: [],
  scale: 1,
  file: 0,
  done: 0,
  total: 0,
  error: '',
});

// For the tests.
export function _resetMp4ExportForTests(): void {
  Object.assign(mp4Export, {
    support: null,
    open: false,
    phase: 'idle',
    paths: [],
    files: [],
    scale: 1,
    file: 0,
    done: 0,
    total: 0,
    error: '',
  });
}
