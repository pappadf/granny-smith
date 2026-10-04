import { describe, it, expect, vi, beforeEach } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { initEmulator, restartEmulator, reconcileUiWithMachine } = await import('@/bus/boot');
const { maybeOfferBackgroundCheckpoint, resolveResume } = await import('@/bus/checkpoint');
const { clearProfileCache } = await import('@/bus/profile');
const { machine } = await import('@/state/machine.svelte');
const { debug } = await import('@/state/debug.svelte');
const { images } = await import('@/state/images.svelte');

// The reconcile's first step reads the machine's id: its request marks that
// the reconcile ran.
const RECONCILE_MARK = 'machine.id';

// A running SE/30, as the core would answer after a boot or a restore.
function se30(mode = 'paced'): void {
  bridge.reply('machine.boot', true);
  bridge.reply('machine.restart', true);
  bridge.reply('machine.id', 'se30');
  bridge.reply('machine.name', 'Macintosh SE/30');
  bridge.reply('machine.ram', 8192);
  bridge.reply('catalog.profile', { id: 'se30', capabilities: { mmu: { kind: '68030_pmmu' } } });
  // An enum reads as {enum, index} over the bridge.
  const index = ['paced', 'accelerated', 'turbo'].indexOf(mode);
  bridge.reply('scheduler.mode', (args: unknown) => (args ? null : { enum: mode, index }));
  bridge.reply('scheduler.run', null);
}

const BOOT = {
  model: 'se30',
  rom: '/opfs/images/rom/se30.rom',
  vrom: '(auto)',
  floppies: [],
  hd: '',
  cd: '',
};

