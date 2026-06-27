// Typed bus surface for the Debug view. Everything the Debug view does
// goes through this module — DisassemblyPane, RegistersSection,
// MemorySection, BreakpointsSection, CallStackSection. Re-exports the
// scheduler/restart calls already in bus/emulator.ts so the Debug
// toolbar only needs to import here.
//
// `lib/disasm.ts` parses the raw `debug.disasm` output; this file owns
// the gsEval dispatch.

import {
  gsEval,
  shutdownEmulator,
  isModuleReady,
  initEmulator,
  getLastBootConfig,
} from './emulator';
import { parseDisasmBlock, type DisasmRow } from '@/lib/disasm';
import { bumpDebugRefresh } from '@/state/debug.svelte';

export type { DisasmRow };

export interface Registers {
  d: number[]; // [D0..D7]
  a: number[]; // [A0..A7]
  pc: number;
  sr: number;
  usp: number;
  ssp: number;
}

// One row of the bundled debug frame — disassembly line plus
// per-instruction MMU translation (`phys` null when the walk failed).
export interface DebugFrameRow {
  addr: number;
  phys: number | null;
  valid: boolean;
  mnem: string;
  ops: string;
}

// One FPU data register, returned by `debug.frame` for CPUs that have
// an FPU. `hex` is the raw 80-bit register (4 hex digits of exponent
// with sign + underscore + 16 hex digits of mantissa). `val` is the
// human-readable decimal (or special form like `Inf` / `-Inf` / `NaN`).
export interface FpuRegister {
  hex: string;
  val: string;
}

export interface FpuFrame {
  fp: FpuRegister[]; // 8 entries, fp0..fp7
  fpcr: number;
  fpsr: number;
  fpiar: number;
}

// Bundled snapshot returned by `debug.frame`. Replaces the per-register
// + per-row gsEval fan-out the Debug view used to do (~21 round-trips)
// with a single bridge call.
export interface DebugFrame {
  regs: Registers;
  rows: DebugFrameRow[];
  // Present only when the running CPU model has an FPU (68030 with
  // built-in 68882 — SE/30, IIcx, IIfx). 68000 machines (Plus, SE)
  // never see this field.
  fpu?: FpuFrame;
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

// Fetch the bundled debug frame in a single bridge round-trip. Default
// is 32 rows starting at PC. When `addr` is omitted the C side uses PC.
// Returns null on parse failure or when the module isn't ready.
export async function loadDebugFrame(addr?: number, count = 32): Promise<DebugFrame | null> {
  if (!isModuleReady()) return null;
  const args: number[] = addr === undefined ? [] : [addr >>> 0, count];
  if (addr === undefined && count !== 32) args.push(0, count);
  const r = await gsEval('debug.frame', args.length ? args : undefined);
  if (typeof r !== 'string') return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(r);
  } catch {
    return null;
  }
  if (!parsed || typeof parsed !== 'object') return null;
  const obj = parsed as { regs?: Record<string, number>; rows?: unknown[] };
  if (!obj.regs || !Array.isArray(obj.rows)) return null;
  const regs: Registers = {
    d: Array.from({ length: 8 }, (_, i) => coerceNum(obj.regs![`d${i}`])),
    a: Array.from({ length: 8 }, (_, i) => coerceNum(obj.regs![`a${i}`])),
    pc: coerceNum(obj.regs.pc),
    sr: coerceNum(obj.regs.sr),
    usp: coerceNum(obj.regs.usp),
    ssp: coerceNum(obj.regs.ssp),
  };
  const rows: DebugFrameRow[] = obj.rows
    .map((row) => {
      if (!row || typeof row !== 'object') return null;
      const o = row as {
        addr: unknown;
        phys: unknown;
        valid: unknown;
        mnem: unknown;
        ops: unknown;
      };
      return {
        addr: coerceNum(o.addr),
        phys: o.phys === null || o.phys === undefined ? null : coerceNum(o.phys),
        valid: o.valid === true,
        mnem: String(o.mnem ?? ''),
        ops: String(o.ops ?? ''),
      };
    })
    .filter((x): x is DebugFrameRow => x !== null);

