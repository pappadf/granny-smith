// Booting a machine from the UI: the boot document, the media, and the
// reconciliation after it (capabilities, the status bar's identity).  Kept
// out of emulator.ts, the bridge, because it is built on bus/profile.ts and
// bus/media.ts, which are built on the bridge.

import { gsEval, gsErrorText, seedPram, applySchedulerMode } from './emulator';
import { getProfile } from './profile';
import { attachHardDisk, attachCdrom, insertFloppy, type MediaResult } from './media';
import type { MachineConfig } from './types';
import { machine, type MmuKind, type AuxCpu } from '@/state/machine.svelte';
import { setMounted } from '@/state/images.svelte';
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

  await syncMachineIdentity();
  await applyCapabilities(config.model);
  // machine.videoin / machine.audioin reset with the machine; re-assert the
  // user's camera and microphone toggles (or drop them if the new model has
  // no digitizer / no audio input).
  await reapplyCameraSource();
  await reapplyMicrophoneSource();

  // The Caps Lock latch is host-keyboard state: a mechanically locking key
  // is already down when the machine powers on. Latch it BEFORE the machine
  // runs, so the ROM's ADB init finds the key down and reports it into
  // KeyMap — that is the gate Copland D11E4's boot blocks test.
  if (machine.capsLock) await gsEval('machine.adb.keyboard.down', ['capslock']);
  // A fresh core boots paced; re-assert the user's toolbar selection so a
  // pre-selected Turbo survives machine (re)creation.
  await applySchedulerMode(machine.scheduler);
  await gsEval('scheduler.run');
  // onRunStateChange will flip machine.status to 'running' once the
  // worker pushes the transition.
  // Every new boot starts with the Debug-tab sections collapsed —
  // only the always-visible Disassembly pane shows by default.
  // Persisted localStorage state is overwritten by this reset, which
  // is the user-requested behaviour (each new machine gets a clean
  // debug layout).
  resetDebugSections();
  showNotification('Machine started', 'info');
}
