// Machine state — current emulator status, model info, drive activity glyphs,
// scheduler mode, zoom level.

export type MachineStatus = 'no-machine' | 'running' | 'paused' | 'stopped';

// Drive activity glyph state. `idle` = dim; `read` = white flash; `write` =
// yellow flash. The flash decays on a 180 ms timeout (matches prototype).
export type DriveActivity = 'idle' | 'read' | 'write';

export type SchedulerMode = 'strict' | 'live' | 'fast';

interface MachineState {
  status: MachineStatus;
  model: string | null;
  ram: string | null;
  mmuEnabled: boolean;
  screen: { width: number; height: number };
  driveActivity: { hd: DriveActivity; fd: DriveActivity; cd: DriveActivity };
  scheduler: SchedulerMode;
  zoom: number;
}

export const machine: MachineState = $state({
  status: 'no-machine',
  model: null,
  ram: null,
  mmuEnabled: false,
  screen: { width: 512, height: 342 },
  driveActivity: { hd: 'idle', fd: 'idle', cd: 'idle' },
  scheduler: 'live',
  zoom: 200,
});

const ZOOM_MIN = 100;
const ZOOM_MAX = 300;

export function setZoom(value: number): void {
  machine.zoom = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, Math.round(value)));
}

export function setSchedulerMode(mode: SchedulerMode): void {
  machine.scheduler = mode;
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
