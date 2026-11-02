// What the page can say about a slow or stuck emulator without the
// emulator's help.  Two things, both printed on the console (lines start
// "diag:" and "page:") and mirrored to the browser's devtools console:
//
//  - dumpDiagnostics: everything the page can read from this thread when the
//    emulator stalls or crashes -- the emulator thread's breadcrumbs (where
//    it is, since when, the last PC: src/core/debug/breadcrumb.h), the
//    mailbox (heartbeat, the requests in flight and their ages, the rings),
//    the Rage 128 GPU worker's control words and its own counters, the
//    page's memory and the core's recent events.
//  - the page monitor: every `log.perf_every` seconds, beside the core's own
//    "perf:" lines, the same sources as rates.  It keeps printing while the
//    core is stuck, which the core's lines cannot.

import {
  breadcrumbsPtr,
  bridgeDeadReason,
  getMailbox,
  getModule,
  gsEval,
  recentCoreEvents,
} from '@/bus/emulator';
import { routePrintLine } from '@/bus/logSink';
import { rage128GpuProbe, rage128GpuWorkerStats } from '@/gpu/rage128Gpu.svelte';

// The breadcrumb block's words (keep in step with breadcrumb.h)
const CRUMB = {
  MAGIC: 0,
  VERSION: 1,
  SEQ: 2,
  PHASE: 3,
  PHASE_MS: 4,
  OUTER: 5,
  TICKS: 6,
  TICK_MS: 7,
  TICK_VBLS: 8,
  TICK_VBL: 9,
  INSTR_LO: 10,
  INSTR_HI: 11,
  PC: 12,
  R24: 13,
  VBL_MS: 14,
  HOST_US_PER_VBL: 15,
  HOST_US_PER_LOOP: 16,
  MODE: 17,
  JIT_PENDING: 18,
  GPU_WAITS: 19,
  GPU_WAIT_MS: 20,
} as const;
const CRUMB_MAGIC = 0x47534442;
const PHASES = [
  'event-loop',
  'tick',
  'run',
  'video',
  'requests',
  'idle',
  'gpu-ring',
  'gpu-ack',
  'gpu-pace',
  'jit-install',
  'checkpoint',
  'watch',
];
const MODES = ['paced', 'turbo', 'accelerated'];

const pageStart = performance.now();

function out(prefix: string, line: string): void {
  const text = `${prefix} ${line}`;
  routePrintLine(text);
  console.warn(text);
}

const hex = (v: number) => (v >>> 0).toString(16).padStart(8, '0');
const secs = (ms: number) => `${(ms / 1000).toFixed(1)} s`;
// Age of a breadcrumb time (wall-clock ms, low 32 bits)
const ageMs = (word: number) => ((Date.now() >>> 0) - word) >>> 0;

interface Crumbs {
  seq: number;
  phase: string;
  outer: string;
  phaseMs: number;
  ticks: number;
  tickMs: number;
  tickVbls: number;
  tickVbl: number;
  instr: number;
  pc: number;
  r24: number;
  vblMs: number;
  usPerVbl: number;
  usPerLoop: number;
  mode: string;
  jitPending: number;
  gpuWaits: number;
  gpuWaitMs: number;
}

function readCrumbs(): Crumbs | null {
  const mod = getModule();
  const ptr = breadcrumbsPtr();
  if (!mod || !ptr) return null;
  const buffer =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory?.buffer ?? mod.HEAPU8.buffer;
  const w = new Uint32Array(buffer, ptr, 32);
  const at = (i: number) => Atomics.load(w, i) >>> 0;
  if (at(CRUMB.MAGIC) !== CRUMB_MAGIC) return null;
  return {
    seq: at(CRUMB.SEQ),
    phase: PHASES[at(CRUMB.PHASE)] ?? `#${at(CRUMB.PHASE)}`,
    outer: PHASES[at(CRUMB.OUTER)] ?? `#${at(CRUMB.OUTER)}`,
    phaseMs: ageMs(at(CRUMB.PHASE_MS)),
    ticks: at(CRUMB.TICKS),
    tickMs: ageMs(at(CRUMB.TICK_MS)),
    tickVbls: at(CRUMB.TICK_VBLS),
    tickVbl: at(CRUMB.TICK_VBL),
    instr: at(CRUMB.INSTR_HI) * 2 ** 32 + at(CRUMB.INSTR_LO),
    pc: at(CRUMB.PC),
    r24: at(CRUMB.R24),
    vblMs: ageMs(at(CRUMB.VBL_MS)),
    usPerVbl: at(CRUMB.HOST_US_PER_VBL),
    usPerLoop: at(CRUMB.HOST_US_PER_LOOP),
    mode: MODES[at(CRUMB.MODE)] ?? `#${at(CRUMB.MODE)}`,
    jitPending: at(CRUMB.JIT_PENDING),
    gpuWaits: at(CRUMB.GPU_WAITS),
    gpuWaitMs: at(CRUMB.GPU_WAIT_MS),
  };
}

