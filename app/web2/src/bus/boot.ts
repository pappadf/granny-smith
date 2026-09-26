// Booting a machine from the UI: the boot document, the media, and the
// reconciliation after it (capabilities, the status bar's identity).  Kept
// out of emulator.ts, the bridge, because it is built on bus/profile.ts and
// bus/media.ts, which are built on the bridge.

import { gsEval, gsErrorText, seedPram, applySchedulerMode, handleScreenResize } from './emulator';
import { getProfile } from './profile';
import { attachHardDisk, attachCdrom, insertFloppy, type MediaResult } from './media';
import type { MachineConfig } from './types';
import {
  machine,
  setSchedulerMode,
  type MmuKind,
  type AuxCpu,
  type SchedulerMode,
} from '@/state/machine.svelte';
import { images, setMounted } from '@/state/images.svelte';
import { reapplyCameraSource } from '@/state/camera.svelte';
import { reapplyMicrophoneSource } from '@/state/microphone.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { resetDebugSections } from '@/state/debug.svelte';
import { formatRamKb } from '@/lib/machine';

// Read a model's capability probe from `machine.profile().capabilities` and
// apply it to the shared machine state. Replaces the old display-name regex
// that silently misclassified any MMU machine whose name didn't match the
// hardcoded pattern. `mmuKind` is the full typed kind the core exports (all
// six; this used to keep two and collapse the 68040 and PowerPC MMUs to
// "none", F-05), and `mmuEnabled` means "has an MMU of any kind" — every
// kind answers the same machine.cpu.mmu.translate/peek (bus/mmu.ts).  `fpu`
// gates the FPU panel; `auxCpus` lists the auxiliary cores the Debug view
// renders (it was exported and read by nothing, F-08).
export async function applyCapabilities(model: string): Promise<void> {
  let kind: MmuKind = 'none';
  let fpu = false;
  let videoIn = false;
  let audioIn = false;
  let auxCpus: AuxCpu[] = [];
  try {
    const parsed = await getProfile(model);
    if (parsed) {
      const k = parsed.capabilities?.mmu?.kind;
      const KINDS: readonly MmuKind[] = [
        '68030_pmmu',
        '68040',
        'ppc_601',
        'ppc_604',
        'lisa_segment',
      ];
      if (KINDS.includes(k as MmuKind)) kind = k as MmuKind;
      fpu = parsed.capabilities?.cpu?.fpu === true;
      videoIn = parsed.capabilities?.video_in === true;
      audioIn = parsed.capabilities?.audio_in === true;
      auxCpus = parseAuxCpus(parsed.capabilities?.aux_cpus);
    }
  } catch {
    /* leave kind = 'none', fpu = false, videoIn/audioIn = false */
  }
  machine.mmuKind = kind;
  machine.mmuEnabled = kind !== 'none';
  machine.fpu = fpu;
  machine.videoIn = videoIn;
  machine.audioIn = audioIn;
  machine.auxCpus = auxCpus;
}

// capabilities.aux_cpus -> AuxCpu[].  A name becomes a path segment
// (machine.<name>.frame), so only a plain identifier is accepted.
export function parseAuxCpus(raw: unknown): AuxCpu[] {
  if (!Array.isArray(raw)) return [];
  const out: AuxCpu[] = [];
  for (const e of raw) {
    if (!e || typeof e !== 'object') continue;
    const { name, arch, freq } = e as { name?: unknown; arch?: unknown; freq?: unknown };
    if (typeof name !== 'string' || !/^[a-z][a-z0-9_]*$/.test(name)) continue;
    out.push({
      name,
      arch: typeof arch === 'string' ? arch : '',
      freq: typeof freq === 'number' ? freq : 0,
    });
  }
  return out;
}

// The status bar's identity, read from the running machine after a boot or
// a resume (M6, N-12): machine.name and machine.ram, so every entry point —
// the dialog, a URL, a dropped ROM, a checkpoint — shows the same thing.
// (It used to be the dialog's display name on one path and a model id on
// two others, and the dialog's RAM string or nothing.)
export async function syncMachineIdentity(): Promise<void> {
  const name = await gsEval('machine.name');
  const ram = await gsEval('machine.ram');
  machine.model = typeof name === 'string' && name ? name : null;
  machine.ram = typeof ram === 'number' && ram > 0 ? formatRamKb(ram) : null;
}

