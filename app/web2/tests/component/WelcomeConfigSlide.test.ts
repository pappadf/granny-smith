// The configuration dialog renders the machine-description tree and edits a
// configuration document.  Every tree here is a real catalog.profile answer
// (tests/helpers/configTree.ts); the boot it sends is read back from the
// mocked bridge.
import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine } from '@/state/machine.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { clearProfileCache, type ConfigDocument } from '@/bus/profile';
import { MockOpfs } from '../helpers/mockOpfs';
import { modelValue } from '../helpers/modelSelect';

// The MockOpfs ROMs: the Plus ROM boots the Plus; the other one stands in for
// a ROM of whichever models a test names.
const fake = vi.hoisted(() => ({
  other: ['iix'] as string[],
  calls: [] as Array<{ path: string; args: unknown }>,
  patch: null as null | ((id: string, t: Record<string, unknown>) => void),
}));

vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  const helper = await import('../helpers/configTree');
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown) => {
      fake.calls.push({ path, args });
      const a = Array.isArray(args) ? args : [];
      if (path === 'machine.rom.identify') {
        const p = String(a[0] ?? '');
        if (p.endsWith('plus-v3-4d1f8172.rom')) return helper.romIdentity('plus-rom', ['plus']);
        if (p.endsWith('iix-iicx-se30-97221136.rom'))
          return helper.romIdentity('other-rom', fake.other);
        return null;
      }
      if (path === 'catalog.profile') {
        const id = String(a[0] ?? '');
        const t = helper.tree(id);
        if (t && fake.patch) fake.patch(id, t);
        return t ?? null;
      }
      if (path === 'machine.boot') return true;
      if (path === 'machine.attach_media') return { bus: a[0], id: a[1], label: '' };
      if (path.startsWith('machine.floppy.drive[')) return true;
      return null;
    },
  };
});

beforeEach(() => {
  _resetForTests();
  clearProfileCache();
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  fake.other = ['iix'];
  fake.calls = [];
  fake.patch = null;
  setWelcomeSlide('configuration');
});

const $ = <T extends Element = HTMLSelectElement>(c: HTMLElement, sel: string) =>
  c.querySelector(sel) as T | null;
const labels = (s: HTMLSelectElement | null) =>
  Array.from(s?.options ?? []).map((o) => o.textContent?.trim());
function choose(sel: HTMLSelectElement, value: string) {
  sel.value = value;
  sel.dispatchEvent(new Event('change', { bubbles: true }));
}

// Render the dialog with `model` selected and its document loaded.
async function open(model: string): Promise<HTMLElement> {
  if (model !== 'plus') fake.other = [model];
  const { container } = render(WelcomeConfigSlide);
  await waitFor(() => {
    const sel = $(container, '#cfg-model');
    if (!sel || !modelValue(sel, model)) throw new Error('not ready');
  });
  const sel = $(container, '#cfg-model')!;
  if (sel.value !== modelValue(sel, model)) choose(sel, modelValue(sel, model));
  await waitFor(() => {
    if (!$(container, '#cfg-opt-memory')) throw new Error('document not loaded');
  });
  return container;
}

// The machine.boot call's arguments, with its config parsed.
function lastBoot(): { model: string; rom: string; config: ConfigDocument } {
  const boots = fake.calls.filter((c) => c.path === 'machine.boot');
  const args = boots[boots.length - 1]?.args as Record<string, string>;
  return { model: args.model, rom: args.rom, config: JSON.parse(args.config) };
}

async function start(c: HTMLElement) {
  await fireEvent.submit(c.querySelector('form') as HTMLFormElement);
  await waitFor(() => {
    if (!fake.calls.some((x) => x.path === 'machine.boot')) throw new Error('no boot');
  });
}

// The button whose text is `text`.
const button = (c: HTMLElement, text: string) =>
  Array.from(c.querySelectorAll('button')).find((b) => b.textContent?.trim() === text)!;

