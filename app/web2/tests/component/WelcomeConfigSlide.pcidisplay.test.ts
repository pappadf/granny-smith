// The dialog's half of "this machine's display comes only from a PCI slot".
//
// The Power Macintosh 9500 has no built-in video and no NuBus video slots:
// machine.profile reports builtin_video {} and video_slots [], and the only
// display source is a display-class card in pci_slots. What this pins is the
// translation between the one control the user sees and the two boot-document
// fields it produces — `pci_card` (which card is seated) and `prom` (the
// expansion ROM that drives it) — plus the two states either side of it: the
// card is not offered at all until its ROM has been uploaded, and the dialog
// says so in those words rather than showing an empty picker.

import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { initEmulator } from '@/bus/emulator';

const TNT_ROM = '/opfs/images/rom/pm7500-pm8500-pm9500-96cd923d.rom';
const MACH64_PROM = '/opfs/images/prom/437584e0';

// Whether this fixture's OPFS has the card's expansion ROM in it. The two
// states are the whole point of the row, so they are one switch.
let promPresent = true;

vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    initEmulator: vi.fn(async () => {}),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'machine.rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('pm7500-pm8500-pm9500-96cd923d.rom')) {
          return {
            recognised: true,
            checksum: '96cd923d',
            name: 'Power Macintosh 7500/8500/9500 ROM',
            compatible: ['pm9500'],
            size: 4 * 1024 * 1024,
          };
        }
        return null;
      }
      // The core owns the blob -> card mapping; the UI carries none.
      if (path === 'machine.prom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p === MACH64_PROM) {
          return { recognised: true, card_id: 'mach64_gx', compatible: ['mach64_gx'] };
        }
        return { recognised: false };
      }
      if (path === 'machine.profile') {
        const id = (args?.[0] as string) ?? '';
        const byId: Record<string, object> = {
          // The authentic 9500: no built-in video, no NuBus video slots, and
          // one display-class card offered by every PCI socket.
          pm9500: {
            name: 'Power Macintosh 9500',
            builtin_video: {},
            video_slots: [],
            pci_slots: [
              {
                slot: 1,
                label: 'A1',
                fixed: false,
                cards: [
                  {
                    id: 'mach64_gx',
                    display_name: 'Apple Accelerated PCI Graphics Card (ATI Mach64 GX)',
                    class: 'display',
                    requires_prom: true,
                    monitors: [],
                  },
                ],
              },
              // A second socket offering the SAME card: the picker must not
              // list it twice.
              {
                slot: 2,
                label: 'B1',
                fixed: false,
                cards: [
                  {
                    id: 'mach64_gx',
                    display_name: 'Apple Accelerated PCI Graphics Card (ATI Mach64 GX)',
                    class: 'display',
                    requires_prom: true,
                    monitors: [],
                  },
                ],
              },
            ],
            ram_options: [32768, 65536],
            ram_default: 32768,
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

class OpfsWithTnt extends MockOpfs {
  async scanRoms() {
    return [
      ...(await super.scanRoms()),
      { name: 'pm7500-pm8500-pm9500-96cd923d.rom', path: TNT_ROM, size: 4 * 1024 * 1024 },
    ];
  }
  async scanImages(cat: 'rom' | 'vrom' | 'prom' | 'fd' | 'hd' | 'cd') {
    if (cat === 'prom') {
      return promPresent ? [{ name: '437584e0', path: MACH64_PROM, kind: 'file' as const }] : [];
    }
    return super.scanImages(cat);
  }
}

beforeEach(async () => {
  _resetForTests();
  promPresent = true;
  setOpfsBackend(new OpfsWithTnt());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  setWelcomeSlide('configuration');
  stopDriveActivityMock();
  vi.mocked(initEmulator).mockClear();
});

async function ready(): Promise<HTMLElement> {
  const { container } = render(WelcomeConfigSlide);
  await waitFor(() => {
    const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
    const has = sel && Array.from(sel.options).some((o) => o.value === 'pm9500');
    if (!has) throw new Error('pm9500 not scanned yet');
  });
  const sel = container.querySelector('#cfg-model') as HTMLSelectElement;
  sel.value = 'pm9500';
  await fireEvent.change(sel);
  return container;
}

describe('WelcomeConfigSlide display-class PCI cards', () => {
  it('offers the PCI display card once, however many sockets take it', async () => {
    const container = await ready();
    const sel = await waitFor(() => {
      const s = container.querySelector('#cfg-card') as HTMLSelectElement | null;
      if (!s) throw new Error('no display picker');
      return s;
    });
    const values = Array.from(sel.options).map((o) => o.value);
    expect(values).toEqual(['mach64_gx']);
    // It is the only display source, so it must also be the standing pick —
    // the picker being a single entry is exactly when nothing else would set
    // it, and submit relies on it.
    expect(sel.value).toBe('mach64_gx');
  });

  it('boots it as pci_card with its expansion ROM, not as a NuBus card', async () => {
    const container = await ready();
    await waitFor(() => {
      if (!container.querySelector('#cfg-card')) throw new Error('no display picker');
    });
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);
    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    const cfg = vi.mocked(initEmulator).mock.calls[0][0];
    expect(cfg.model).toBe('pm9500');
    expect(cfg.pciCard).toBe('mach64_gx');
    expect(cfg.prom).toBe(MACH64_PROM);
    // The two buses are different fields. Sending video_card here would make
    // the core look for a NuBus card that does not exist.
    expect(cfg.videoCard).toBeUndefined();
    // There is no built-in port to unplug on this machine, so no override.
    expect(cfg.monitor).toBeUndefined();
  });

  it('says the machine needs a display card when no expansion ROM is present', async () => {
    promPresent = false;
    const container = await ready();
    await waitFor(() => {
      const text = container.textContent ?? '';
      if (!text.includes('requires a display card in a PCI slot')) {
        throw new Error(`hint not shown: ${text.slice(0, 200)}`);
      }
    });
    // ...and it does not offer a picker it cannot honour.
    expect(container.querySelector('#cfg-card')).toBeNull();
  });
});
