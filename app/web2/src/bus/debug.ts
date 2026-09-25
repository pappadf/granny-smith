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
  gsOk,
  isGsError,
  shutdownEmulator,
  isModuleReady,
  restartEmulator,
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
  // debug.frame returns a native nested object (V_MAP through the gsEval
  // bridge) — no inner JSON.parse.
  const parsed = await gsEval('debug.frame', args.length ? args : undefined);
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
  if (reads.some((x) => x === null || x === undefined || isGsError(x))) {
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

// A register name is one identifier: it becomes a path segment, so anything
// else (a space, '=', '.') would address something other than a register.
const REGISTER_NAME = /^[a-z][a-z0-9_]*$/;

export async function writeRegister(name: string, value: number): Promise<boolean> {
  if (!isModuleReady() || !REGISTER_NAME.test(name)) return false;
  // The typed setter: an attribute path plus exactly one argument.  A
  // "path = value" string is shell syntax, not a gsEval path — the core
  // rejects it, which is how every register edit used to fail silently.
  const r = await gsEval(`machine.cpu.${name}`, [value >>> 0]);
  return gsOk(r);
}

export async function peekL(addr: number): Promise<number | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.memory.peek.l', [addr >>> 0]);
  if (r === null || r === undefined || isGsError(r)) return null;
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
  // Enumerate by id, not by count: ids are stable and never reused, so after
  // one removal `count` no longer indexes the live entries.  meta.indices
  // returns exactly the live ids.  (`debug.breakpoints.entries.count` never
  // resolved at all: the synthetic count hangs off the collection's owner.)
  const ids = await gsEval('debug.breakpoints.meta.indices', ['entries']);
  if (!Array.isArray(ids)) return [];
  const out: Breakpoint[] = [];
  for (const raw of ids) {
    const id = coerceNum(raw);
    const base = `debug.breakpoints.entries[${id}]`;
    const addr = await gsEval(`${base}.addr`);
    if (isGsError(addr)) continue; // removed between the listing and this read
    const enabled = await gsEval(`${base}.enabled`);
    const cond = await gsEval(`${base}.condition`);
    const hits = await gsEval(`${base}.hit_count`);
    out.push({
      id,
      addr: coerceNum(addr),
      enabled: enabled === true,
      condition: typeof cond === 'string' && cond.length ? cond : undefined,
      hits: coerceNum(hits),
    });
  }
  return out;
}

// Add a breakpoint.  The core returns the existing entry for an address that
// already has one, so a repeated add never stacks a duplicate.
export async function addBreakpoint(addr: number, condition?: string): Promise<boolean> {
  if (!isModuleReady()) return false;
  const args: unknown[] = [addr >>> 0];
  if (condition && condition.trim().length) args.push(condition.trim());
  const r = await gsEval('debug.breakpoints.add', args);
  return gsOk(r);
}

// Remove the breakpoint with stable id `id`.
export async function removeBreakpoint(id: number): Promise<boolean> {
  if (!isModuleReady()) return false;
  const r = await gsEval(`debug.breakpoints.entries[${id}].remove`);
  return gsOk(r);
}

// Remove the breakpoint at `addr`, for callers that know an address but not
// an id (the Disassembly pane's context menu).  False when there is none.
export async function removeBreakpointAt(addr: number): Promise<boolean> {
  const hit = (await listBreakpoints()).find((b) => b.addr === addr >>> 0);
  return hit ? removeBreakpoint(hit.id) : false;
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
  // debug.step runs N instructions through the frame loop (VBL and timers
  // keep running) and stops before it returns.
  if (!gsOk(await gsEval('debug.step', [n]))) return;
  // The run starts and stops inside one call, so Module.onRunStateChange
  // need not fire; bumping refreshGen makes the Debug panes re-fetch.
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

// Restart the current machine: machine.restart power-cycles it in the core
// (proposal-boot-vs-reset §3.2), rebuilding the recorded hardware from cold
// ROM state with the mounted media still attached — no cached-config replay
// or manual re-insertion needed.
export async function restart(): Promise<void> {
  if (!isModuleReady()) return;
  await restartEmulator();
  bumpDebugRefresh();
}
