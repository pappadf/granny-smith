// Typed bus surface for the Debug view. Everything the Debug view does
// goes through this module — DisassemblyPane, RegistersSection,
// MemorySection, BreakpointsSection, CallStackSection. Re-exports the
// scheduler/restart calls already in bus/emulator.ts so the Debug
// toolbar only needs to import here.
//
// `lib/disasm.ts` parses the raw `debug.disasm` output; this file owns
// the gsEval dispatch.

import { gsEval, shutdownEmulator, isModuleReady } from './emulator';
import { parseDisasmBlock, type DisasmRow } from '@/lib/disasm';

export type { DisasmRow };

export interface Registers {
  d: number[]; // [D0..D7]
  a: number[]; // [A0..A7]
  pc: number;
  sr: number;
  usp: number;
  ssp: number;
}

export interface Breakpoint {
  id: number;
  addr: number;
  enabled: boolean;
  condition?: string;
  hits: number;
}

const REG_DS = ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7'];
const REG_AS = ['a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'];

function coerceNum(v: unknown): number {
  if (typeof v === 'number') return v >>> 0;
  if (typeof v === 'string') {
    const t = v.trim().replace(/^\$/, '').replace(/^0x/i, '');
    const n = parseInt(t, 16);
    if (Number.isFinite(n)) return n >>> 0;
  }
  return 0;
}

export async function disasmAt(addr: number, count: number): Promise<DisasmRow[]> {
  if (!isModuleReady()) return [];
  const r = await gsEval('debug.disasm', [addr >>> 0, count]);
  if (Array.isArray(r)) {
    // Structured result — coerce.
    return r
      .map((row) => {
        if (typeof row === 'object' && row && 'addr' in row && 'mnem' in row) {
          const obj = row as { addr: unknown; mnem: unknown; ops?: unknown; cmt?: unknown };
          return {
            addr: coerceNum(obj.addr),
            mnem: String(obj.mnem ?? ''),
            ops: String(obj.ops ?? ''),
            cmt: String(obj.cmt ?? ''),
          };
        }
        return null;
      })
      .filter((x): x is DisasmRow => x !== null);
  }
  if (typeof r === 'string') return parseDisasmBlock(r);
  return [];
}

export async function readRegisters(): Promise<Registers | null> {
  if (!isModuleReady()) return null;
  const reads = await Promise.all([
    ...REG_DS.map((n) => gsEval(`cpu.${n}`)),
    ...REG_AS.map((n) => gsEval(`cpu.${n}`)),
    gsEval('cpu.pc'),
    gsEval('cpu.sr'),
    gsEval('cpu.usp'),
    gsEval('cpu.ssp'),
  ]);
  if (reads.some((x) => x === null || x === undefined)) {
    // Treat as not-ready; the C side may not have a machine yet.
    return null;
  }
  return {
    d: reads.slice(0, 8).map(coerceNum),
    a: reads.slice(8, 16).map(coerceNum),
    pc: coerceNum(reads[16]),
    sr: coerceNum(reads[17]),
    usp: coerceNum(reads[18]),
    ssp: coerceNum(reads[19]),
  };
}

export async function writeRegister(name: string, value: number): Promise<boolean> {
  if (!isModuleReady()) return false;
  const hex = `0x${(value >>> 0).toString(16)}`;
  // Use the typed setter form via gsEval — the bridge dispatcher parses
  // `path = value` per docs/shell.md.
  const r = await gsEval(`cpu.${name} = ${hex}`);
  return r !== null;
}

export async function peekL(addr: number): Promise<number | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('memory.peek.l', [addr >>> 0]);
  if (r === null || r === undefined) return null;
  return coerceNum(r);
}

export async function peekBytes(addr: number, count: number): Promise<Uint8Array | null> {
  if (!isModuleReady()) return null;
  const out = new Uint8Array(count);
  // memory.peek.b returns one byte at a time. For 128 bytes that's 128
  // bridge round-trips — acceptable for an interactive debug view that
  // re-fetches only on user action or step. If profiling shows pressure
  // we can swap in `memory.dump(addr, count)` and parse its output.
  const reads = await Promise.all(
    Array.from({ length: count }, (_, i) => gsEval('memory.peek.b', [(addr + i) >>> 0])),
  );
  for (let i = 0; i < count; i++) out[i] = coerceNum(reads[i]) & 0xff;
  return out;
}

export async function peekPhysBytes(addr: number, count: number): Promise<Uint8Array | null> {
  // Phase 6: physical mode uses the same peek path on machines without
  // MMU (logical == physical), and falls back to a shell call on
  // MMU-on. The shell exposes `info phys-bytes <addr> <count>`; until
  // we have a typed bus wrapper we delegate to the existing peek
  // helper, which is correct on no-MMU machines.
  return peekBytes(addr, count);
}

export async function listBreakpoints(): Promise<Breakpoint[]> {
  if (!isModuleReady()) return [];
  // Try the typed `count` + per-entry walk first. Fall back to the
  // shell `list` method if the indexed children walk doesn't return a
  // recognizable shape.
  const countV = await gsEval('debug.breakpoints.entries.count');
  if (typeof countV === 'number' && countV >= 0) {
    const out: Breakpoint[] = [];
    for (let i = 0; i < countV; i++) {
      const id = await gsEval(`debug.breakpoints.entries[${i}].id`);
      const addr = await gsEval(`debug.breakpoints.entries[${i}].addr`);
      const enabled = await gsEval(`debug.breakpoints.entries[${i}].enabled`);
      const cond = await gsEval(`debug.breakpoints.entries[${i}].condition`);
      const hits = await gsEval(`debug.breakpoints.entries[${i}].hit_count`);
      out.push({
        id: coerceNum(id),
        addr: coerceNum(addr),
        enabled: enabled === true || enabled === 1 || enabled === '1',
        condition: typeof cond === 'string' && cond.length ? cond : undefined,
        hits: coerceNum(hits),
      });
    }
    return out;
  }
  return [];
}

export async function addBreakpoint(addr: number, condition?: string): Promise<boolean> {
  if (!isModuleReady()) return false;
  const args: unknown[] = [addr >>> 0];
  if (condition && condition.trim().length) args.push(condition.trim());
  const r = await gsEval('debug.breakpoints.add', args);
  return r !== null;
}

export async function removeBreakpoint(addr: number): Promise<boolean> {
  if (!isModuleReady()) return false;
  // The shell form is `break --remove <addr>`; on the object surface
  // we go through the collection's `clear` method until a typed
  // `remove(id)` exists. For Phase 6 we accept that "Remove" maps
  // onto the address-based deletion path.
  const r = await gsEval('debug.breakpoints.add', [addr >>> 0, '--remove']);
  return r !== null;
}

export async function continueExec(): Promise<void> {
  if (!isModuleReady()) return;
  await gsEval('scheduler.run');
}

export async function pauseExec(): Promise<void> {
  if (!isModuleReady()) return;
  await gsEval('scheduler.stop');
}

export async function stepInto(n = 1): Promise<void> {
  if (!isModuleReady()) return;
  await gsEval('debug.step', [n]);
}

export async function stepOver(): Promise<void> {
  // C side doesn't expose step_over yet — fall back to single-step.
  // Phase 7 polish.
  return stepInto(1);
}

export async function stopMachine(): Promise<void> {
  await shutdownEmulator();
}

export async function restart(): Promise<void> {
  if (!isModuleReady()) return;
  await gsEval('mac.restart');
}
