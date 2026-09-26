// Machine state — current emulator status, model info, drive activity glyphs,
// scheduler mode, zoom level.

// 'crashed': the emulator's worker died (a wasm trap or abort); nothing more
// can run in this page (bus/emulator.ts markBridgeDead).
export type MachineStatus = 'no-machine' | 'running' | 'paused' | 'stopped' | 'crashed';

// Drive activity glyph state. `idle` = dim; `read` = white flash; `write` =
// yellow flash. The flash decays on a 180 ms timeout (matches prototype).
export type DriveActivity = 'idle' | 'read' | 'write';

// Three pacing modes, mirroring the core's schedule_paced/schedule_accelerated/
// schedule_unthrottled (proposal-scheduler-two-modes.md and
// proposal-scheduler-accelerated-mode.md): 'live' = wall-clock paced, 'accel' =
// real-time timebase with a faster CPU (accelerator-card model; the adaptive
// governor picks the speed), 'turbo' = as fast as the host allows. The guest
// timeline is identical in live/turbo; accel trades that determinism for CPU
// throughput while VBL/sound stay real-time. The toolbar labels these
// Real-Time / Accelerated / Fast-Forward (proposal §5.1); the internal ids
// below predate the relabel and stay stable.
export type SchedulerMode = 'live' | 'accel' | 'turbo';

// Typed MMU kind, sourced from `machine.profile(id).capabilities.mmu.kind`
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
  // most everything else is square 1:1). Reported by the core via onScreenResize.
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
  // original Mac's speed). Only meaningful — and only shown — in `accel` mode,
  // where the adaptive governor moves it; pushed from the core on change
  // (bus/emulator.ts handleSchedulerSpeed). 1 in every other mode.
  acceleratedSpeed: number;
  // Live performance readout, pushed from the core ~1 Hz (perf proposal P12):
  // emulated MIPS from instruction-count deltas, and the RAF tick rate the
  // main loop is achieving. 0 until the first push after machine start.
  mips: number;
  ticksPerSecond: number;
  zoom: number;
}

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
  zoom: 200,
});

const ZOOM_MIN = 100;
const ZOOM_MAX = 300;

export function setZoom(value: number): void {
  machine.zoom = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, Math.round(value)));
}

// Pure state update. Pushing the mode to the emulator core lives in
// bus/emulator.ts (applySchedulerMode), which calls this on success.
export function setSchedulerMode(mode: SchedulerMode): void {
  machine.scheduler = mode;
  // Any mode switch resets the core's governor to the authentic floor
  // (scheduler_set_mode → scheduler_governor_reset), so the applied speed is
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
export function setPerfStats(mips: number, ticksPerSecond: number): void {
  machine.mips = mips;
  machine.ticksPerSecond = ticksPerSecond;
}

// Core-pushed quick-checkpoint completion (bus/emulator.ts
// handleCheckpointSaved). A fresh object per push so effects keyed on
// identity re-fire even for back-to-back saves.
export function setCheckpointSaved(ms: number): void {
  machine.checkpoint = { at: Date.now(), ms };
}

// ---- Drive activity ----
//
// Core-pushed, on a state edge only (Module.onDriveActivity, em_main.c): the
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