describe('one post-boot reconciliation, every path', () => {
  beforeEach(() => {
    bridge.reset();
    clearProfileCache();
    machine.model = null;
    machine.ram = null;
    machine.scheduler = 'live';
    machine.mmuKind = 'none';
  });

  it('a dialog boot reconciles, then runs', async () => {
    se30();
    await initEmulator(BOOT);
    const paths = bridge.paths();
    expect(paths).toContain(RECONCILE_MARK);
    expect(paths.indexOf(RECONCILE_MARK)).toBeLessThan(paths.lastIndexOf('scheduler.run'));
    expect(machine.model).toBe('Macintosh SE/30');
    expect(machine.ram).toBe('8 MB');
    expect(machine.mmuKind).toBe('68030_pmmu');
    // A boot keeps the toolbar's pacing: pacing is the page's, not the
    // machine's, so nothing reads it back from the core.
    se30('turbo');
    await initEmulator(BOOT);
    expect(machine.scheduler).toBe('live');
  });

  // A rejected boot document leaves the previous machine in place, so
  // nothing may be attached to it, reconciled from it, or run.
  it('a rejected boot attaches, reconciles and runs nothing', async () => {
    bridge.reply('machine.boot', { error: 'machine.boot: unknown model' });
    await initEmulator({ ...BOOT, hd: '/opfs/images/hd/a.img' });
    expect(bridge.paths()).toEqual(['machine.boot']);
  });

  // The page names the disk's own SCSI id as the startup device,
  // through the core's setter -- no PRAM bytes, and no seed at all otherwise.
  it('a boot with a hard disk names its SCSI id as the startup device', async () => {
    se30();
    bridge.reply('machine.attach_hd', { bus: 'scsi', id: 3, label: 'ID 3' });
    bridge.reply('machine.rtc.pram.boot_device', null);
    await initEmulator({ ...BOOT, hd: '/opfs/images/hd/a.img', hdBay: 1 });
    const set = bridge.calls.find((c) => c.path === 'machine.rtc.pram.boot_device');
    expect(set?.args).toEqual([3]);
    expect(bridge.paths().some((p) => p.includes('pram.poke') || p === 'shell.run')).toBe(false);
  });

  // An Open Firmware machine keeps Mac OS's PRAM in its NVRAM, not in Cuda:
  // the id goes to machine.nvram.startup_disk and the RTC is left alone.
  it('on an Open Firmware machine the startup device goes to the NVRAM', async () => {
    se30();
    bridge.reply('machine.attach_hd', { bus: 'scsi', id: 3, label: 'ID 3' });
    bridge.reply('machine.nvram.startup_disk', null);
    await initEmulator({ ...BOOT, hd: '/opfs/images/hd/a.img', hdBay: 1 });
    const set = bridge.calls.find((c) => c.path === 'machine.nvram.startup_disk');
    expect(set?.args).toEqual([3]);
    expect(bridge.paths()).not.toContain('machine.rtc.pram.boot_device');
  });

  // machine.restart power-cycles the SAME machine: its PRAM still names the
  // startup disk and nothing about the machine changed, so the page writes
  // nothing and reconciles nothing -- it only runs it.
  it('a restart re-asserts nothing, then runs', async () => {
    se30();
    images.mounted['/opfs/images/hd/a.img'] = { kind: 'hd', bus: 'scsi', drive: 2 };
    await restartEmulator();
    expect(bridge.paths()).toEqual(['machine.restart', 'scheduler.run']);
    delete images.mounted['/opfs/images/hd/a.img'];
  });

  it("pacing is the page's: neither a boot nor a restore reads or re-asserts it", async () => {
    se30('turbo');
    await reconcileUiWithMachine('restore');
    await reconcileUiWithMachine('boot');
    expect(machine.scheduler).toBe('live');
    expect(bridge.paths()).not.toContain('scheduler.mode');
    expect(images.fdDriveCount).toBe(-1);
  });

  it('a restore of the same model keeps the Debug layout; another model resets it', async () => {
    se30();
    await reconcileUiWithMachine('boot');
    debug.sections.registers = true;
    await reconcileUiWithMachine('restore');
    expect(debug.sections.registers).toBe(true);
    bridge.reply('machine.id', 'plus');
    await reconcileUiWithMachine('restore');
    expect(debug.sections.registers).toBe(false);
  });

  it('a restore sets the run state from the core, from Welcome too (#239)', async () => {
    se30();
    machine.status = 'no-machine';
    bridge.reply('scheduler.running', true);
    await reconcileUiWithMachine('restore');
    expect(machine.status).toBe('running');
    bridge.reply('scheduler.running', false);
    await reconcileUiWithMachine('restore');
    expect(machine.status).toBe('paused');
  });

  it('after a running restore, the stop that follows shows as paused (#267)', async () => {
    const { dispatchCoreEvent } = await import('@/bus/emulator');
    const { EVT_STATE } = await import('@/bus/mailbox');
    se30();
    // A paused machine first, so the bridge's mirror says "not running".
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_ended","mode":1}');
    machine.status = 'no-machine';
    bridge.reply('scheduler.running', true);
    await reconcileUiWithMachine('restore');
    expect(machine.status).toBe('running');
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_ended","mode":1}');
    expect(machine.status).toBe('paused');
  });

  it('the resume prompt reconciles after checkpoint.load', async () => {
    se30();
    bridge.reply('checkpoint.probe', true);
    bridge.reply('checkpoint.load', true);
    bridge.reply('scheduler.running', true);
    const resumed = maybeOfferBackgroundCheckpoint();
    await vi.waitFor(() => expect(bridge.paths()).toContain('checkpoint.probe'));
    resolveResume(true);
    expect(await resumed).toBe(true);
    expect(bridge.paths()).toContain(RECONCILE_MARK);
  });
});

// Every place that makes a machine appear must reconcile: the unit tests
// above drive the paths that need no DOM or network; this pins the rest (the
// URL boot, the dropped ROM and .checkpoint, Checkpoints ▸ Load) at source.
describe('every machine-making call site reconciles', () => {
  const srcDir = join(process.cwd(), 'src');
  function walk(dir: string): string[] {
    const out: string[] = [];
    for (const entry of readdirSync(dir)) {
      const p = join(dir, entry);
      if (statSync(p).isDirectory()) out.push(...walk(p));
      else if (/\.(ts|svelte)$/.test(entry)) out.push(p);
    }
    return out;
  }
  const makers = /gsEval\(\s*'(machine\.boot|machine\.rebuild|checkpoint\.load)'/;

  it('each file that boots, rebuilds or loads a checkpoint calls reconcileUiWithMachine', () => {
    const offenders = walk(srcDir)
      .filter((f) => makers.test(readFileSync(f, 'utf8')))
      .filter((f) => !/reconcileUiWithMachine\(/.test(readFileSync(f, 'utf8')));
    expect(offenders).toEqual([]);
  });
});
