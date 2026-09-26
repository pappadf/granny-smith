// The MMU, as the core exposes it on every MMU kind (11-WORK-ORDER D3/D5).
//
// machine.cpu.mmu.translate and .peek have the same signatures and result
// shapes on the 68030, the 68040, the PowerPC 601/604 and the Lisa's segment
// MMU, so nothing here branches on the architecture except which registers
// the State tab lists.  This replaces bus/mockMmu.ts, whose hand-written
// SE/30 fixtures the MMU tabs, and the L:/P: labels next to breakpoints,
// stack frames and memory rows, used to show as if they were live (F-07).

import { gsEval, isGsError, isModuleReady } from './emulator';
import { fmtHex32 } from '@/lib/hex';
import type { MmuKind } from '@/state/machine.svelte';

// One translated address: `phys` only when valid; `via` says how it resolved
// (identity / tt / bat / segment / page); `space` on the Lisa (ram / io / rom).
export interface Translation {
  phys?: number;
  valid: boolean;
  via: string;
  space?: string;
}

// Translate one logical address.  `supervisor` omitted: the CPU's state.
export async function translateAddr(
  addr: number,
  supervisor?: boolean,
): Promise<Translation | null> {
  if (!isModuleReady()) return null;
  const args: Record<string, unknown> = { addr: addr >>> 0 };
  if (supervisor !== undefined) args.supervisor = supervisor;
  const r = await gsEval('machine.cpu.mmu.translate', args);
  if (!r || typeof r !== 'object' || isGsError(r)) return null;
  const o = r as { phys?: unknown; valid?: unknown; via?: unknown; space?: unknown };
  return {
    phys: typeof o.phys === 'number' ? o.phys >>> 0 : undefined,
    valid: o.valid === true,
    via: typeof o.via === 'string' ? o.via : '',
    space: typeof o.space === 'string' ? o.space : undefined,
  };
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
    const value = typeof v === 'number' ? v >>> 0 : v === true ? 1 : v === false ? 0 : NaN;
    if (Number.isNaN(value)) continue;
    out.push({ name: path.slice(path.lastIndexOf('.') + 1), value });
  }
  return out;
}
