// The dialog's half of the "which port is the monitor plugged into" contract,
// for a machine that has BOTH built-in video and NuBus slots (the PDM family).
//
// The C side offers the choice through machine.profile's `builtin_video`, and
// consumes it as two independent boot-document fields: `video_card` (which
// card, if any, is seated) and `monitor` (what is strapped to the built-in
// port). What this pins is the translation between the one control the user
// sees and those two fields — specifically that choosing a NuBus card sends
// monitor:'none', because that is what makes the ROM turn built-in video off
// and hand the screen to the card. Without it the card is enumerated as a
// second screen nothing ever draws on, and its accelerator is never used.

import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { initEmulator } from '@/bus/emulator';

const PDM_ROM = '/opfs/images/rom/pm6100-pm7100-pm8100-9feb69b3.rom';

vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    initEmulator: vi.fn(async () => {}),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'machine.rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('pm6100-pm7100-pm8100-9feb69b3.rom')) {
          return {
            recognised: true,
            checksum: 'pdm-checksum',
            name: 'Power Macintosh 6100/7100/8100 ROM',
            compatible: ['pm8100'],
            size: 4 * 1024 * 1024,
          };
        }
        return null;
      }
      if (path === 'machine.profile') {
        const id = (args?.[0] as string) ?? '';
        const byId: Record<string, object> = {
          // Three NuBus sockets AND substrate built-in video: the shape that
          // makes the display picker a real choice.  The generic-vROM 24AC
          // twin needs no vROM, so it is installable in this fixture.
          pm8100: {
            name: 'Power Macintosh 8100',
            builtin_video: { id: 'builtin', display_name: 'Built-in video (Ariel II)' },
            video_slots: [
              {
                slot: 'B',
                fixed: false,
                default_card: '24ac',
                cards: [
                  {
                    id: '24ac',
                    requires_vrom: false,
                    display_name: 'Apple Macintosh Display Card 24AC (generic video ROM)',
                    monitors: [],
                  },
                ],
              },
            ],
            ram_options: [8192, 16384],
            ram_default: 16384,
            floppy_slots: [{ label: 'Internal Floppy', kind: 'hd' }],
            has_cdrom: true,
          },
        };
        return byId[id] ?? { name: id };
      }
      return null;
    },
  };
});

// MockOpfs's ROM fixtures carry no PDM ROM, so pm8100 would never appear.
class OpfsWithPdmRom extends MockOpfs {
  async scanRoms() {
    return [
      ...(await super.scanRoms()),
      { name: 'pm6100-pm7100-pm8100-9feb69b3.rom', path: PDM_ROM, size: 4 * 1024 * 1024 },
    ];
  }
}

beforeEach(async () => {
  _resetForTests();
  setOpfsBackend(new OpfsWithPdmRom());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  setWelcomeSlide('configuration');
  stopDriveActivityMock();
  vi.mocked(initEmulator).mockClear();
});

async function selectModel(container: HTMLElement, id: string): Promise<void> {
  const sel = container.querySelector('#cfg-model') as HTMLSelectElement;
  sel.value = id;
  await fireEvent.change(sel);
  await waitFor(() => {
    if (sel.value !== id) throw new Error('model not applied');
  });
}

async function readyWithPdm(): Promise<HTMLElement> {
  const { container } = render(WelcomeConfigSlide);
  // Only the PDM ROM is recognised by this fixture, so wait for that one
  // option rather than for a populated list.
  await waitFor(() => {
    const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
    const has = sel && Array.from(sel.options).some((o) => o.value === 'pm8100');
    if (!has) throw new Error('pm8100 not scanned yet');
  });
  await selectModel(container, 'pm8100');
  return container;
}

describe('WelcomeConfigSlide built-in video vs NuBus card', () => {
  it('offers built-in video beside the NuBus cards', async () => {
    const container = await readyWithPdm();
    const sel = await waitFor(() => {
      const s = container.querySelector('#cfg-card') as HTMLSelectElement | null;
      if (!s) throw new Error('no display picker');
      return s;
    });
    const values = Array.from(sel.options).map((o) => o.value);
    expect(values).toContain('builtin');
    expect(values).toContain('24ac');
    // Built-in is the default: a stock machine ships with no card in it.
    expect(sel.value).toBe('builtin');
  });

  it('boots built-in video with no card and no monitor override', async () => {
    const container = await readyWithPdm();
    await waitFor(() => {
      if (!container.querySelector('#cfg-card')) throw new Error('no display picker');
    });
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);

    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    const cfg = vi.mocked(initEmulator).mock.calls[0][0];
    expect(cfg.model).toBe('pm8100');
    expect(cfg.videoCard).toBeUndefined();
    // Unset, not 'hires': the machine's own default already is its monitor,
    // and sending a redundant field would make the record lie about what the
    // user chose.
    expect(cfg.monitor).toBeUndefined();
  });

  it('unplugs the built-in port when a NuBus card is chosen', async () => {
    const container = await readyWithPdm();
    const sel = await waitFor(() => {
      const s = container.querySelector('#cfg-card') as HTMLSelectElement | null;
      if (!s) throw new Error('no display picker');
      return s;
    });
    sel.value = '24ac';
    await fireEvent.change(sel);
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);

    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    const cfg = vi.mocked(initEmulator).mock.calls[0][0];
    expect(cfg.videoCard).toBe('24ac');
    // The point of the whole feature: without this the card is a second
    // screen nothing draws on.
    expect(cfg.monitor).toBe('none');
  });
});
