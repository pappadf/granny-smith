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

// === Walk steps, map runs and descriptors ===================================
//
// The core's walk / map / descriptor results carry the same field names on
// every MMU kind (machine.cpu.mmu, debug_mmu.h), so one formatter serves the
// 68030, the 68040, the PowerPC and the Lisa.  Numbers arrive as plain
// numbers; which of them read as hex follows from the field's name.

// Fields that hold an address, a descriptor word or a register image.
const HEX_FIELDS = new Set([
  'addr',
  'desc',
  'desc_lo',
  'next',
  'phys',
  'size',
  'value',
  'vsid',
  'buid',
  'hash',
  'sor',
  'slr',
  'base',
  'ea',
]);

// Fields a step or descriptor line already shows in its main text.
const MAIN_FIELDS = new Set([
  'name',
  'index',
  'addr',
  'desc',
  'desc_lo',
  'type',
  'next',
  'phys',
  'reason',
]);

// One field's value as text: $-hex for addresses and words, else as is.
export function fmtField(key: string, v: number | boolean | string): string {
  if (typeof v === 'number' && HEX_FIELDS.has(key))
    return `$${(v >>> 0)
      .toString(16)
      .toUpperCase()
      .padStart(key === 'size' ? 1 : 8, '0')}`;
  return String(v);
}

// The fields not in the main text, as "key=value" (flags as their name when
// set, omitted when clear).
export function extraFields(fields: Record<string, number | boolean | string>): string[] {
  const out: string[] = [];
  for (const [k, v] of Object.entries(fields)) {
    if (MAIN_FIELDS.has(k)) continue;
    if (typeof v === 'boolean') {
      if (v) out.push(k.toUpperCase());
      continue;
    }
    out.push(`${k}=${fmtField(k, v)}`);
  }
  return out;
}

// One walk step or descriptor as a line of text: what it is, where it was
// read, what it held, and where it led.
export interface FormattedEntry {
  title: string; // "LEVEL A", "PTEG primary", "TT", ...
  main: string; // "[3] @$00001008 = $00002003 table → $00002000"
  extra: string[]; // remaining fields
}

export function formatEntry(
  label: string,
  fields: Record<string, number | boolean | string>,
): FormattedEntry {
  const f = fields;
  const title = [label.toUpperCase(), f.name !== undefined ? String(f.name) : '']
    .filter(Boolean)
    .join(' ');
  const parts: string[] = [];
  if (f.index !== undefined) parts.push(`[${f.index}]`);
  if (f.addr !== undefined) parts.push(`@${fmtField('addr', f.addr)}`);
  if (f.desc !== undefined)
    parts.push(
      `= ${fmtField('desc', f.desc)}${f.desc_lo !== undefined ? ` ${fmtField('desc_lo', f.desc_lo)}` : ''}`,
    );
  if (f.type !== undefined) parts.push(String(f.type));
  if (f.next !== undefined) parts.push(`→ ${fmtField('next', f.next)}`);
  if (f.phys !== undefined) parts.push(`→ P:${fmtField('phys', f.phys)}`);
  if (f.reason !== undefined) parts.push(`(${f.reason})`);
  return { title, main: parts.join(' '), extra: extraFields(fields) };
}

// A map run's end address (exclusive), as a 33-bit number.
export function runEnd(start: number, size: number): number {
  return (start >>> 0) + size;
}

// A byte count as K/M/G when exact, else hex.
export function fmtSize(size: number): string {
  const units: [number, string][] = [
    [2 ** 30, 'G'],
    [2 ** 20, 'M'],
    [2 ** 10, 'K'],
  ];
  for (const [n, u] of units) if (size >= n && size % n === 0) return `${size / n}${u}`;
  return `$${size.toString(16).toUpperCase()}`;
}
