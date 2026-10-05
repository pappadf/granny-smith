// The MMU, as the core exposes it on every MMU kind.
//
// machine.cpu.mmu.translate, .walk, .map, .descriptor and .peek have the same
// signatures and result shapes on the 68030, the 68040, the PowerPC 601/604
// and the Lisa's segment MMU, so nothing here branches on the architecture
// except which registers the State tab lists and which descriptor formats
// the Descriptors tab offers.  This replaces bus/mockMmu.ts, whose hand-written
// SE/30 fixtures the MMU tabs, and the L:/P: labels next to breakpoints,
// stack frames and memory rows, used to show as if they were live.

import { gsEval, isGsError, isModuleReady } from './emulator';
import { fmtHex32 } from '@/lib/hex';
import type { MmuKind } from '@/state/machine.svelte';

// One translated address: `phys` only when valid; `via` says how it resolved
// (identity / tt / bat / segment / page); `access` what the privilege may do
// there (rw / ro / none); `space` on the Lisa (ram / io / rom).
export interface Translation {
  phys?: number;
  valid: boolean;
  via: string;
  access?: string;
  space?: string;
}

// A number as the bridge delivers it: the core marks addresses and register
// images for hex display, and the JSON encoding carries those as "0x..."
// strings; other integers arrive as numbers.
export function bridgeNum(v: unknown): number | undefined {
  if (typeof v === 'number') return v;
  if (typeof v === 'string' && /^0x[0-9a-f]+$/i.test(v)) return Number(v);
  return undefined;
}

// A 32-bit address from the bridge.
function addr32(v: unknown): number | undefined {
  const n = bridgeNum(v);
  return n === undefined ? undefined : n >>> 0;
}

// The fields of a Translation, read from a core result map.
function toTranslation(o: Record<string, unknown>): Translation {
  return {
    phys: addr32(o.phys),
    valid: o.valid === true,
    via: typeof o.via === 'string' ? o.via : '',
    access: typeof o.access === 'string' ? o.access : undefined,
    space: typeof o.space === 'string' ? o.space : undefined,
  };
}

// The core's argument map for (addr, supervisor?, fetch?).
function modeArgs(supervisor?: boolean, fetch?: boolean): Record<string, unknown> {
  const args: Record<string, unknown> = {};
  if (supervisor !== undefined) args.supervisor = supervisor;
  if (fetch) args.fetch = true;
  return args;
}

// Translate one logical address.  `supervisor` omitted: the CPU's state.
export async function translateAddr(
  addr: number,
  supervisor?: boolean,
): Promise<Translation | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.cpu.mmu.translate', {
    addr: addr >>> 0,
    ...modeArgs(supervisor),
  });
  if (!r || typeof r !== 'object' || isGsError(r)) return null;
  return toTranslation(r as Record<string, unknown>);
}

// One step of a walk: what was consulted (`step`: tt / root / level / bat /
// segment / pteg), what it decided (`outcome`: miss / next / hit / fault),
// and the fields the MMU read or derived there (name, index, addr, desc,
// type, phys, reason, ...) -- numbers and flags as the core reported them.
export interface WalkStep {
  step: string;
  outcome: string;
  fields: Record<string, number | boolean | string>;
}

// A translation with the steps that produced it.
export interface Walk extends Translation {
  steps: WalkStep[];
}

// Keep a core value that is a number (hex strings read as numbers), flag or
// string.
function scalarFields(
  o: Record<string, unknown>,
  skip: string[],
): Record<string, number | boolean | string> {
  const out: Record<string, number | boolean | string> = {};
  for (const [k, v] of Object.entries(o)) {
    if (skip.includes(k)) continue;
    const n = bridgeNum(v);
    if (n !== undefined) out[k] = n;
    else if (typeof v === 'boolean' || typeof v === 'string') out[k] = v;
  }
  return out;
}

// Walk one logical address: the translation plus every step the MMU took.
export async function walkAddr(
  addr: number,
  supervisor?: boolean,
  fetch?: boolean,
): Promise<Walk | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.cpu.mmu.walk', {
    addr: addr >>> 0,
    ...modeArgs(supervisor, fetch),
  });
  if (!r || typeof r !== 'object' || isGsError(r)) return null;
  const o = r as Record<string, unknown>;
  const steps = Array.isArray(o.steps) ? o.steps : [];
  return {
    ...toTranslation(o),
    steps: steps
      .filter((s): s is Record<string, unknown> => !!s && typeof s === 'object')
      .map((s) => ({
        step: String(s.step ?? ''),
        outcome: String(s.outcome ?? ''),
        fields: scalarFields(s, ['step', 'outcome']),
      })),
  };
}

// One mapped run: [start, start + size) translates linearly to phys.
export interface MapRun {
  start: number;
  size: number;
  phys: number;
  via: string;
  access: string;
  space?: string;
}

// The most runs one map call lists (the core's default limit).
export const MAP_LIMIT = 512;

