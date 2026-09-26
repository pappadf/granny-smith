// Typed bus surface for the Debug view. Everything the Debug view does
// goes through this module — DisassemblyPane, RegistersSection,
// MemorySection, BreakpointsSection, CallStackSection. Re-exports the
// scheduler/restart calls already in bus/emulator.ts so the Debug
// toolbar only needs to import here.
//
// This file owns the gsEval dispatch.

import {
  gsEval,
  gsOk,
  isGsError,
  shutdownEmulator,
  isModuleReady,
  restartEmulator,
} from './emulator';
import { bumpDebugRefresh } from '@/state/debug.svelte';

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

// One floating-point data register of a frame's `fpu` block. `hex` is the
// raw register as the core formats it (68K: 80-bit extended, exponent_
// mantissa; PPC: the 64-bit double; DSP3210: the 40-bit accumulator,
// mantissa_exponent). `val` is the human-readable decimal (or `Inf` /
// `-Inf` / `NaN`).
export interface FpuRegister {
  hex: string;
  val: string;
}

// The FPU register file in an architecture-neutral shape: the data
// registers (68K fp0-fp7, PPC fpr0-fpr31, the DSP3210's accumulators a0-a3)
// and the control registers by name (68K fpcr/fpsr/fpiar, PPC fpscr).
export interface FpuFrame {
  prefix: string; // display name of the data registers: 'FP', 'FPR' or 'A'
  data: FpuRegister[];
  control: Array<{ name: string; value: number }>;
}

// Bundled snapshot returned by `machine.<core>.frame` (debug.frame is the
// main CPU's). Replaces the per-register + per-row gsEval fan-out the Debug
// view used to do (~21 round-trips) with a single bridge call.
export interface DebugFrame {
  // The core's architecture tag: 'm68k', 'ppc' or 'dsp3210'.
  arch: string;
  pc: number;
  // Every register the core reported, by its own names.
  rawRegs: Record<string, number>;
  // The 68K register view — null on any other architecture, never a 68K
  // shape filled with zeros for registers the core does not have.
  regs: Registers | null;
  rows: DebugFrameRow[];
  // The floating-point block, when the core has one (68882/68040, PPC, DSP).
  fpu?: FpuFrame;
}

export interface Breakpoint {
  id: number;
  addr: number;
  enabled: boolean;
  condition?: string;
  hits: number;
}

function coerceNum(v: unknown): number {
  if (typeof v === 'number') return v >>> 0;
  if (typeof v === 'string') {
    const t = v.trim().replace(/^\$/, '').replace(/^0x/i, '');
    const n = parseInt(t, 16);
    if (Number.isFinite(n)) return n >>> 0;
  }
  return 0;
}

// A core's object-model node name — `cpu`, or an auxiliary core's
// (capabilities.aux_cpus) — is one identifier: it becomes a path segment.
const CORE_NAME = /^[a-z][a-z0-9_]*$/;

