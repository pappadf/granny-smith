// Pure decoders for 68030 PMMU register words + address-pair formatting
// helpers. No bus access; consumers feed in the raw register values and
// receive {field: value} records. Unit-tested.
//
// Bit layouts follow the M68030 user manual / docs/memory.md:
//   TC (32-bit): E[31] SRE[25] FCL[24] PS[23..20] IS[19..16]
//                TIA[15..12] TIB[11..8] TIC[7..4] TID[3..0]
//   CRP/SRP: 64-bit pair shown as "limit_dt_pointer" hex string in the
//            prototype's fixture. We decode the high 16 bits (limit + DT)
//            and the low 32 bits (pointer).

import { fmtHex32 } from './hex';

export interface DecodedTc {
  E: number;
  SRE: number;
  FCL: number;
  PS: number; // page-size shift; PS=13 means 8 KB pages
  IS: number; // initial shift (skipped MSBs)
  TIA: number;
  TIB: number;
  TIC: number;
  TID: number;
}

export function decodeTc(tc: number): DecodedTc {
  const v = tc >>> 0;
  return {
    E: (v >>> 31) & 1,
    SRE: (v >>> 25) & 1,
    FCL: (v >>> 24) & 1,
    PS: (v >>> 20) & 0xf,
    IS: (v >>> 16) & 0xf,
    TIA: (v >>> 12) & 0xf,
    TIB: (v >>> 8) & 0xf,
    TIC: (v >>> 4) & 0xf,
    TID: v & 0xf,
  };
}

export interface DecodedRootPointer {
  limit: number;
  dt: number;
  pointer: number;
}

// The prototype encodes CRP/SRP as a two-word hex string joined by `_`:
//   high = "00000002" (limit=0, DT=2 — short table)
//   low  = "001FE000" (root pointer)
export function decodeCrpFromHex(hex: string): DecodedRootPointer {
  const parts = hex.split('_');
  const high = parts[0] ? parseInt(parts[0], 16) >>> 0 : 0;
  const low = parts[1] ? parseInt(parts[1], 16) >>> 0 : 0;
  return {
    limit: (high >>> 16) & 0x7fff,
    dt: high & 0x3,
    pointer: low >>> 0,
  };
}

// DT field for PMMU descriptors:
//   0 = INVALID
//   1 = PAGE (page descriptor)
//   2 = TABLE8 (8-byte child entries)
//   3 = TABLE4 (4-byte child entries)
export function dtName(dt: number): 'INVALID' | 'PAGE' | 'TABLE8' | 'TABLE4' {
  if (dt === 0) return 'INVALID';
  if (dt === 1) return 'PAGE';
  if (dt === 2) return 'TABLE8';
  return 'TABLE4';
}

// Address-pair label for views that show L:$ and P:$ side by side.
// Returns plain object so consumers can render however they like.
export interface AddrPair {
  logical: string;
  physical?: string;
  tag?: 'TT' | 'PT' | 'INVALID';
}

export function fmtAddrPair(
  logical: number,
  lookup?: { valid: boolean; phys?: number; kind?: 'TT' | 'PT' },
): AddrPair {
  if (!lookup) {
    return { logical: `$${fmtHex32(logical)}` };
  }
  if (!lookup.valid) {
    return { logical: `L:$${fmtHex32(logical)}`, tag: 'INVALID' };
  }
  return {
    logical: `L:$${fmtHex32(logical)}`,
    physical: `P:$${fmtHex32(lookup.phys ?? 0)}`,
    tag: lookup.kind ?? 'PT',
  };
}

// Range-pair label for views that show L:$<lo>–$<hi> P:$<plo>–$<phi>.
export interface RangePair {
  l: string;
  p?: string;
  size: string;
}

export function fmtRangePair(lo: number, hi: number, base: number, mmuOn: boolean): RangePair {
  const size = (hi - lo + 1) >>> 0;
  if (!mmuOn) {
    return { l: `$${fmtHex32(lo)} – $${fmtHex32(hi)}`, size: humanSize(size) };
  }
  const physEnd = (base + size - 1) >>> 0;
  return {
    l: `L:$${fmtHex32(lo)} – $${fmtHex32(hi)}`,
    p: `P:$${fmtHex32(base)} – $${fmtHex32(physEnd)}`,
    size: humanSize(size),
  };
}

export function humanSize(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}