// Record where a boot-time medium went (the Images panel's badge), or say
// that it did not go in.
function reportMount(path: string, r: MediaResult, what: string): void {
  if (r.ok) setMounted(path, r.mount);
  else showNotification(`${what} not attached: ${r.reason}`, 'error');
}

// Boot a machine from a config. Construction-time settings travel as ONE
// machine.boot configuration document (named JSON-object args, proposal
// proposal-named-args-boot-config §4) — the core validates everything
// before tearing the old machine down, stages the vROM pick, seeds the
// video card/sense/mode, and installs the ROM itself. Only runtime media
// (floppies/HD/CD) remain imperative calls after the boot.
export async function initEmulator(config: MachineConfig): Promise<void> {
  const doc: Record<string, unknown> = {};
  if (config.model) doc.model = config.model;
  // RAM in KB; omitted, the core boots the model's own default.
  if (config.ramKb) doc.ram = config.ramKb;
  // rom is required by machine.boot (the document inherits nothing); a
  // missing one is rejected by the core with a clear error. For vrom,
  // '(auto)' means "let the offer registry resolve" — omit the field.
  if (config.rom && config.rom !== '(auto)') doc.rom = config.rom;
  if (config.vrom && config.vrom !== '(auto)') doc.vrom = config.vrom;
  if (config.videoCard) doc.video_card = config.videoCard;
  // A PCI card is staged by id; '(auto)' for its expansion ROM means the
  // same thing it does for a vROM — omit the field and let the core's
  // offer registry content-match among the files the platform published.
  if (config.pciCard) doc.pci_card = config.pciCard;
  if (config.prom && config.prom !== '(auto)') doc.prom = config.prom;
  if (config.pciOption) doc.pci_option = config.pciOption;
  if (config.videoMode) doc.video_mode = config.videoMode;
  if (config.monitor) doc.monitor = config.monitor;
  const ok = await gsEval('machine.boot', doc);
  if (ok !== true) {
    showNotification(`Boot failed: ${gsErrorText(ok)}`, 'error');
    return;
  }
  // Media, through the one attach helper (bus/media.ts): each into the bay
  // the core derives, and a failure is reported rather than booting the
  // machine without it and no hint.  The boot itself still proceeds.
  for (let i = 0; i < (config.floppies?.length ?? 0); i++) {
    const path = config.floppies[i];
    if (!path || path === '(none)') continue;
    reportMount(path, await insertFloppy(path, true, i), `Floppy ${i + 1}`);
  }
  let bootScsiId = 0;
  if (config.hd && config.hd !== '(none)') {
    const r = await attachHardDisk(config.hd, config.hdBay ?? 0);
    reportMount(config.hd, r, 'Hard disk');
    if (r.ok && r.mount.bus === 'scsi') bootScsiId = r.mount.drive;
  }
  if (config.cd && config.cd !== '(none)') {
    reportMount(config.cd, await attachCdrom(config.cd), 'CD-ROM');
  }
  // Seed a valid PRAM before the machine runs (see seedPram above), naming
  // the disk's own SCSI id as the boot device (it was always 0, F-04).
  await seedPram(config.model, bootScsiId);

  await reconcileUiWithMachine('boot');
  await prepareFreshMachine();
  showNotification('Machine started', 'info');
}

// --- After a machine appears: one reconciliation, every path (R1) ---------
//
// Seven paths leave a machine running: the dialog, a URL, a dropped ROM,
// Restart, and three checkpoint loads (the resume prompt, Checkpoints ▸
// Load, a dropped .checkpoint).  Each did its own subset of the follow-up —
// the checkpoint loads almost none of it, so a restored machine kept the
// previous model's name, RAM, MMU panels, drive count and pacing (F-12,
// F-14).  Now every path calls reconcileUiWithMachine, and the fresh ones
// then prepareFreshMachine.  Neither touches PRAM.