function crumbsLine(c: Crumbs): string {
  const inner =
    c.phase.startsWith('gpu') || c.phase === 'jit-install' ? ` (inside ${c.outer})` : '';
  return (
    `phase ${c.phase}${inner} for ${secs(c.phaseMs)} (seq ${c.seq}); tick #${c.ticks} began ${secs(c.tickMs)} ago ` +
    `with ${c.tickVbls} frame-units, ${c.tickVbl} run, the last ended ${secs(c.vblMs)} ago; ` +
    `instr ${c.instr}, PC=${hex(c.pc)} r24=${hex(c.r24)}; ${c.mode}, est ${(c.usPerVbl / 1000).toFixed(2)} ms/frame-unit ` +
    `${(c.usPerLoop / 1000).toFixed(2)} ms/loop; jit compiles in flight ${c.jitPending}; ` +
    `gpu waits ${c.gpuWaits} (${secs(c.gpuWaitMs)} total)`
  );
}

function memoryLine(): string {
  const mod = getModule();
  const wasm =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory } | null)?.wasmMemory?.buffer.byteLength ??
    mod?.HEAPU8.buffer.byteLength ??
    0;
  const js = (
    performance as unknown as { memory?: { usedJSHeapSize: number; jsHeapSizeLimit: number } }
  ).memory;
  return (
    `wasm memory ${(wasm / 2 ** 20).toFixed(0)} MB` +
    (js
      ? `, js heap ${(js.usedJSHeapSize / 2 ** 20).toFixed(0)} of ${(js.jsHeapSizeLimit / 2 ** 20).toFixed(0)} MB`
      : '')
  );
}

function workerStatsLine(s: Record<string, unknown>): string {
  const n = (k: string) => (typeof s[k] === 'number' ? (s[k] as number) : 0);
  const t = n('seconds') || 1;
  return (
    `${n('records')} records (${(n('records') / t).toFixed(0)}/s, ${(n('bytes') / 1024 / t).toFixed(0)} KB/s), ` +
    `${n('draws')} draws, drains ${n('drains')} busy ${n('drainMs').toFixed(0)} ms (${((100 * n('drainMs')) / (t * 1000)).toFixed(0)}%, longest ${n('drainMaxMs').toFixed(0)} ms), ` +
    `readbacks ${n('readbacks')} avg ${(n('readbacks') ? n('readbackMs') / n('readbacks') : 0).toFixed(1)} ms max ${n('readbackMaxMs').toFixed(0)} ms, ` +
    `presents ${n('presents')} (GPU completion lag max ${n('presentLagMaxMs').toFixed(0)} ms), idle waits ${n('idleWaits')}; ` +
    `attached=${s.attached} lost=${s.lost} loop=${s.loopRunning} readback pending=${s.readbackPending}` +
    (n('readbackForMs') ? ` for ${n('readbackForMs').toFixed(0)} ms` : '') +
    (n('drainingForMs') ? `, in a drain for ${n('drainingForMs').toFixed(0)} ms` : '') +
    `, consumed ${n('consumed')} head ${n('head')}, last record kind ${n('lastKind')}, ` +
    `${n('surfaces')} surfaces ${n('atlases')} atlases ${n('pipelines')} pipelines`
  );
}

let dumping = false;