// The mapped runs of [start, end) -- the whole space when `end` is omitted.
export async function mapRange(
  start: number,
  end: number | undefined,
  supervisor?: boolean,
): Promise<MapRun[] | null> {
  if (!isModuleReady()) return null;
  const args: Record<string, unknown> = {
    start: start >>> 0,
    ...modeArgs(supervisor),
    limit: MAP_LIMIT,
  };
  if (end !== undefined) args.end = end;
  const r = await gsEval('machine.cpu.mmu.map', args);
  if (!Array.isArray(r)) return null;
  return r
    .filter((x): x is Record<string, unknown> => !!x && typeof x === 'object')
    .map((x) => ({
      start: addr32(x.start) ?? 0,
      size: bridgeNum(x.size) ?? 0,
      phys: addr32(x.phys) ?? 0,
      via: String(x.via ?? ''),
      access: String(x.access ?? ''),
      space: typeof x.space === 'string' ? x.space : undefined,
    }));
}

// The descriptor formats each MMU kind's descriptor() accepts (its `format`
// argument), first = the default.  The Lisa's descriptors live in the MMU,
// so its "address" is a segment number and it takes no format.
export function descriptorFormats(kind: MmuKind): string[] {
  switch (kind) {
    case '68030_pmmu':
      return ['short', 'long'];
    case '68040':
      return ['page', 'pointer', 'root'];
    case 'ppc_601':
    case 'ppc_604':
      return ['pte'];
    default:
      return [];
  }
}

// One decoded descriptor: every field the core reported (addr, desc, type,
// next or phys, flags, ...).
export type Descriptor = Record<string, number | boolean | string>;

// Decode `count` descriptors at `addr` (a physical address, or the Lisa's
// segment number), read as `format` when the kind has formats.
export async function readDescriptors(
  addr: number,
  count: number,
  format?: string,
): Promise<Descriptor[] | null> {
  if (!isModuleReady()) return null;
  const args: unknown[] = [addr >>> 0, count];
  if (format) args.push(format);
  const r = await gsEval('machine.cpu.mmu.descriptor', args);
  if (!Array.isArray(r)) return null;
  return r
    .filter((x): x is Record<string, unknown> => !!x && typeof x === 'object')
    .map((x) => scalarFields(x, []));
}

// Translate several addresses (each once), for a list of labels.
export async function translateMany(addrs: number[]): Promise<Record<number, Translation>> {
  const out: Record<number, Translation> = {};
  for (const a of addrs) {
    const key = a >>> 0;
    if (key in out) continue;
    const t = await translateAddr(key);
    if (t) out[key] = t;
  }
  return out;
}

// "L:$logical  P:$physical  VIA" for an MMU machine; the plain address when
// the translation is not known (yet), or "…  INVALID" when there is none.
export function addrLabel(addr: number, t: Translation | undefined): string {
  if (!t) return `$${fmtHex32(addr)}`;
  if (!t.valid) return `L:$${fmtHex32(addr)}  INVALID`;
  const where = t.space ? ` ${t.space}` : '';
  return `L:$${fmtHex32(addr)}  P:$${fmtHex32(t.phys ?? 0)}  ${t.via.toUpperCase()}${where}`;
}

// One MMU register for the State tab.
export interface MmuRegister {
  name: string;
  value: number;
}

// Which registers the State tab reads, per MMU kind, and from which node.
// The 68K MMUs hang their registers on machine.cpu.mmu; the PowerPC keeps
// its MMU registers on machine.cpu; the Lisa's are its START latch and
// context.  (A kind with no entry shows nothing, rather than guessing.)
function registerPaths(kind: MmuKind): string[] {
  const range = (prefix: string, n: number, suffixes: string[] = ['']) =>
    Array.from({ length: n }, (_, i) => suffixes.map((s) => `${prefix}${i}${s}`)).flat();
  switch (kind) {
    case '68030_pmmu':
      return ['tc', 'crp_hi', 'crp_lo', 'srp_hi', 'srp_lo', 'tt0', 'tt1', 'mmusr'].map(
        (n) => `machine.cpu.mmu.${n}`,
      );
    case '68040':
      return ['tc', 'urp', 'srp', 'itt0', 'itt1', 'dtt0', 'dtt1', 'mmusr'].map(
        (n) => `machine.cpu.mmu.${n}`,
      );
    case 'ppc_601':
      return ['msr', 'sdr1', ...range('sr', 16), ...range('bat', 4, ['u', 'l'])].map(
        (n) => `machine.cpu.${n}`,
      );
    case 'ppc_604':
      return [
        'msr',
        'sdr1',
        ...range('sr', 16),
        ...range('bat', 4, ['u', 'l']),
        ...range('dbat', 4, ['u', 'l']),
      ].map((n) => `machine.cpu.${n}`);
    case 'lisa_segment':
      return ['start', 'context'].map((n) => `machine.cpu.mmu.${n}`);
    default:
      return [];
  }
}

// Read the MMU's registers for the State tab.
export async function readMmuState(kind: MmuKind): Promise<MmuRegister[]> {
  if (!isModuleReady()) return [];
  const out: MmuRegister[] = [];
  for (const path of registerPaths(kind)) {
    const v = await gsEval(path);
    if (isGsError(v)) continue;
    const n = addr32(v);
    const value = n !== undefined ? n : v === true ? 1 : v === false ? 0 : NaN;
    if (Number.isNaN(value)) continue;
    out.push({ name: path.slice(path.lastIndexOf('.') + 1), value });
  }
  return out;
}
