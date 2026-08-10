// The dialog's half of the video-card contract: a model whose profile reports
// no video slots must be booted with no video_card at all, even when the card
// picker was populated for a previously selected model.
//
// This is NOT the regression test for the staging v0.8.0 failure ("model
// 'q660av' has no NuBus slots for video_card 'se30'") — the dialog was always
// innocent there, and this test passed before that bug was fixed. The card was
// re-supplied underneath it by machine.boot's inheritance step, which is
// guarded by tests/integration/boot-config and, end to end, by
// tests/e2e/web2-specs/av-boot-no-slots.spec.ts. What this pins is that the
// dialog never starts sending one.

import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { initEmulator } from '@/bus/emulator';

vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    initEmulator: vi.fn(async () => {}),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'machine.rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('iix-iicx-se30-97221136.rom')) {
          return {
            recognised: true,
            checksum: 'se30-checksum',
            name: 'Macintosh SE/30 ROM',
            compatible: ['se30'],
            size: 256 * 1024,
          };
        }
        if (p.endsWith('q840av-q660av-5bf10fd1.rom')) {
          return {
            recognised: true,
            checksum: 'av-checksum',
            name: 'Quadra 840AV / Centris 660AV ROM',
            compatible: ['q660av'],
            size: 2 * 1024 * 1024,
          };
        }
        return null;
      }
      if (path === 'machine.profile') {
        const id = (args?.[0] as string) ?? '';
        const byId: Record<string, object> = {
          // The SE/30 builtin slot carries TWO sibling kinds (the generic
          // 'se30' and the real-vROM 'builtin_se30_video'), so
          // build_video_slots() reports fixed:false and the card is
          // selectable via video_card=.
          se30: {
            name: 'Macintosh SE/30',
            video_slots: [
              {
                slot: 'builtin',
                fixed: false,
                default_card: 'se30',
                cards: [
                  {
                    id: 'se30',
                    requires_vrom: false,
                    display_name: 'Built-in video',
                    monitors: [],
                  },
                  { id: 'builtin_se30_video', requires_vrom: true, monitors: [] },
                ],
              },
            ],
            ram_options: [2048, 4096, 8192, 16384],
            ram_default: 8192,
            floppy_slots: [{ label: 'Internal Floppy', kind: 'hd' }],
            has_cdrom: true,
          },
          // .nubus_slots = NULL upstream → build_video_slots() → empty list.
          q660av: {
            name: 'Centris 660AV',
            video_slots: [],
            ram_options: [8192, 16384, 32768, 68608],
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

const AV_ROM = '/opfs/images/rom/q840av-q660av-5bf10fd1.rom';

// MockOpfs's ROM fixtures are hard-coded and carry no AV ROM, so the AV
// model would never appear in the dropdown. Add it on top.
class OpfsWithAvRom extends MockOpfs {
  async scanRoms() {
    return [
      ...(await super.scanRoms()),
      { name: 'q840av-q660av-5bf10fd1.rom', path: AV_ROM, size: 2 * 1024 * 1024 },
    ];
  }
}

beforeEach(async () => {
  _resetForTests();
  setOpfsBackend(new OpfsWithAvRom());
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

describe('WelcomeConfigSlide video-card leakage', () => {
  it('does not send a video_card for a model with no NuBus slots', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length < 2) throw new Error('models not scanned yet');
    });

    // 1. Pick the SE/30 — its builtin slot selects the generic 'se30' card.
    await selectModel(container, 'se30');

    // 2. Switch to the 660AV, which has no video slots at all.
    await selectModel(container, 'q660av');

    // 3. Boot.
    await fireEvent.submit(container.querySelector('form') as HTMLFormElement);

    await waitFor(() => {
      if (vi.mocked(initEmulator).mock.calls.length === 0) throw new Error('no boot yet');
    });
    const cfg = vi.mocked(initEmulator).mock.calls[0][0];
    expect(cfg.model).toBe('q660av');
    // The C side rejects ANY video_card on a slotless model, so the field
    // must be absent — not merely a different card.
    expect(cfg.videoCard).toBeUndefined();
  });
});
