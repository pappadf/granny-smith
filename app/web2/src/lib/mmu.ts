// Pure decoders for 68030 PMMU register words. No bus access; consumers
// feed in the raw register values (bus/mmu.ts reads them) and receive
// {field: value} records. Unit-tested.
//
// Bit layouts follow the M68030 user manual / docs/memory.md:
//   TC (32-bit): E[31] SRE[25] FCL[24] PS[23..20] IS[19..16]
//                TIA[15..12] TIB[11..8] TIC[7..4] TID[3..0]
//   CRP/SRP: a 64-bit pair; the high word carries limit + DT, the low
//            word the root pointer.

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

// CRP or SRP from its two 32-bit words, e.g. high $00000002 (limit 0,
// DT 2 — short table) and low $001FE000 (root pointer).
export function decodeRootPointer(high: number, low: number): DecodedRootPointer {
  return {
    limit: (high >>> 16) & 0x7fff,
    dt: high & 0x3,
    pointer: low >>> 0,
  };
}
