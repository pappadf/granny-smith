// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The messages between the page (recorder/mp4Export.ts) and the MP4 export
// worker (recorder/export.worker.ts).

import type { RecordingInfo } from './recdec';
import type { AudioCodec } from './probe';

// One recording to export: the .mkv, the .mp4 to write, and the scratch
// file the samples wait in.
export interface ExportJob {
  src: string;
  dst: string;
  tmp: string;
}

// One size the dialog offers, and whether this browser can encode it.
export interface ExportSizeOption {
  scale: number;
  width: number;
  height: number;
  ok: boolean;
}

// What the worker found in a recording (`error` when it cannot be read).
export interface ExportFileInfo {
  path: string;
  info?: RecordingInfo;
  error?: string;
  sizes: ExportSizeOption[];
  // The audio it would be encoded as; null for video only (no encoder, or
  // no sound in the recording).
  audio: AudioCodec | null;
}

// A file written.
export interface ExportOutput {
  src: string;
  dst: string;
  bytes: number;
  frames: number;
  width: number;
  height: number;
  audio: AudioCodec | null;
  hadAudio: boolean;
}

export type WorkerIn =
  | { type: 'info'; moduleUrl: string; wasmUrl: string; paths: string[] }
  | { type: 'export'; moduleUrl: string; wasmUrl: string; jobs: ExportJob[]; scale: number }
  | { type: 'cancel' };

export type WorkerOut =
  | { type: 'info'; files: ExportFileInfo[] }
  | { type: 'progress'; file: number; files: number; done: number; total: number }
  | { type: 'done'; outputs: ExportOutput[] }
  | { type: 'failed'; message: string }
  | { type: 'cancelled' };