// Everything the page can read about the emulator now.  `why` heads the
// dump ("stall", "crash", "requested", ...).
export async function dumpDiagnostics(why: string, detail = ''): Promise<void> {
  if (dumping) return;
  dumping = true;
  try {
    out(
      'diag:',
      `===== ${why}${detail ? `: ${detail}` : ''} at ${new Date().toISOString()} (page up ${secs(performance.now() - pageStart)}) =====`,
    );
    const dead = bridgeDeadReason();
    if (dead) out('diag:', `the bridge is dead: ${dead}`);
    const c = readCrumbs();
    out(
      'diag:',
      c
        ? `emulator thread: ${crumbsLine(c)}`
        : 'emulator thread: no breadcrumbs (the module is not up)',
    );

    const mb = getMailbox();
    if (mb) {
      const st = mb.stats();
      const rw = mb.ringWords();
      const list = mb.inFlightList();
      out(
        'diag:',
        `mailbox: heartbeat ${mb.heartbeat()}, status ${mb.status()}, ${list.length} requests in flight` +
          (list.length
            ? ` (oldest first: ${list
                .slice(0, 12)
                .map((r) => `#${r.id} ${r.path} ${secs(r.ageMs)}`)
                .join(', ')}${list.length > 12 ? ', ...' : ''})`
            : '') +
          `; request ring head ${rw.reqHead} tail ${rw.reqTail}, event ring head ${rw.evtHead} tail ${rw.evtTail} (read to ${rw.readerAt}); ` +
          `served ${st.requests} requests, ${st.events} events, ${st.stalls} ring stalls, drain max ${st.drainMaxUs} us, ${st.bad} bad, ${st.dropped} dropped`,
      );
    }

    const g = rage128GpuProbe();
    if (g) {
      out(
        'diag:',
        `rage128 gpu ring: head ${g.head} tail ${g.tail} (backlog ${g.backlog} of ${g.ringSize} bytes), readback req ${g.req} ack ${g.ack}, ` +
          `status ${g.status}, presents done ${g.presentsDone}, frames ${g.frames} draws ${g.draws} flushes ${g.flushes} readbacks ${g.readbacks}` +
          (g.lost ? `, device lost: ${g.lost}` : ''),
      );
      const ws = await rage128GpuWorkerStats(1000);
      out(
        'diag:',
        ws
          ? `rage128 gpu worker: ${workerStatsLine(ws)}`
          : 'rage128 gpu worker: did not answer within 1 s (busy in a drain, or wedged)',
      );
    }

    out(
      'diag:',
      `page: ${document.visibilityState}, ${memoryLine()}, main-thread lag max ${lagMaxMs.toFixed(0)} ms over the last interval, ` +
        `${navigator.hardwareConcurrency ?? '?'} cores, ${navigator.userAgent}`,
    );
    const events = recentCoreEvents();
    const perf = [...events].reverse().find((e) => e.event === 'perf');
    if (perf) out('diag:', `last perf event: ${JSON.stringify(perf.data)}`);
    const last = events.slice(-15).map((e) => `${e.kind}:${e.event}`);
    if (last.length) out('diag:', `last core events (oldest first): ${last.join(', ')}`);

    // A second look a second later says what is still moving
    if (c) {
      await new Promise((r) => setTimeout(r, 1000));
      const c2 = readCrumbs();
      if (c2)
        out(
          'diag:',
          `one second later: phase ${c2.phase} (seq ${c2.seq - c.seq >= 0 ? `+${c2.seq - c.seq}` : c2.seq}), ` +
            `ticks +${c2.ticks - c.ticks}, frame-units +${c2.tickVbl - c.tickVbl} in tick, instr +${c2.instr - c.instr}, ` +
            `PC=${hex(c2.pc)}, heartbeat ${mb?.heartbeat() ?? '?'}`,
        );
    }
    out('diag:', '===== end =====');
  } finally {
    dumping = false;
  }
}

// While stalled: the breadcrumbs again every few seconds, so the console
// shows whether anything moves
export function stallTrace(): string {
  const c = readCrumbs();
  const g = rage128GpuProbe();
  return (
    (c ? crumbsLine(c) : 'no breadcrumbs') +
    (g
      ? `; gpu ring backlog ${g.backlog}, ack ${g.ack}/${g.req}, presents done ${g.presentsDone}`
      : '') +
    `; heartbeat ${getMailbox()?.heartbeat() ?? '?'}`
  );
}

// --- the page monitor ------------------------------------------------------

let monitorTimer: ReturnType<typeof setInterval> | null = null;
let lagTimer: ReturnType<typeof setInterval> | null = null;
let rafId = 0;
let rafFrames = 0;
let lagMaxMs = 0;
let lagLast = 0;
let prev: {
  t: number;
  hb: number;
  crumbs: Crumbs | null;
  mb: ReturnType<NonNullable<ReturnType<typeof getMailbox>>['stats']> | null;
  gpu: Record<string, number | string> | null;
} | null = null;

function startLagProbe(): void {
  if (lagTimer) return;
  lagLast = performance.now();
  lagTimer = setInterval(() => {
    const now = performance.now();
    const lag = now - lagLast - 100;
    if (lag > lagMaxMs) lagMaxMs = lag;
    lagLast = now;
  }, 100);
  const raf = () => {
    rafFrames++;
    rafId = requestAnimationFrame(raf);
  };
  rafId = requestAnimationFrame(raf);
}

function stopLagProbe(): void {
  if (lagTimer) clearInterval(lagTimer);
  lagTimer = null;
  cancelAnimationFrame(rafId);
}

async function monitorTick(): Promise<void> {
  const now = performance.now();
  const mb = getMailbox();
  const c = readCrumbs();
  const g = rage128GpuProbe();
  const hb = mb?.heartbeat() ?? 0;
  const st = mb?.stats() ?? null;
  if (prev) {
    const dt = (now - prev.t) / 1000;
    const p = prev;
    let line = `${dt.toFixed(2)}s heartbeat +${hb - p.hb}`;
    if (c && p.crumbs) {
      const di = c.instr - p.crumbs.instr;
      line +=
        `; core ${c.phase} for ${secs(c.phaseMs)}, ticks +${c.ticks - p.crumbs.ticks}, ` +
        `last tick ${c.tickVbl}/${c.tickVbls} frame-units, instr +${di} (${(di / dt / 1e6).toFixed(1)} MIPS), ` +
        `PC=${hex(c.pc)}, jit in flight ${c.jitPending}, gpu waits +${c.gpuWaits - p.crumbs.gpuWaits} ` +
        `(${c.gpuWaitMs - p.crumbs.gpuWaitMs} ms)`;
    }
    if (mb && st && p.mb)
      line += `; mailbox ${mb.inFlight()} in flight, +${st.requests - p.mb.requests} requests +${st.events - p.mb.events} events`;
    if (g) {
      const pg = p.gpu;
      const d = (k: string) => (pg ? (g[k] as number) - (pg[k] as number) : 0);
      line +=
        `; gpu ring backlog ${g.backlog} B, presents done +${d('presentsDone')}, frames +${d('frames')}, ` +
        `draws +${d('draws')}, readbacks +${d('readbacks')}`;
    }
    line += `; page ${(rafFrames / dt).toFixed(0)} fps, main-thread lag max ${lagMaxMs.toFixed(0)} ms, ${memoryLine()}`;
    out('page:', line);
    if (g) {
      const ws = await rage128GpuWorkerStats(Math.min(1000, monitorSeconds * 500));
      out(
        'page:',
        ws
          ? `rage128 gpu worker: ${workerStatsLine(ws)}`
          : 'rage128 gpu worker: no answer (busy in a drain, or wedged)',
      );
    }
  }
  rafFrames = 0;
  lagMaxMs = 0;
  prev = { t: now, hb, crumbs: c, mb: st, gpu: g };
}

let monitorSeconds = 0;

// Runs the page monitor every `seconds` (0 stops it).  The core's own
// perf lines follow log.perf_every; this follows it too (onPerfEvery).
export function setPageMonitor(seconds: number): void {
  monitorSeconds = seconds;
  if (monitorTimer) clearInterval(monitorTimer);
  monitorTimer = null;
  prev = null;
  if (seconds <= 0) {
    stopLagProbe();
    return;
  }
  startLagProbe();
  monitorTimer = setInterval(() => void monitorTick(), seconds * 1000);
  void monitorTick();
}

// ?perf=N in the page URL: the core's perf lines and this monitor from the
// start, every N seconds
export async function applyPerfFromUrl(params: URLSearchParams): Promise<void> {
  const v = params.get('perf');
  if (v === null) return;
  const n = v === '' ? 1 : Number(v);
  if (!Number.isFinite(n) || n < 0) return;
  await gsEval('log.perf_every', [Math.round(n)]);
  setPageMonitor(Math.round(n));
}

// The lag probe runs whenever something may want its figure (a stall).
export function ensureLagProbe(): void {
  startLagProbe();
}
