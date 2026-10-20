// Machine state — current emulator status, model info, drive activity glyphs,
// scheduler mode, zoom level.

import { showNotification } from '@/state/toasts.svelte';

// 'crashed': the emulator's worker died (a wasm trap or abort); nothing more
// can run in this page (bus/emulator.ts markBridgeDead).
export type MachineStatus = 'no-machine' | 'running' | 'paused' | 'stopped' | 'crashed';

// Drive activity glyph state. `idle` = dim; `read` = white flash; `write` =
// yellow flash. The flash decays on a 180 ms timeout.
export type DriveActivity = 'idle' | 'read' | 'write';

// Three pacing modes, mirroring the core's schedule_paced/schedule_accelerated/
// schedule_unthrottled (docs/internals/core/scheduler/scheduler.md): 'live' = wall-clock
// paced, 'accel' = real-time timebase with a faster CPU (accelerator-card
// model; the adaptive governor picks the speed), 'turbo' = as fast as the host
// allows. The guest timeline is identical in live/turbo; accel trades that
// determinism for CPU throughput while VBL/sound stay real-time. The toolbar
// labels these Real / Faster / Max; the internal ids below
// predate the relabel and stay stable.
export type SchedulerMode = 'live' | 'accel' | 'turbo';

// Typed MMU kind, sourced from `catalog.profile(id).capabilities.mmu.kind`
// (no longer guessed from the model's display name). The debug panels gate
// their PMMU register views on this so the Lisa's segment MMU never shows
// the wrong (68030) panels.
export type MmuKind = 'none' | '68030_pmmu' | '68040' | 'ppc_601' | 'ppc_604' | 'lisa_segment';

// An auxiliary CPU core (capabilities.aux_cpus): `name` is its object-model
// node, machine.<name>, which answers the same `frame` as machine.cpu.
export interface AuxCpu {
  name: string;
  arch: string;
  freq: number;
}

interface MachineState {
  status: MachineStatus;
  model: string | null;
  ram: string | null;
  // True iff the active machine has an MMU of any kind — logical and physical
  // addresses can differ, so the panels show L:/P: labels and offer a
  // physical memory view.  Derived from mmuKind.
  mmuEnabled: boolean;
  mmuKind: MmuKind;
  // True iff the active machine has an FPU (capabilities.cpu.fpu). The FPU
  // debug panel gates its presence on this static capability rather than
  // waiting for the first debug frame to carry an `fpu` block.
  fpu: boolean;
  // True iff the active machine has the on-board video digitizer
  // (capabilities.video_in — the AV family). Gates the camera toolbar button.
  videoIn: boolean;
  // True iff the active machine has on-board audio input (capabilities.
  // audio_in — the AV family's Singer codec). Gates the microphone button.
  audioIn: boolean;
  // The machine's auxiliary cores (capabilities.aux_cpus — the AV family's
  // DSP3210), each rendered in the Debug view; empty on every other machine.
  auxCpus: AuxCpu[];
  // width/height are the framebuffer pixel dimensions; parW/parH are the
  // monitor's pixel aspect ratio (display pixel width:height), so the renderer
  // can show non-square pixels correctly (the Lisa 2's 720x364 raster is 2:3,
  // most everything else is square 1:1). Reported by the core's `screen` event.
  screen: { width: number; height: number; parW: number; parH: number };
  driveActivity: { hd: DriveActivity; fd: DriveActivity; cd: DriveActivity };
  // Which lights this model has at all (from its profile: hard-disk bays,
  // floppy slots, a CD bay) -- no CD light on a CD-less Plus.
  drives: { hd: boolean; fd: boolean; cd: boolean };
  // Last successful quick/background checkpoint, pushed from the core
  // (bus/emulator.ts handleCheckpointSaved): wall-clock stamp + save
  // duration. null until the first save after page load; the status bar
  // flashes its CP glyph on each push and shows this in the tooltip.
  checkpoint: { at: number; ms: number } | null;
  scheduler: SchedulerMode;
  // Caps Lock latch. Caps Lock is a mechanically locking key on the emulated
  // keyboards, and booting Copland D11E4 requires it latched across a
  // (re)boot — no browser reports the host key as held, so the latch is UI
  // state: toggled from the status bar, pushed to the live machine, and
  // re-asserted after every boot/restart (bus/emulator.ts).
  capsLock: boolean;
  // Effective CPU speed multiplier the core is currently applying (1 = the
  // original Mac's speed). The adaptive governor moves it: above 1 in `accel`
  // mode, below 1 in `accel` and `live` alike when this computer cannot
  // emulate a frame per VBL period; pushed from the core on change
  // (bus/emulator.ts handleSchedulerSpeed). Always 1 in `max`.
  acceleratedSpeed: number;
  // Live performance readout, pushed from the core ~1 Hz:
  // emulated MIPS from instruction-count deltas, and the RAF tick rate the
  // main loop is achieving. 0 until the first push after machine start.
  mips: number;
  ticksPerSecond: number;
  // Per-tick wall time over the last ~60 ticks (ms): the worst and the
  // median em_main_tick, and the worst bridge-request share of a tick. A
  // stutter is a tickMaxMs far above tickP50Ms; the rates above hide it.
  tickMaxMs: number;
  tickP50Ms: number;
  pollMaxMs: number;
  // The CPU engine, pushed with the perf sample: the switch interpreter,
  // the predecoded interpreter, or the PowerPC translation tier on top of
  // it ('' until the first push).  The core decides it (em_main.c): never
  // 'jit' with predecode off, nor on a 68K machine.  With the tier, the
  // share of the last sample's instructions that ran translated, whether
  // it translates kernel code, and its totals; `ppc` says whether the
  // machine's CPU is a PowerPC (the tier's entries in the engine menu).
  engine: CpuEngine;
  jitPct: number;
  jitSupervisor: boolean;
  jitRegions: number;
  jitKb: number;
  ppc: boolean;
  zoom: number;
}

