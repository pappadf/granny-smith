// Booting a machine from the UI: the boot document, the media, and the
// reconciliation after it (capabilities, the status bar's identity).  Kept
// out of emulator.ts, the bridge, because it is built on bus/profile.ts and
// bus/media.ts, which are built on the bridge.

import { gsEval, gsErrorText, gsOk, setRunStateMirror } from './emulator';
import { getProfile } from './profile';
import { attachMedia, insertFloppy, type MediaResult } from './media';
import type { MachineConfig } from './types';
import { machine, resetDriveActivity, type MmuKind, type AuxCpu } from '@/state/machine.svelte';
import { images, setMounted, clearMounts } from '@/state/images.svelte';
import { reapplyCameraSource } from '@/state/camera.svelte';
import { reapplyMicrophoneSource } from '@/state/microphone.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { resetDebugSections } from '@/state/debug.svelte';
import { formatRamKb } from '@/lib/machine';
import { recentLabel } from '@/lib/recentMachines';
import { addRecentMachine } from '@/state/recent.svelte';

// Read a model's capability probe from `catalog.profile().capabilities` and
// apply it to the shared machine state. Replaces the old display-name regex
// that silently misclassified any MMU machine whose name didn't match the
// hardcoded pattern. `mmuKind` is the full typed kind the core exports (all
// six; this used to keep two and collapse the 68040 and PowerPC MMUs to
// "none"), and `mmuEnabled` means "has an MMU of any kind" — every
// kind answers the same machine.cpu.mmu.translate/peek (bus/mmu.ts).  `fpu`
// gates the FPU panel; `auxCpus` lists the auxiliary cores the Debug view
// renders (it was exported and read by nothing).
export async function applyCapabilities(model: string): Promise<void> {
  let kind: MmuKind = 'none';
  let fpu = false;
  let videoIn = false;
  let audioIn = false;
  let auxCpus: AuxCpu[] = [];
  let drives = { hd: false, fd: false, cd: false };
  try {
    const parsed = await getProfile(model);
    if (parsed) {
      // What the model's storage can take, from its tree.
      const accepts = (t: string) => parsed.storage.some((b) => b.accepts.some((a) => a.id === t));
      drives = { hd: accepts('hd'), fd: parsed.floppies.length > 0, cd: accepts('cd') };
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
  machine.drives = drives;
  resetDriveActivity();
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
// a resume: machine.name and machine.ram, so every entry point —
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

// Boot a machine from a config.  The configuration travels as ONE
// machine.boot argument, the configuration document (config=, JSON): the
// core validates all of it, builds the new machine from it -- its options,
// drives, cards and their ROMs, the connected display, the startup device --
// and only then replaces the running one.  Only the images are imperative
// calls after the boot, each into the device the document placed.
export async function initEmulator(config: MachineConfig): Promise<void> {
  const args: Record<string, unknown> = { model: config.model };
  // rom is required by machine.boot (it inherits nothing); a missing one is
  // rejected by the core with a clear error.
  if (config.rom) args.rom = config.rom;
  if (config.config) args.config = JSON.stringify(config.config);
  const ok = await gsEval('machine.boot', args);
  if (ok !== true) {
    showNotification(`Boot failed: ${gsErrorText(ok)}`, 'error');
    return;
  }
  clearMounts();
  // Media, through the one attach helper (bus/media.ts); a failure is
  // reported rather than booting the machine without it and no hint.  The
  // boot itself still proceeds.  The startup device is the document's, so
  // nothing here records one.
  for (const [id, path] of Object.entries(config.floppies ?? {})) {
    const drive = Number(id.replace(/^fd/, ''));
    if (!path || !Number.isInteger(drive)) continue;
    reportMount(path, await insertFloppy(path, true, drive), `Floppy ${drive + 1}`);
  }
  for (const m of config.media ?? []) {
    const what = m.type === 'cd' ? 'CD-ROM' : 'Hard disk';
    reportMount(m.path, await attachMedia(m.bus, m.unit, m.type, m.path), what);
  }

  await reconcileUiWithMachine('boot');
  await recordRecentBoot(config);
  await prepareFreshMachine();
  showNotification('Machine started', 'info');
}

// Put a machine that just booted on the Welcome page's Recent list, labelled
// from the running machine (its name and RAM, after reconciliation) and the
// profile's card names.  Recorded: New Machine (and a Recent relaunch, which
// moves it to the top), both through initEmulator, and a dropped ROM's
// default boot (bus/upload.ts).  Not recorded: URL-media boots -- the URL
// is the way back to those, and their media may be scratch downloads -- and
// checkpoint loads, which restore a saved state rather than boot a
// configuration (the Checkpoints panel keeps those).
export async function recordRecentBoot(config: MachineConfig): Promise<void> {
  const profile = await getProfile(config.model).catch(() => null);
  const cards = (config.config?.cards ?? []).map(
    (c) => profile?.cards.find((p) => p.id === c.card)?.label ?? c.card,
  );
  const name = machine.model ?? profile?.name ?? config.model;
  addRecentMachine(config, recentLabel({ name, ram: machine.ram, cards, config }));
}

// --- After a machine appears: one reconciliation, every path --------------
//
// Six paths leave a NEW machine running: the dialog, a URL, a dropped ROM,
// and three checkpoint loads (the resume prompt, Checkpoints ▸ Load, a
// dropped .checkpoint).  Each did its own subset of the follow-up — the
// checkpoint loads almost none of it, so a restored machine kept the
// previous model's name, RAM, MMU panels, drive count and pacing.  Now every
// path calls reconcileUiWithMachine, and the fresh ones then
// prepareFreshMachine.  Neither touches PRAM.  Restart is not among them: it
// power-cycles the SAME machine, so there is nothing new to reconcile with.

// How the machine came to be running.  Pacing is the page's, not the
// machine's: neither origin touches it.
export type MachineOrigin = 'boot' | 'restore';

// The model the UI last reconciled with: a restore of another model resets
// the Debug layout, a restore of the same one keeps it.
let reconciledModel: string | null = null;

export async function reconcileUiWithMachine(origin: MachineOrigin): Promise<void> {
  const id = await gsEval('machine.id');
  const model = typeof id === 'string' && id ? id : null;
  await syncMachineIdentity();
  if (model) await applyCapabilities(model);
  // Per-machine caches go with the machine.  A boot cleared the mounts
  // before attaching its own media; a restore brings the checkpoint's media,
  // which the page has no record of, so no badge may claim a drive.
  images.fdDriveCount = -1;
  if (origin === 'restore') clearMounts();
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
  // Every restore entry point (background resume, Open Checkpoint, a drop,
  // the Checkpoints tab) brings the page's run state into line here.
  if (origin === 'restore' && model) {
    const running = (await gsEval('scheduler.running')) === true;
    setRunStateMirror(running);
    machine.status = running ? 'running' : 'paused';
  }
}

// A fresh machine (a boot) is ready to run.  The Caps Lock latch is
// host-keyboard state: a mechanically locking key is already down when the
// machine powers on, so latch it BEFORE the machine runs, so the ROM's ADB
// init finds the key down and reports it into KeyMap — that is the gate
// Copland D11E4's boot blocks test.  The mode events then flip
// machine.status to 'running' once the worker pushes the transition.
export async function prepareFreshMachine(): Promise<void> {
  if (machine.capsLock) await gsEval('machine.adb.keyboard.down', ['capslock']);
  const run = await gsEval('scheduler.run');
  if (!gsOk(run)) showNotification(`Could not run the machine: ${gsErrorText(run)}`, 'error');
}

// Power-cycle the running machine.  machine.restart tears nothing down: the
// core resets the same machine with its RAM cold, so the PRAM/NVRAM (the
// startup disk with it), the mounted media, the Caps Lock latch, the camera
// and microphone sources and the pacing all survive because nothing
// destroyed them.  There is nothing to re-assert; only run it.
export async function restartEmulator(): Promise<void> {
  const ok = await gsEval('machine.restart');
  if (ok !== true) {
    showNotification(`Restart failed: ${gsErrorText(ok)}`, 'error');
    return;
  }
  const run = await gsEval('scheduler.run');
  if (!gsOk(run)) {
    showNotification(`Restarted, but could not run: ${gsErrorText(run)}`, 'error');
    return;
  }
  showNotification('Machine restarted', 'info');
}
