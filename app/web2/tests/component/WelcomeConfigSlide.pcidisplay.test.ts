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

// Which models this fixture's ROM lights up.  Narrowing it to one model is
// how a test renders the dialog with that model already selected, which is
// the only way to observe the STANDING pick: switching models keeps a
// still-valid card, so a pick made on the previous model would mask it.
let romModels = ['pm9500', 'pm7500'];

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
            compatible: romModels,
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
                    options: [
                      {
                        key: 'vram',
                        label: 'Video Memory',
                        default_value: '2m',
                        values: [
                          { id: '2m', label: '2 MB' },
                          { id: '4m', label: '4 MB (expansion module)' },
                        ],
                      },
                    ],
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
              // The real profile also carries this, and leaving it out of an
              // earlier version of this fixture is exactly why the dialog
              // shipped offering "Control / Chaos on-board video" as though
              // it were an installable card.  It is soldered (fixed) AND a
              // stand-in (fallback): the 9500 has no on-board video, and
              // this entry exists only so a cardless boot has somewhere to
              // draw.  It must not reach the picker and must not stop the
              // dialog asking for the card's ROM.
              {
                slot: 7,
                label: 'VCI',
                fixed: true,
                fallback: true,
                default_card: 'tnt_control',
                cards: [
                  {
                    id: 'tnt_control',
                    display_name: 'Control / Chaos on-board video',
                    class: 'display',
                    requires_prom: false,
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
          // The 7500/8500 shape: the same sockets, but its Control IS the
          // machine's on-board video — soldered and NOT a fallback.  It must
          // be offered as built-in video, and its presence must stop the
          // "your display card needs a ROM" message, because this machine
          // has a working screen either way.
          pm7500: {
            name: 'Power Macintosh 7500',
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
              {
                slot: 4,
                label: 'VCI',
                fixed: true,
                fallback: false,
                default_card: 'tnt_control',
                cards: [
                  {
                    id: 'tnt_control',
                    display_name: 'Control / Chaos on-board video',
                    class: 'display',
                    requires_prom: false,
                    monitors: [],
                  },
                ],
              },
            ],
            ram_options: [32768],
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
  romModels = ['pm9500', 'pm7500'];
  setOpfsBackend(new OpfsWithTnt());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  setWelcomeSlide('configuration');
  stopDriveActivityMock();
  vi.mocked(initEmulator).mockClear();
});

async function ready(model = 'pm9500'): Promise<HTMLElement> {
  const { container } = render(WelcomeConfigSlide);
  await waitFor(() => {
    const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
    const has = sel && Array.from(sel.options).some((o) => o.value === model);
    if (!has) throw new Error(`${model} not scanned yet`);
  });
  const sel = container.querySelector('#cfg-model') as HTMLSelectElement;
  sel.value = model;
  await fireEvent.change(sel);
  await waitFor(() => {
    if (sel.value !== model) throw new Error('model not applied');
  });
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

  it('renders a card-declared option and sends only a non-default pick', async () => {
    const container = await ready();
    const vram = await waitFor(() => {
      const s = container.querySelector('#cfg-pciopt-vram') as HTMLSelectElement | null;
      if (!s) throw new Error('no vram control');
      return s;
    });
    // The dialog knows nothing about "vram" — the card declared it and the
    // control is rendered from that declaration.
    expect(Array.from(vram.options).map((o) => o.value)).toEqual(['2m', '4m']);
    expect(vram.value).toBe('2m');

    // Leaving it at the card's own default sends nothing: the boot record
    // must not claim a choice the user did not make.
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);
    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    expect(vi.mocked(initEmulator).mock.calls[0][0].pciOption).toBeUndefined();

    // Choosing the other value does send it.
    vi.mocked(initEmulator).mockClear();
    await fireEvent.change(vram, { target: { value: '4m' } });
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);
    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no second boot');
    });
    expect(vi.mocked(initEmulator).mock.calls[0][0].pciOption).toBe('vram=4m');
  });

  it("lists the machine's expansion slots, with the card in the first", async () => {
    const container = await ready();
    await waitFor(() => {
      if (!container.querySelector('.slot-list')) throw new Error('no slot list');
    });
    const rows = Array.from(container.querySelectorAll('.slot-row')).map((r) =>
      (r.textContent ?? '').replace(/\s+/g, ' ').trim(),
    );
    // Two sockets; the fixed VCI stand-in is not a slot the user populates.
    expect(rows).toHaveLength(2);
    expect(rows[0]).toContain('A1');
    expect(rows[0]).toContain('Mach64 GX');
    expect(rows[1]).toContain('B1');
    expect(rows[1]).toContain('(empty)');
  });

  it('never offers a soldered stand-in as an installable card', async () => {
    const container = await ready();
    const sel = await waitFor(() => {
      const s = container.querySelector('#cfg-card') as HTMLSelectElement | null;
      if (!s) throw new Error('no display picker');
      return s;
    });
    // tnt_control is soldered down; the core's pci_card_fits_socket refuses
    // to stage it, so a picker offering it would be offering a boot the core
    // is guaranteed to reject.
    const values = Array.from(sel.options).map((o) => o.value);
    expect(values).not.toContain('tnt_control');
  });

  it('asks for the PCI expansion ROM — not a Video ROM — when none is present', async () => {
    promPresent = false;
    const container = await ready();
    await waitFor(() => {
      const text = container.textContent ?? '';
      if (!text.includes('needs a PCI expansion ROM')) {
        throw new Error(`hint not shown: ${text.slice(0, 300)}`);
      }
    });
    // The stand-in must not suppress the ask by looking like on-board video.
    expect(container.textContent).not.toContain('needs a Video ROM');
    // ...and no picker, exactly as a IIfx with no vROM shows none.
    expect(container.querySelector('#cfg-card')).toBeNull();
  });

  it("treats a soldered NON-fallback card as the machine's built-in video", async () => {
    promPresent = false;
    const container = await ready('pm7500');
    // A 7500's Control really is on-board video, so this machine has a
    // screen with no card installed and must not be told to find a ROM.
    await waitFor(() => {
      if (!(container.textContent ?? '').includes('Machine Model')) {
        throw new Error('not rendered');
      }
    });
    expect(container.textContent).not.toContain('needs a PCI expansion ROM');
    expect(container.textContent).not.toContain('needs a Video ROM');
  });

  it('offers the soldered card beside the installable one, and defaults to it', async () => {
    romModels = ['pm7500'];
    const container = await ready('pm7500');
    const sel = await waitFor(() => {
      const s = container.querySelector('#cfg-card') as HTMLSelectElement | null;
      if (!s) throw new Error('no display picker');
      return s;
    });
    const values = Array.from(sel.options).map((o) => o.value);
    // On-board video first — it is what the machine ships as — then the card.
    expect(values).toEqual(['tnt_control', 'mach64_gx']);
    // A stock machine ships no card, so the soldered port is the default.
    expect(sel.value).toBe('tnt_control');
  });

  it('does not stage the soldered card as a pci_card pick', async () => {
    romModels = ['pm7500'];
    const container = await ready('pm7500');
    await waitFor(() => {
      if (!container.querySelector('#cfg-card')) throw new Error('no display picker');
    });
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);
    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    const cfg = vi.mocked(initEmulator).mock.calls[0][0];
    // It seats itself from the machine's own slot table; sending it as a
    // staged pick would be a boot the core rejects.
    expect(cfg.pciCard).toBeUndefined();
    expect(cfg.videoCard).toBeUndefined();
  });
});