export type CpuEngine = '' | 'interpreter' | 'predecoded' | 'jit';

export const machine: MachineState = $state({
  status: 'no-machine',
  model: null,
  ram: null,
  mmuEnabled: false,
  mmuKind: 'none',
  fpu: false,
  videoIn: false,
  audioIn: false,
  auxCpus: [],
  screen: { width: 512, height: 342, parW: 1, parH: 1 },
  driveActivity: { hd: 'idle', fd: 'idle', cd: 'idle' },
  drives: { hd: false, fd: false, cd: false },
  checkpoint: null,
  scheduler: 'live',
  capsLock: false,
  acceleratedSpeed: 1,
  mips: 0,
  ticksPerSecond: 0,
  tickMaxMs: 0,
  tickP50Ms: 0,
  pollMaxMs: 0,
  engine: '',
  jitPct: 0,
  jitSupervisor: false,
  jitRegions: 0,
  jitKb: 0,
  ppc: false,
  zoom: 200,
});

const ZOOM_MIN = 50;
const ZOOM_MAX = 300;

// Whether the user has chosen a zoom.  Until they do, the display fits the
// screen (fitZoom): the default 200 % cut off anything larger than a
// compact Mac's screen in an ordinary window.
let zoomChosen = false;

export function setZoom(value: number): void {
  zoomChosen = true;
  machine.zoom = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, Math.round(value)));
}

// The largest zoom, in 10 % steps from 50 % up to the 200 % default, at
// which a `width` × `height` screen (pixel aspect parH/parW) fits a
// `boxW` × `boxH` display area.  Applied only while the user has not set a
// zoom; at 50 % a screen still too big for the area scrolls.
export function fitZoom(
  width: number,
  height: number,
  aspect: number,
  boxW: number,
  boxH: number,
): void {
  if (zoomChosen || width <= 0 || height <= 0 || boxW <= 0 || boxH <= 0) return;
  const fits = Math.min(boxW / width, boxH / (height * aspect)) * 100;
  machine.zoom = Math.max(ZOOM_MIN, Math.min(200, Math.floor(fits / 10) * 10));
}