const sectionTitles = (c: HTMLElement) =>
  Array.from(c.querySelectorAll('.config-section')).map((n) => n.textContent?.trim());

describe('WelcomeConfigSlide: the tree, rendered', () => {
  it('names each model as the core does', async () => {
    const c = await open('iici');
    expect(labels($(c, '#cfg-model'))).toEqual(
      expect.arrayContaining(['Macintosh Plus', 'Macintosh IIci']),
    );
  });

  it('a slotless machine: options, its screen, two floppies and its SCSI defaults', async () => {
    const c = await open('plus');
    expect($(c, '#cfg-opt-memory')!.value).toBe('4096');
    expect(labels($(c, '#cfg-opt-memory'))).toContain('4 MB');
    expect($(c, '#cfg-opt-appletalk')).not.toBeNull();
    expect(sectionTitles(c)).toEqual(['Machine', 'Monitor', 'Floppy drives', 'Storage']);
    // One device, one monitor: nothing to choose.
    expect($(c, '#cfg-display')).toBeNull();
    expect($(c, '#cfg-monitor')).toBeNull();
    expect(c.querySelectorAll('select[id^="cfg-fd"]:not([id^="cfg-fd-type"])').length).toBe(2);
    // The external drive can be taken out.
    expect(labels($(c, '#cfg-fd-type-fd1'))).toContain('None');
    const rows = Array.from(c.querySelectorAll('.device-row')).map((r) =>
      r.getAttribute('data-position'),
    );
    expect(rows).toEqual(['scsi:0', 'scsi:3']);
    expect(labels($(c, '#cfg-startup'))).toContain('No default (search all drives)');
    expect($(c, '#cfg-startup')!.value).toBe('scsi:0');
  });

  it('an empty floppy position has no disk picker until a drive is put there', async () => {
    const c = await open('iix');
    expect($(c, '#cfg-fd1')).toBeNull();
    choose($(c, '#cfg-fd-type-fd1')!, 'hd');
    await waitFor(() => expect($(c, '#cfg-fd1')).not.toBeNull());
  });

  it('a model change and Reset to defaults reload the defaults', async () => {
    const c = await open('iix');
    const other = Array.from($(c, '#cfg-opt-memory')!.options).map((o) => o.value)[3];
    choose($(c, '#cfg-opt-memory')!, other);
    await waitFor(() => expect($(c, '#cfg-opt-memory')!.value).toBe(other));
    await fireEvent.click(c.querySelector('[data-testid="cfg-reset"]')!);
    await waitFor(() => expect($(c, '#cfg-opt-memory')!.value).toBe('8192'));
    choose($(c, '#cfg-opt-memory')!, other);
    const model = $(c, '#cfg-model')!;
    choose(model, modelValue(model, 'plus'));
    await waitFor(() => expect($(c, '#cfg-opt-memory')!.value).toBe('4096'));
  });
});

