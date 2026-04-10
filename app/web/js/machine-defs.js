// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Machine model definitions for the configuration dialog.
// Each entry describes the capabilities and options for a machine model.

export const MACHINE_DEFS = {
  plus: {
    displayName: 'Macintosh Plus',
    hasVrom: false,
    ramOptions: [1, 2, 2.5, 4],       // MB
    defaultRam: 4,
    floppyDirs: ['fd'],                // 400K/800K only
    floppySlots: ['Internal FD0', 'External FD1'],
    scsiSlots: ['SCSI HD0', 'SCSI HD1'],
    hasCdrom: false,
  },
  se30: {
    displayName: 'Macintosh SE/30',
    hasVrom: true,
    ramOptions: [1, 2, 4, 8, 16, 32, 64, 128],  // MB
    defaultRam: 8,
    floppyDirs: ['fd', 'fdhd'],        // combined 400/800K + 1.4M
    floppySlots: ['Internal FD0', 'External FD1'],
    scsiSlots: ['SCSI HD0', 'SCSI HD1'],
    hasCdrom: true,
  },
};
