// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Centralized constants and path conventions.
export const CONFIG = {
  // UI settings
  MAX_HISTORY_ENTRIES: 200,
  DEFAULT_ZOOM_PCT: 200,
  MIN_ZOOM_PCT: 100,
  MAX_ZOOM_PCT: 300,
  // Timeouts (milliseconds)
  RELOAD_DELAY_MS: 250,
  FIT_TERMINAL_DELAY_MS: 50,
  TOAST_DURATION_MS: 5000,
};

// Persistent boot directory layout
export const BOOT_DIR = '/persist/boot';
export const ROMS_DIR = '/persist/boot/roms';
export const CHECKPOINT_DIR = '/persist/checkpoint';
export const IMAGES_DIR = '/persist/images';

// Checkpoint file signatures: v2 (per-block RLE) and v3 (whole-file RLE)
export const CHECKPOINT_MAGIC_V2 = 'GSCHKPT2';
export const CHECKPOINT_MAGIC_V3 = 'GSCHKPT3';
export const CHECKPOINT_MAGIC = CHECKPOINT_MAGIC_V2; // legacy alias
// Shared prefix for signature detection (first 7 chars are identical)
export const CHECKPOINT_MAGIC_PREFIX = Array.from('GSCHKPT').map(c => c.charCodeAt(0));
export const CHECKPOINT_MAGIC_BYTES = Array.from(CHECKPOINT_MAGIC_V2).map(c => c.charCodeAt(0));