describe('WelcomeConfigSlide: storage', () => {
  it('adds a device at a free unit, never a used or reserved one', async () => {
    const c = await open('plus');
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-device-scsi"]')!);
    const unit = $(c, '#cfg-add-unit-scsi')!;
    const offered = Array.from(unit.options).map((o) => Number(o.value));
    expect(offered).toEqual([1, 2, 4, 5, 6]);
    choose($(c, '#cfg-add-type-scsi')!, 'hd');
    choose(unit, '4');
    const add = button(c, 'Add');
    await fireEvent.click(add);
    await waitFor(() => expect(c.querySelector('[data-position="scsi:4"]')).not.toBeNull());
  });

  it('removing the startup disk moves the startup choice', async () => {
    const c = await open('plus');
    const remove = c.querySelector('[data-position="scsi:0"] button') as HTMLButtonElement;
    await fireEvent.click(remove);
    await waitFor(() => expect(c.querySelector('[data-position="scsi:0"]')).toBeNull());
    expect($(c, '#cfg-startup')!.value).toBe('');
  });

  it('names each bus of a two-bus machine, and the startup list only where it can point', async () => {
    const c = await open('pmg3dt');
    const heads = Array.from(c.querySelectorAll('.bus-head')).map((h) =>
      h.getAttribute('data-bus'),
    );
    expect(heads).toEqual(['scsi', 'ata0', 'ata1']);
    // An ATA bus takes hard disks only: no type picker.
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-device-ata0"]')!);
    expect($(c, '#cfg-add-type-ata0')).toBeNull();
    const add = button(c, 'Add');
    await fireEvent.click(add);
    await waitFor(() => expect(c.querySelector('[data-position="ata0:0"]')).not.toBeNull());
    expect(Array.from($(c, '#cfg-startup')!.options).map((o) => o.value)).not.toContain('ata0:0');
  });

  it('the Network Server: two channels and its keyswitch', async () => {
    const c = await open('ans500');
    expect(labels($(c, '#cfg-opt-keyswitch'))).toEqual(['Unlocked', 'Service', 'Locked']);
    expect(c.textContent).toContain('Internal SCSI bus 0');
    expect(c.textContent).toContain('Internal SCSI bus 1');
    expect($(c, '#cfg-startup')!.value).toBe('scsi:2');
  });

  it('the Lisa: one ProFile, no SCSI', async () => {
    const c = await open('lisa');
    expect(c.textContent).toContain('ProFile port');
    expect(c.querySelector('[data-bus="scsi"]')).toBeNull();
  });

  it('a hard disk with no image is dropped at Start, with a notice; images go to their device', async () => {
    const c = await open('plus');
    choose($(c, '#cfg-media-scsi-3')!, 'system7.iso');
    choose($(c, '#cfg-fd0')!, 'Disk_Tools.dsk');
    await waitFor(() => expect($(c, '#cfg-media-scsi-3')!.value).toBe('system7.iso'));
    await start(c);
    const boot = lastBoot();
    expect(boot.config.storage).toEqual([{ bus: 'scsi', unit: 3, type: 'cd' }]);
    expect(boot.config.startup).toBeNull();
    expect([...toasts.active, ...toasts.queued].some((t) => t.msg.includes('has no image'))).toBe(
      true,
    );
    await waitFor(() => {
      const attach = fake.calls.find((x) => x.path === 'machine.attach_media');
      expect(attach?.args).toEqual(['scsi', 3, 'cd', '/opfs/images/cd/system7.iso']);
    });
    expect(fake.calls.some((x) => x.path === 'machine.floppy.drive[0].insert')).toBe(true);
  });
});