// Fetch a core's frame, `machine.<core>.frame`, in a single bridge
// round-trip: the main CPU by default, or an auxiliary core (the AV DSP) —
// every CPU-like object answers the same contract. Default is 32 rows
// starting at PC. When `addr` is omitted the C side uses PC.
// Returns null on parse failure or when the module isn't ready.
// `before` (with no addr): how many rows to show ahead of the PC; the core
// re-synchronises the window so a row always lands exactly on the PC.
export async function loadDebugFrame(
  addr?: number,
  count = 32,
  before = 0,
  core = 'cpu',
): Promise<DebugFrame | null> {
  if (!CORE_NAME.test(core)) return null;
  if (!isModuleReady()) return null;
  // Named arguments: a lone positional argument is `addr` to the core, so
  // `[0, n]` used to disassemble from address 0 (N-32).
  const args: Record<string, number> = { count };
  if (addr !== undefined) args.addr = addr >>> 0;
  else if (before > 0) args.before = before;
  // The frame is a native nested object (V_MAP through the gsEval bridge) —
  // no inner JSON.parse.
  const parsed = await gsEval(`machine.${core}.frame`, args);
  if (!parsed || typeof parsed !== 'object' || isGsError(parsed)) return null;
  const obj = parsed as {
    arch?: unknown;
    pc?: unknown;
    regs?: Record<string, unknown>;
    rows?: unknown[];
  };
  if (!obj.regs || !Array.isArray(obj.rows)) return null;
  const arch = typeof obj.arch === 'string' ? obj.arch : 'm68k';
  const rawRegs: Record<string, number> = {};
  for (const [k, v] of Object.entries(obj.regs)) rawRegs[k] = coerceNum(v);
  const regs: Registers | null =
    arch === 'm68k'
      ? {
          d: Array.from({ length: 8 }, (_, i) => rawRegs[`d${i}`] ?? 0),
          a: Array.from({ length: 8 }, (_, i) => rawRegs[`a${i}`] ?? 0),
          pc: rawRegs.pc ?? 0,
          sr: rawRegs.sr ?? 0,
          usp: rawRegs.usp ?? 0,
          ssp: rawRegs.ssp ?? 0,
        }
      : null;
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

  // The core's floating-point block, only when it has one (absent on a
  // Plus): 68K {fp, fpcr, fpsr, fpiar}, PPC {fpr, fpscr}, DSP3210 {a}.  The
  // one list is the data registers, named after its key; every number is a
  // control register.  Mapped onto one shape so the pane needs no per-arch
  // code.
  let fpu: FpuFrame | undefined;
  const fpuObj = (parsed as { fpu?: unknown }).fpu;
  if (fpuObj && typeof fpuObj === 'object') {
    const raw = fpuObj as Record<string, unknown>;
    const listKey = Object.keys(raw).find((k) => Array.isArray(raw[k])) ?? '';
    const list = listKey ? (raw[listKey] as unknown[]) : [];
    const data: FpuRegister[] = list.map((entry) => {
      if (!entry || typeof entry !== 'object') return { hex: '', val: '' };
      const e = entry as { hex?: unknown; val?: unknown };
      return { hex: String(e.hex ?? ''), val: String(e.val ?? '') };
    });
    const control = Object.entries(raw)
      .filter(([k, v]) => k !== listKey && typeof v === 'number')
      .map(([name, v]) => ({ name, value: coerceNum(v) }));
    fpu = { prefix: listKey.toUpperCase(), data, control };
  }

  return { arch, pc: coerceNum(obj.pc ?? rawRegs.pc), rawRegs, regs, rows, fpu };
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

// A 32-bit read of a LOGICAL address, whatever the architecture: on 68K
// machine.memory.peek goes through the MMU; on PPC it is physical, so read
// through the core's own translation (machine.cpu.mmu.peek) instead.
export async function peekLogicalL(addr: number, arch: string): Promise<number | null> {
  if (arch !== 'ppc') return peekL(addr);
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.cpu.mmu.peek', [addr >>> 0, 4]);
  if (r === null || r === undefined || isGsError(r)) return null;
  return coerceNum(r);
}

export async function peekL(addr: number): Promise<number | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.memory.peek.l', [addr >>> 0]);
  if (r === null || r === undefined || isGsError(r)) return null;
  return coerceNum(r);
}

// `count` bytes at `addr`, in one bridge round-trip.  `space` is passed
// explicitly: "logical" reads through the CPU's own translation on every
// architecture (68K MMU, Lisa segment MMU, PowerPC), "physical" reads the
// physical address.  The C side serialises V_BYTES as "0x<hex>".
export async function peekBytes(
  addr: number,
  count: number,
  space: 'logical' | 'physical' = 'logical',
): Promise<Uint8Array | null> {
  if (!isModuleReady()) return null;
  const r = await gsEval('machine.memory.peek.bytes', [addr >>> 0, count, space]);
  if (typeof r !== 'string') return null;
  const hex = r.startsWith('0x') ? r.slice(2) : r;
  const bytes = hex.length / 2;
  const out = new Uint8Array(bytes);
  for (let i = 0; i < bytes; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16) & 0xff;
  }
  return out;
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
