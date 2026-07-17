// Machine state — current emulator status, model info, drive activity glyphs,
// scheduler mode, zoom level.

export type MachineStatus = 'no-machine' | 'running' | 'paused' | 'stopped';

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
export type MmuKind = 'none' | '68030_pmmu' | 'lisa_segment';

interface MachineState {
  status: MachineStatus;
  model: string | null;
  ram: string | null;
  // True iff the active machine has a 68030 PMMU (i.e. the panels that show
  // TC/CRP/SRP/TT0/TT1/MMUSR are meaningful). Derived from mmuKind.
  mmuEnabled: boolean;
  mmuKind: MmuKind;
  // True iff the active machine has an FPU (capabilities.cpu.fpu). The FPU
  // debug panel gates its presence on this static capability rather than
  // waiting for the first debug frame to carry an `fpu` block.
  fpu: boolean;
  // width/height are the framebuffer pixel dimensions; parW/parH are the
  // monitor's pixel aspect ratio (display pixel width:height), so the renderer
  // can show non-square pixels correctly (the Lisa 2's 720x364 raster is 2:3,
  // most everything else is square 1:1). Reported by the core via onScreenResize.
  screen: { width: number; height: number; parW: number; parH: number };
  driveActivity: { hd: DriveActivity; fd: DriveActivity; cd: DriveActivity };
  scheduler: SchedulerMode;
  // Effective CPU speed multiplier the core is currently applying (1 = the
  // original Mac's speed). Only meaningful — and only shown — in `accel` mode,
  // where the adaptive governor moves it; pushed from the core on change
  // (bus/emulator.ts handleSchedulerSpeed). 1 in every other mode.
  acceleratedSpeed: number;
  zoom: number;
}

export const machine: MachineState = $state({
  status: 'no-machine',
  model: null,
  ram: null,
  mmuEnabled: false,
  mmuKind: 'none',
  fpu: false,
  screen: { width: 512, height: 342, parW: 1, parH: 1 },
  driveActivity: { hd: 'idle', fd: 'idle', cd: 'idle' },
  scheduler: 'live',
  acceleratedSpeed: 1,
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

// ---- Drive activity mock ----
//
// Replicates the prototype's setInterval at app.js:2465-2471 — once a machine
// is running, flash a random drive read every 1500 ms; each flash is cleared
// after 180 ms. Phase 3 replaces this with real drive-activity callbacks from
// the C side.

const FLASH_INTERVAL_MS = 1500;
const FLASH_HOLD_MS = 180;

let driveTimer: ReturnType<typeof setInterval> | null = null;
let flashClearTimer: ReturnType<typeof setTimeout> | null = null;

export function startDriveActivityMock(): void {
  if (driveTimer !== null) return;
  driveTimer = setInterval(() => {
    if (machine.status !== 'running') return;
    const drives = ['hd', 'fd', 'cd'] as const;
    const pick = drives[Math.floor(Math.random() * drives.length)];
    machine.driveActivity[pick] = 'read';
    if (flashClearTimer !== null) clearTimeout(flashClearTimer);
    flashClearTimer = setTimeout(() => {
      machine.driveActivity[pick] = 'idle';
      flashClearTimer = null;
    }, FLASH_HOLD_MS);
  }, FLASH_INTERVAL_MS);
}

export function stopDriveActivityMock(): void {
  if (driveTimer !== null) {
    clearInterval(driveTimer);
    driveTimer = null;
  }
  if (flashClearTimer !== null) {
    clearTimeout(flashClearTimer);
    flashClearTimer = null;
  }
  machine.driveActivity.hd = 'idle';
  machine.driveActivity.fd = 'idle';
  machine.driveActivity.cd = 'idle';
}