// Pure state update. Pushing the mode to the emulator core lives in
// bus/emulator.ts (applySchedulerMode), which calls this on success.
export function setSchedulerMode(mode: SchedulerMode): void {
  machine.scheduler = mode;
  // Any mode switch resets the core's governor to the authentic floor
  // (scheduler_apply_pacing → scheduler_governor_reset), so the applied speed is
  // 1x until the governor earns headroom again. Mirror that immediately; the
  // core's push then tracks the climb. (Not a guess — it matches the
  // documented governor-reset behaviour, like the optimistic mode mirror.)
  machine.acceleratedSpeed = 1;
}

// Core-pushed effective CPU speed multiplier (bus/emulator.ts handleSchedulerSpeed).
export function setAcceleratedSpeed(multiplier: number): void {
  machine.acceleratedSpeed = multiplier;
}

// Core-pushed live performance metrics (bus/emulator.ts handlePerfUpdate).
export function setPerfStats(
  mips: number,
  ticksPerSecond: number,
  tick: { tickMaxMs: number; tickP50Ms: number; pollMaxMs: number },
  cpu?: {
    engine: CpuEngine;
    jitPct: number;
    jitSupervisor: boolean;
    jitRegions: number;
    jitKb: number;
    ppc: boolean;
  },
): void {
  machine.mips = mips;
  machine.ticksPerSecond = ticksPerSecond;
  machine.tickMaxMs = tick.tickMaxMs;
  machine.tickP50Ms = tick.tickP50Ms;
  machine.pollMaxMs = tick.pollMaxMs;
  if (cpu) {
    machine.engine = cpu.engine;
    machine.jitPct = cpu.jitPct;
    machine.jitSupervisor = cpu.jitSupervisor;
    machine.jitRegions = cpu.jitRegions;
    machine.jitKb = cpu.jitKb;
    machine.ppc = cpu.ppc;
  }
}

// Core-pushed quick-checkpoint completion (bus/emulator.ts
// handleCheckpointSaved). A fresh object per push so effects keyed on
// identity re-fire even for back-to-back saves.
export function setCheckpointSaved(ms: number): void {
  machine.checkpoint = { at: Date.now(), ms };
  checkpointFailureToastAt = null;
}

// Core-pushed quick-checkpoint (autosave) failure: the save was not written,
// and the last one on disk is all a reload would resume.  Said once per run
// of failures: an autosave every few seconds that keeps failing would
// otherwise fill the screen.  A save that works ends the run; a run that
// lasts is said again after CHECKPOINT_FAILURE_REPEAT_MS.
export const CHECKPOINT_FAILURE_REPEAT_MS = 10 * 60_000;
let checkpointFailureToastAt: number | null = null;

// Returns whether a toast was shown.
export function noteCheckpointFailed(error: string, now = Date.now()): boolean {
  if (
    checkpointFailureToastAt !== null &&
    now - checkpointFailureToastAt < CHECKPOINT_FAILURE_REPEAT_MS
  )
    return false;
  checkpointFailureToastAt = now;
  showNotification(
    `Automatic save failed${error ? ` (${error})` : ''}: the machine's state was not saved, so a reload resumes an older state.`,
    'error',
  );
  return true;
}

// ---- Drive activity ----
//
// Core-emitted, on a state edge only (the drive_activity event, em_main.c): the
// core counts every disk read and write, samples the per-kind sums once per
// tick and holds a light on for a minimum visible time
// (src/core/storage/drive_activity.c).  kind: 0 hd, 1 fd, 2 cd; state:
// 0 idle, 1 read, 2 write.  It replaces a mock that was never started.
const DRIVE_KINDS = ['hd', 'fd', 'cd'] as const;
const DRIVE_STATES: readonly DriveActivity[] = ['idle', 'read', 'write'];

export function setDriveActivity(kind: number, state: number): void {
  const k = DRIVE_KINDS[kind];
  const s = DRIVE_STATES[state];
  if (k && s) machine.driveActivity[k] = s;
}

// Every light off (a new machine).
export function resetDriveActivity(): void {
  for (const k of DRIVE_KINDS) machine.driveActivity[k] = 'idle';
}