describe('WelcomeConfigSlide: cards and the monitor', () => {
  it('a NuBus machine without built-in video: its default card is the screen', async () => {
    const c = await open('iix');
    const row = c.querySelector('[data-slot="nubus_9"]')!;
    expect(row.textContent).toContain('Macintosh Display Card 8•24');
    expect(row.textContent).toContain('NuBus slot 1');
    expect(row.textContent).toContain('connected');
    expect($(c, '#cfg-display')).toBeNull(); // one display device
    expect(labels($(c, '#cfg-monitor')).length).toBeGreaterThan(1);
    await fireEvent.click(row.querySelector('button')!);
    await waitFor(() => expect(c.textContent).toContain('It will start with no screen'));
  });

  it('adding a card never moves the monitor; Connected to moves it', async () => {
    const c = await open('iici');
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-card"]')!);
    expect(labels($(c, '#cfg-add-card-slot'))).toEqual([
      'NuBus slot 4',
      'NuBus slot 5',
      'NuBus slot 6',
    ]);
    const add = button(c, 'Add');
    await fireEvent.click(add);
    await waitFor(() => expect($(c, '#cfg-display')).not.toBeNull());
    expect($(c, '#cfg-display')!.value).toBe('builtin');
    expect(labels($(c, '#cfg-display'))).toEqual([
      'Built-in video',
      'Macintosh Display Card 8•24 (NuBus slot 4)',
    ]);
    expect(c.querySelector('[data-slot="nubus_c"]')!.textContent).toContain('no monitor');
    choose($(c, '#cfg-display')!, 'nubus_c');
    await waitFor(() => expect($(c, '#cfg-display')!.value).toBe('nubus_c'));
    await start(c);
    const { config } = lastBoot();
    expect(config.cards).toEqual([{ slot: 'nubus_c', card: 'mdc_8_24', options: {} }]);
    expect(config.displays.builtin.monitor).toBe('none');
    expect(config.displays.nubus_c.monitor).toBe('13in_rgb');
  });

  it('a video mode is offered for the chosen monitor, the Mac’s own choice first', async () => {
    const c = await open('iix');
    const modes = labels($(c, '#cfg-video-mode'));
    expect(modes[0]).toBe('Default (chosen by the Mac)');
    expect(modes).toContain('640 × 480, 256 colors');
  });

  it('a PCI card’s options are rendered from the tree, with the monitor’s device', async () => {
    const c = await open('pm7500');
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-card"]')!);
    choose($(c, '#cfg-add-card')!, 'mach64_gx');
    await fireEvent.click(button(c, 'Add'));
    // Not connected: its video memory is on its own row.
    await waitFor(() => expect($(c, '#cfg-card-opt-pci_1-vram')).not.toBeNull());
    expect(labels($(c, '#cfg-card-opt-pci_1-vram'))).toEqual(['2 MB', '4 MB (expansion module)']);
    // Connected: under Monitor instead.
    choose($(c, '#cfg-display')!, 'pci_1');
    await waitFor(() => expect($(c, '#cfg-display-opt-vram')).not.toBeNull());
    expect($(c, '#cfg-card-opt-pci_1-vram')).toBeNull();
    choose($(c, '#cfg-display-opt-vram')!, '4m');
    await start(c);
    expect(lastBoot().config.cards).toEqual([
      { slot: 'pci_1', card: 'mach64_gx', options: { vram: '4m' } },
    ]);
  });

  it('a card with a one-value option shows no picker for it', async () => {
    const c = await open('pm7500');
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-card"]')!);
    choose($(c, '#cfg-add-card')!, 'voodoo2');
    await fireEvent.click(button(c, 'Add'));
    await waitFor(() => expect(c.querySelector('[data-slot="pci_1"]')).not.toBeNull());
    expect(c.querySelector('[data-slot="pci_1"]')!.textContent).toContain('3dfx Voodoo2');
    expect($(c, '#cfg-card-opt-pci_1-raster')).toBeNull();
  });

  it('a card whose ROM is missing is offered disabled, with the reason', async () => {
    fake.patch = (id, t) => {
      if (id !== 'pm9500') return;
      const cards = t.cards as Array<Record<string, unknown>>;
      cards[0].status = 'unavailable';
    };
    const c = await open('pm9500');
    expect(c.textContent).toContain('It will start with no screen');
    await fireEvent.click(c.querySelector('[data-testid="cfg-add-card"]')!);
    const opt = Array.from($(c, '#cfg-add-card')!.options).find((o) => o.value === 'mach64_gx')!;
    expect(opt.disabled).toBe(true);
    expect(opt.textContent).toContain('expansion ROM (.prom)');
  });
});

describe('WelcomeConfigSlide: navigation', () => {
  it('Back returns to the home slide', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      if (!container.querySelector('.back-link')) throw new Error('not ready');
    });
    await fireEvent.click(container.querySelector('.back-link') as HTMLAnchorElement);
    expect(layout.welcomeSlide).toBe('home');
  });

  it('Start boots the model with its ROM and the document, then returns home', async () => {
    const c = await open('plus');
    await start(c);
    const boot = lastBoot();
    expect(boot.model).toBe('plus');
    expect(boot.rom).toBe('/opfs/images/rom/plus-v3-4d1f8172.rom');
    expect(boot.config.options.memory).toBe('4096');
    await waitFor(() => expect(layout.welcomeSlide).toBe('home'));
  });
});
