import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

// The Configuration slide drives the model dropdown by calling
// `machine.rom.identify` on every ROM in OPFS, then `machine.profile` to get the
// human-readable name. Mock both so tests don't need a live WASM module.
vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'machine.rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('plus-v3-4d1f8172.rom')) {
          return {
            recognised: true,
            checksum: 'plus-checksum',
            name: 'Macintosh Plus ROM',
            compatible: ['plus'],
            size: 128 * 1024,
          };
        }
        if (p.endsWith('iix-iicx-se30-97221136.rom')) {
          return {
            recognised: true,
            checksum: 'se30-checksum',
            name: 'Macintosh SE/30 ROM',
            compatible: ['se30'],
            size: 256 * 1024,
          };
        }
        return null;
      }
      if (path === 'machine.profile') {
        const id = (args?.[0] as string) ?? '';
        const byId: Record<string, object> = {
          plus: {
            name: 'Macintosh Plus',
            // No video_slots → no card declares requires_vrom → VROM row hidden.
            ram_options: [1024, 2048, 4096],
            ram_default: 4096,
            floppy_slots: [
              { label: 'Internal Floppy', kind: 'standard' },
              { label: 'External Floppy', kind: 'standard' },
            ],
          },
          se30: {
            name: 'Macintosh SE/30',
            // Built-in video card declares requires_vrom → VROM row shown.
            video_slots: [
              {
                slot: 'builtin',
                fixed: true,
                default_card: 'builtin_se30_video',
                cards: [{ id: 'builtin_se30_video', requires_vrom: true, monitors: [] }],
              },
            ],
            ram_options: [2048, 4096, 8192, 16384],
            ram_default: 8192,
            floppy_slots: [{ label: 'Internal Floppy', kind: 'hd' }],
            has_cdrom: true, // SE/30 advertises a CD-ROM → the CD row is shown
          },
        };
        return byId[id] ?? { name: id };
      }
      return null;
    },
  };
});

beforeEach(() => {
  _resetForTests();
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  setWelcomeSlide('configuration');
  stopDriveActivityMock();
});

describe('WelcomeConfigSlide', () => {
  it('builds the Model dropdown from rom.identify results', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    const labels = Array.from(modelSel.options).map((o) => o.textContent);
    expect(labels).toContain('Macintosh Plus');
    expect(labels).toContain('Macintosh SE/30');
    // Each model has exactly one ROM in the fixture, so the ROM picker is hidden.
    expect(container.querySelector('#cfg-rom')).toBeNull();
  });

  it('renders profile-driven defaults for the chosen model', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    // Default model is 'plus' (first scanned). Its profile has no video_slots
    // (so no display-card row is shown), ram_default=4096 KB, two floppy slots,
    // and no has_cdrom (→ the SCSI CD-ROM row is hidden).
    expect(container.querySelector('#cfg-card')).toBeNull();
    expect((container.querySelector('#cfg-ram') as HTMLSelectElement).value).toBe('4 MB');
    expect(container.querySelectorAll('select[id^="cfg-fd"]').length).toBe(2);
    expect(container.querySelector('#cfg-cd')).toBeNull();
  });

  it('shows the SCSI CD-ROM row only for models whose profile reports has_cdrom', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    // Plus: has_cdrom unset → CD row hidden.
    expect(container.querySelector('#cfg-cd')).toBeNull();
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    modelSel.value = 'se30';
    modelSel.dispatchEvent(new Event('change', { bubbles: true }));
    await waitFor(() => {
      // SE/30: has_cdrom true → CD row shown.
      if (!container.querySelector('#cfg-cd')) throw new Error('cd row not shown yet');
    });
  });

  it('surfaces the Video ROM requirement only for models whose video card requires a VROM', async () => {
    // The dialog is now card-centric: instead of a raw `#cfg-vrom` filename row,
    // a model whose display card requires a vROM that isn't staged shows a
    // "needs a Video ROM" hint (videoUnavailable); with a vROM present it becomes
    // a `#cfg-card` picker. This mock stages no vROM, so SE/30's built-in card
    // surfaces the hint.
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    const vromHintShown = () =>
      [...container.querySelectorAll('.form-help')].some((n) =>
        (n.textContent ?? '').includes('needs a Video ROM'),
      );
    // Plus: no video slot / no card that requires a vROM → no card row, no hint.
    expect(container.querySelector('#cfg-card')).toBeNull();
    expect(vromHintShown()).toBe(false);
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    modelSel.value = 'se30';
    modelSel.dispatchEvent(new Event('change', { bubbles: true }));
    await waitFor(() => {
      // SE/30: built-in video card requires a vROM, none staged → hint shown.
      if (!vromHintShown()) throw new Error('vrom hint not shown yet');
    });
  });

  it('RAM options follow machine.profile and reset to ram_default on model switch', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    let ramSel = container.querySelector('#cfg-ram') as HTMLSelectElement;
    // Plus: ram_options [1, 2, 4] MB, default 4 MB.
    expect(Array.from(ramSel.options).map((o) => o.textContent)).toEqual(['1 MB', '2 MB', '4 MB']);
    expect(ramSel.value).toBe('4 MB');
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    modelSel.value = 'se30';
    modelSel.dispatchEvent(new Event('change', { bubbles: true }));
    await waitFor(() => {
      ramSel = container.querySelector('#cfg-ram') as HTMLSelectElement;
      // SE/30: ram_options [2, 4, 8, 16] MB, default 8 MB.
      const labels = Array.from(ramSel.options).map((o) => o.textContent);
      if (labels.join() !== ['2 MB', '4 MB', '8 MB', '16 MB'].join())
        throw new Error('ram options not refreshed yet');
    });
    expect(ramSel.value).toBe('8 MB');
  });

  it('renders one floppy row per profile.floppy_slots entry', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    // Plus: two floppy slots.
    expect(container.querySelectorAll('select[id^="cfg-fd"]').length).toBe(2);
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    modelSel.value = 'se30';
    modelSel.dispatchEvent(new Event('change', { bubbles: true }));
    await waitFor(() => {
      // SE/30: one floppy slot.
      if (container.querySelectorAll('select[id^="cfg-fd"]').length !== 1)
        throw new Error('floppy rows not resized yet');
    });
  });

  it('Back link returns to the home slide', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      if (!container.querySelector('.back-link')) throw new Error('not ready');
    });
    const backLink = container.querySelector('.back-link') as HTMLAnchorElement;
    await fireEvent.click(backLink);
    expect(layout.welcomeSlide).toBe('home');
  });

  it('Start Machine submit attempts boot (no-op without Module, but resets slide)', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    const form = container.querySelector('form') as HTMLFormElement;
    await fireEvent.submit(form);
    // The form's submit handler resets the slide so the next Welcome lands
    // on Home. Without a real Module the gsEval calls are no-ops, so
    // machine.status doesn't change.
    expect(layout.welcomeSlide).toBe('home');
  });
});