// How the machine came to be running.  A restore brings its own scheduler
// mode (the checkpoint carries it); a boot or restart gets the toolbar's.
export type MachineOrigin = 'boot' | 'restart' | 'restore';

// The model the UI last reconciled with: a restore of another model resets
// the Debug layout, a restore of the same one keeps it.
let reconciledModel: string | null = null;

// The core's scheduler.mode -> the toolbar's mode.
const UI_MODE: Record<string, SchedulerMode> = {
  paced: 'live',
  accelerated: 'accel',
  turbo: 'turbo',
};

export async function reconcileUiWithMachine(origin: MachineOrigin): Promise<void> {
  const id = await gsEval('machine.id');
  const model = typeof id === 'string' && id ? id : null;
  await syncMachineIdentity();
  if (model) await applyCapabilities(model);
  // Per-machine caches go with the machine.
  images.fdDriveCount = -1;
  // machine.videoin / machine.audioin reset with the machine; re-assert the
  // user's camera and microphone toggles (or drop them if the new model has
  // no digitizer / no audio input).
  await reapplyCameraSource();
  await reapplyMicrophoneSource();
  // Every new machine starts with the Debug sections collapsed (a user
  // request: each new machine gets a clean debug layout); a restore of the
  // same model keeps the layout the user had.
  if (origin === 'boot' || (origin === 'restore' && model !== reconciledModel)) {
    resetDebugSections();
  }
  reconciledModel = model;
  if (origin === 'restore') {
    // The checkpoint restored the core's pacing: show it, do not override it.
    const mode = await gsEval('scheduler.mode');
    const ui = typeof mode === 'string' ? UI_MODE[mode] : undefined;
    if (ui) setSchedulerMode(ui);
  } else {
    // A fresh core boots paced; re-assert the user's toolbar selection so a
    // pre-selected Turbo survives machine (re)creation.
    await applySchedulerMode(machine.scheduler);
  }
  await seedScreenFromCore();
}

// The screen geometry is pushed when it changes, and a restore can land on
// one that never changed in this page: read the live size once.
async function seedScreenFromCore(): Promise<void> {
  const w = await gsEval('machine.screen.width');
  const h = await gsEval('machine.screen.height');
  if (typeof w !== 'number' || typeof h !== 'number' || w <= 0 || h <= 0) return;
  const pw = await gsEval('machine.screen.par_w');
  const ph = await gsEval('machine.screen.par_h');
  handleScreenResize(w, h, typeof pw === 'number' ? pw : 1, typeof ph === 'number' ? ph : 1);
}

// A fresh machine (boot or restart) is ready to run.  The Caps Lock latch is
// host-keyboard state: a mechanically locking key is already down when the
// machine powers on, so latch it BEFORE the machine runs, so the ROM's ADB
// init finds the key down and reports it into KeyMap — that is the gate
// Copland D11E4's boot blocks test.  onRunStateChange then flips
// machine.status to 'running' once the worker pushes the transition.
export async function prepareFreshMachine(): Promise<void> {
  if (machine.capsLock) await gsEval('machine.adb.keyboard.down', ['capslock']);
  await gsEval('scheduler.run');
}

// Power-cycle the running machine. machine.restart rebuilds the machine
// from its built-from record — same model, RAM, card, ROM — and keeps the
// mounted media attached by transferring the open image handles across the
// teardown (proposal-boot-vs-reset §3.3), so no manual re-attachment is
// needed here. Only runtime state that is not construction configuration
// (camera/microphone source, scheduler mode) is re-asserted.
export async function restartEmulator(): Promise<void> {
  const ok = await gsEval('machine.restart');
  if (ok !== true) {
    showNotification(`Restart failed: ${gsErrorText(ok)}`, 'error');
    return;
  }
  // The rebuilt machine starts with blank PRAM again — re-seed it.
  const id = await gsEval('machine.id');
  if (typeof id === 'string' && id) await seedPram(id, 0);
  await reconcileUiWithMachine('restart');
  // Re-asserts the Caps Lock latch too (the core also carries it across
  // machine.restart; a re-latch of an already-down key is a no-op).
  await prepareFreshMachine();
  showNotification('Machine restarted', 'info');
}
