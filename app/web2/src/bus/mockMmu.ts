// Mock MMU data for the Debug view. Phase 6 ships the UI structure
// against this fixture; Phase 7 replaces it with real C-side wiring
// (mmu.translate / mmu.walk / mmu.map / mmu.descriptors) without
// touching the consuming components.
//
// Layout matches the prototype's fixtures (app.js lines 340-372) — an
// SE/30-with-System-7-ish demo layout: low memory + I/O TT-mapped,
// user code + shlib PT-mapped, top of address space TT-mapped.

import { dtName } from '@/lib/mmu';

export interface MmuMapping {
  lo: number;
  hi: number;
  base: number;
  kind: 'TT' | 'PT';
  flags: string[];
}

export interface LookupResult {
  valid: boolean;
  phys?: number;
  kind?: 'TT' | 'PT';
  flags?: string[];
}

export interface WalkLevel {
  idx: number;
  descriptorAddr: number;
  descriptorWord: number;
  dt: number;
  next?: number;
}

export interface WalkResult {
  logical: number;
  physical?: number;
  valid: boolean;
  root: 'CRP' | 'SRP';
  levels: WalkLevel[];
  kind?: 'TT' | 'PT' | 'INVALID';
}

export interface DescriptorRow {
  addr: number;
  word: number;
  dt: number;
  dtLabel: ReturnType<typeof dtName>;
  next?: number;
  phys?: number;
  flags?: { U: number; WP: number; M: number; CI: number };
}

export const mmuMappings: MmuMapping[] = [
  { lo: 0x00000000, hi: 0x001fffff, base: 0x00000000, kind: 'TT', flags: ['S'] },
  { lo: 0x00400000, hi: 0x00ffffff, base: 0x50000000, kind: 'PT', flags: ['U=1', 'M=1'] },
  { lo: 0x47f00000, hi: 0x47f1ffff, base: 0x00100000, kind: 'PT', flags: ['U=1'] },
  { lo: 0xf0000000, hi: 0xffffffff, base: 0xf0000000, kind: 'TT', flags: [] },
];

export const mmuRegs = {
  enabled: true,
  tc: 0x80008307, // E=1, SRE=0, FCL=0, PS=13 (8K pages), IS=0, TIA=4 TIB=4 TIC=3 TID=0
  crp: '00000002_001FE000',
  srp: '00000002_001FE000',
  tt0: 0x00c07fe0,
  tt1: 0x00000000,
  pageSize: 8192,
  levels: 2,
};

export function mmuLookup(logical: number): LookupResult {
  const a = logical >>> 0;
  for (const m of mmuMappings) {
    if (a >= m.lo && a <= m.hi) {
      return { valid: true, phys: (m.base + (a - m.lo)) >>> 0, kind: m.kind, flags: m.flags };
    }
  }
  return { valid: false };
}

export function mmuWalk(logical: number, supervisor: boolean): WalkResult {
  const a = logical >>> 0;
  const lookup = mmuLookup(a);
  const root: 'CRP' | 'SRP' = supervisor ? 'SRP' : 'CRP';
  if (!lookup.valid) {
    return { logical: a, valid: false, root, levels: [], kind: 'INVALID' };
  }
  if (lookup.kind === 'TT') {
    return { logical: a, physical: lookup.phys, valid: true, root, levels: [], kind: 'TT' };
  }
  // Synthetic 2-level walk for PT mappings. Descriptor addresses are
  // mockups but follow the prototype's shape so the UI can render full
  // walk paths.
  const tableBase = 0x001fe000;
  const ix0 = (a >>> 24) & 0xff;
  const ix1 = (a >>> 16) & 0xff;
  const lvl0Addr = (tableBase + ix0 * 4) >>> 0;
  const lvl1Base = 0x001ff000;
  const lvl1Addr = (lvl1Base + ix1 * 4) >>> 0;
  return {
    logical: a,
    physical: lookup.phys,
    valid: true,
    root,
    kind: 'PT',
    levels: [
      {
        idx: ix0,
        descriptorAddr: lvl0Addr,
        descriptorWord: (lvl1Base | 0x3) >>> 0,
        dt: 3,
        next: lvl1Base,
      },
      {
        idx: ix1,
        descriptorAddr: lvl1Addr,
        descriptorWord: (((lookup.phys ?? 0) >>> 12) << 12) | 0x1,
        dt: 1,
      },
    ],
  };
}

export function mmuMap(start: number, end: number): MmuMapping[] {
  const s = start >>> 0;
  const e = end >>> 0;
  return mmuMappings.filter((m) => m.hi >= s && m.lo <= e);
}

// Synthetic descriptor decode — count entries starting at `addr`, 4-byte
// stride. Generates a believable mix of TABLE / PAGE / INVALID rows.
export function mmuDescriptors(addr: number, count: number): DescriptorRow[] {
  const out: DescriptorRow[] = [];
  for (let i = 0; i < count; i++) {
    const a = (addr + i * 4) >>> 0;
    // Cycle through DT kinds for demo variety.
    const dt = i % 4;
    const row: DescriptorRow = { addr: a, word: 0, dt, dtLabel: dtName(dt) };
    if (dt === 0) {
      row.word = 0;
    } else if (dt === 1) {
      const phys = 0x00100000 + i * 0x2000;
      row.word = ((phys >>> 12) << 12) | 0x1;
      row.phys = phys >>> 0;
      row.flags = { U: i & 1, WP: 0, M: i & 1, CI: 0 };
    } else if (dt === 2) {
      const next = 0x001ff000 + i * 0x100;
      row.word = (next | 0x2) >>> 0;
      row.next = next >>> 0;
    } else {
      const next = 0x001fe000 + i * 0x80;
      row.word = (next | 0x3) >>> 0;
      row.next = next >>> 0;
    }
    out.push(row);
  }
  return out;
}
