// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ROM checksum → machine model database.
// Mirrors the C-side ROM_TABLE in src/core/memory/rom.c.
// Must be kept in sync manually.

export const ROM_DATABASE = {
  '4D1EEEE1': { models: ['plus'], name: 'Macintosh Plus (Rev 1, Lonely Hearts)', size: 128 * 1024 },
  '4D1EEAE1': { models: ['plus'], name: 'Macintosh Plus (Rev 2, Lonely Heifers)', size: 128 * 1024 },
  '4D1F8172': { models: ['plus'], name: 'Macintosh Plus (Rev 3, Loud Harmonicas)', size: 128 * 1024 },
  '97221136': { models: ['se30', 'iicx'], name: 'Macintosh SE/30 (Universal ROM)', size: 256 * 1024 },
};

// Given a list of ROM checksums present on disk, derive the set of
// available machine models with their associated ROM checksum.
// Returns a Map of model_id → { romChecksum, romName }.
export function getAvailableModels(romChecksums) {
  const models = new Map();
  for (const cs of romChecksums) {
    const entry = ROM_DATABASE[cs.toUpperCase()];
    if (!entry) continue;
    for (const model of entry.models) {
      if (!models.has(model)) {
        models.set(model, { romChecksum: cs.toUpperCase(), romName: entry.name });
      }
    }
  }
  return models;
}