  // Parse the optional FPU block. The C side emits it only when the
  // running CPU model has an FPU; on Plus / SE the field is missing.
  let fpu: FpuFrame | undefined;
  const fpuObj = (parsed as { fpu?: unknown }).fpu;
  if (fpuObj && typeof fpuObj === 'object') {
    const fpuRaw = fpuObj as { fp?: unknown; fpcr?: unknown; fpsr?: unknown; fpiar?: unknown };
    const fpList = Array.isArray(fpuRaw.fp) ? fpuRaw.fp : [];
    const fp: FpuRegister[] = fpList.map((entry) => {
      if (!entry || typeof entry !== 'object') return { hex: '', val: '' };
      const e = entry as { hex?: unknown; val?: unknown };
      return { hex: String(e.hex ?? ''), val: String(e.val ?? '') };
    });
    fpu = {
      fp,
      fpcr: coerceNum(fpuRaw.fpcr),
      fpsr: coerceNum(fpuRaw.fpsr),
      fpiar: coerceNum(fpuRaw.fpiar),
    };
  }

  return { regs, rows, fpu };
}

export async function readRegisters(): Promise<Registers | null> {
  if (!isModuleReady()) return null;
  const reads = await Promise.all([
    ...REG_DS.map((n) => gsEval(`machine.cpu.${n}`)),
    ...REG_AS.map((n) => gsEval(`machine.cpu.${n}`)),
    gsEval('machine.cpu.pc'),
    gsEval('machine.cpu.sr'),
    gsEval('machine.cpu.usp'),
    gsEval('machine.cpu.ssp'),
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
  const r = await gsEval(`machine.cpu.${name} = ${hex}`);
  return r !== null;
}

export async function peekL(addr: number): Promise<number | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.memory.peek.l', [addr >>> 0]);
  if (r === null || r === undefined) return null;
  return coerceNum(r);
}

export async function peekBytes(addr: number, count: number): Promise<Uint8Array | null> {
  if (!isModuleReady()) return null;
  // Single bridge round-trip via `machine.memory.peek.bytes(addr, count)`. The
  // C side serialises V_BYTES as the string "0x<hex>", which we
  // decode into a Uint8Array. This replaces the previous per-byte
  // fan-out (128 round-trips for a 128-byte window) that made the
  // Memory pane drag noticeably while single-stepping.
  const r = await gsEval('machine.memory.peek.bytes', [addr >>> 0, count]);
  if (typeof r !== 'string') return null;
  const hex = r.startsWith('0x') ? r.slice(2) : r;
  const bytes = hex.length / 2;
  const out = new Uint8Array(bytes);
  for (let i = 0; i < bytes; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16) & 0xff;
  }
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
  // debug.step runs N instructions then scheduler_stop(). When the
  // machine was already paused, scheduler_stop is a no-op (no run-state
  // transition), so Module.onRunStateChange never fires and the Debug
  // panes wouldn't otherwise know to re-fetch. Bumping refreshGen
  // forces a reactive re-render.
  bumpDebugRefresh();
}

export async function stepOver(): Promise<void> {
  // C side doesn't expose step_over yet — fall back to single-step.
  // Phase 7 polish.
  return stepInto(1);
}

export async function stopMachine(): Promise<void> {
  await shutdownEmulator();
}

// Restart the current machine by re-applying the cached boot config
// (model, RAM, ROM, floppies, HD, CD). No dedicated C-side reset method
// exists today; tearing down and re-creating gives the user a full,
// well-defined reboot from cold ROM state.
export async function restart(): Promise<void> {
  if (!isModuleReady()) return;
  const cfg = getLastBootConfig();
  if (!cfg) return;
  await shutdownEmulator();
  await initEmulator(cfg);
  bumpDebugRefresh();
}
