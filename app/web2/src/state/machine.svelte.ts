// Machine state — current emulator status, model info, drive activity glyphs.
// Phase 1 only ever observes `status === 'no-machine'`; Phase 2+ flips to
// 'running' / 'paused' / 'stopped' as the user starts/stops a machine.

export type MachineStatus = 'no-machine' | 'running' | 'paused' | 'stopped';

// Drive activity glyph state. `idle` = dim; `read` = white flash; `write` =
// yellow flash. The flash decays on a 180 ms timeout (matches prototype).
export type DriveActivity = 'idle' | 'read' | 'write';

interface MachineState {
  status: MachineStatus;
  model: string | null;
  ram: string | null;
  mmuEnabled: boolean;
  screen: { width: number; height: number };
  driveActivity: { hd: DriveActivity; fd: DriveActivity; cd: DriveActivity };
}

export const machine: MachineState = $state({
  status: 'no-machine',
  model: null,
  ram: null,
  mmuEnabled: false,
  screen: { width: 512, height: 342 },
  driveActivity: { hd: 'idle', fd: 'idle', cd: 'idle' },
});
