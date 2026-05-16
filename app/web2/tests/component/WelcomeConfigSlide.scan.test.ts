import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { _resetForTests } from '@/state/toasts.svelte';
import { layout } from '@/state/layout.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import type { OpfsBackend } from '@/bus/opfs';
import type { OpfsEntry, ImageCategory, RomInfo } from '@/bus/types';

// Mock the emulator bus: the new Config slide drives its dropdowns by
// calling rom.identify + machine.profile via gsEval. Stub both so tests
// don't need a live Module.
vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'rom.identify') {
        const p = (args?.[0] as string) ?? '';
        // Default behavior: a ROM named *Plus* matches model 'plus', *SE30*
        // matches 'se30'. Tests that need finer control swap in a
        // different fixture via the StubOpfs path list.
        if (/Plus/i.test(p)) {
          return JSON.stringify({
            recognised: true,
            checksum: `cs-${p}`,
            name: 'Macintosh Plus ROM',
            compatible: ['plus'],
            size: 128 * 1024,
          });
        }
        if (/SE30|SE_30|SE\/30/i.test(p)) {
          return JSON.stringify({
            recognised: true,
            checksum: `cs-${p}`,
            name: 'Macintosh SE/30 ROM',
            compatible: ['se30'],
            size: 256 * 1024,
          });
        }
        return null;
      }
      if (path === 'machine.profile') {
        const id = (args?.[0] as string) ?? '';
        const names: Record<string, string> = {
          plus: 'Macintosh Plus',
          se30: 'Macintosh SE/30',
        };
        return JSON.stringify({ name: names[id] ?? id });
      }
      return null;
    },
  };
});

// A test backend that lets each test seed which ROMs / images are present.
class StubOpfs implements OpfsBackend {
  constructor(public images: Partial<Record<ImageCategory, OpfsEntry[]>> = {}) {}
  async list(): Promise<OpfsEntry[]> {
    return [];
  }
  async scanRoms(): Promise<RomInfo[]> {
    return (this.images.rom ?? []).map((e) => ({ name: e.name, path: e.path, size: 0 }));
  }
  async scanImages(cat: ImageCategory): Promise<OpfsEntry[]> {
    return this.images[cat] ?? [];
  }
  async scanCheckpoints(): Promise<never[]> {
    return [];
  }
  async readJson(): Promise<null> {
    return null;
  }
  async writeJson(): Promise<void> {
    /* no-op */
  }
  async move(): Promise<void> {
    /* no-op */
  }
  async delete(): Promise<void> {
    /* no-op */
  }
  async rename(): Promise<void> {
    /* no-op */
  }
  async mkdirP(): Promise<void> {
    /* no-op */
  }
}

beforeEach(() => {
  _resetForTests();
  layout.welcomeSlide = 'configuration';
  machine.status = 'no-machine';
  stopDriveActivityMock();
});

describe('WelcomeConfigSlide OPFS scan', () => {
  it('shows the "no ROMs in storage" help when no ROMs are present', async () => {
    setOpfsBackend(new StubOpfs());
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      expect(container.querySelector('.form-help')?.textContent).toContain('No ROMs in storage');
    });
    const submit = container.querySelector('.primary-button') as HTMLButtonElement;
    expect(submit.disabled).toBe(true);
  });

  it('shows the ROM picker only when more than one ROM matches the chosen model', async () => {
    // Two ROMs, both compatible with 'plus' → the ROM picker should appear.
    setOpfsBackend(
      new StubOpfs({
        rom: [
          { name: 'Plus_v1.rom', path: '/opfs/images/rom/Plus_v1.rom', kind: 'file' },
          { name: 'Plus_v3.rom', path: '/opfs/images/rom/Plus_v3.rom', kind: 'file' },
        ],
      }),
    );
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-rom') as HTMLSelectElement | null;
      if (!sel) throw new Error('rom select not rendered yet');
      expect(sel.options.length).toBe(2);
    });
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    expect(modelSel.options.length).toBe(1);
  });

  it('hides the ROM picker when each model has a single matching ROM', async () => {
    setOpfsBackend(
      new StubOpfs({
        rom: [
          { name: 'Plus_v3.rom', path: '/opfs/images/rom/Plus_v3.rom', kind: 'file' },
          { name: 'SE30.rom', path: '/opfs/images/rom/SE30.rom', kind: 'file' },
        ],
      }),
    );
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    expect(container.querySelector('#cfg-rom')).toBeNull();
    const modelSel = container.querySelector('#cfg-model') as HTMLSelectElement;
    const labels = Array.from(modelSel.options).map((o) => o.textContent);
    expect(labels).toEqual(expect.arrayContaining(['Macintosh Plus', 'Macintosh SE/30']));
  });

  it('falls back to MockOpfs by default in the test setup', async () => {
    setOpfsBackend(new MockOpfs());
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel) throw new Error('not ready');
      expect(sel.options.length).toBeGreaterThan(0);
    });
  });
});
