import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

// The Configuration slide drives the model dropdown by calling
// `rom.identify` on every ROM in OPFS, then `machine.profile` to get the
// human-readable name. Mock both so tests don't need a live WASM module.
vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('Plus_v3.rom')) {
          return JSON.stringify({
            recognised: true,
            checksum: 'plus-checksum',
            name: 'Macintosh Plus ROM',
            compatible: ['plus'],
            size: 128 * 1024,
          });
        }
        if (p.endsWith('SE30.rom')) {
          return JSON.stringify({
            recognised: true,
            checksum: 'se30-checksum',
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

  it('renders the remaining fields with prototype defaults', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-model') as HTMLSelectElement | null;
      if (!sel || sel.options.length === 0) throw new Error('not ready');
    });
    expect((container.querySelector('#cfg-vrom') as HTMLSelectElement).value).toBe('(auto)');
    expect((container.querySelector('#cfg-ram') as HTMLSelectElement).value).toBe('4 MB');
    expect((container.querySelector('#cfg-fd') as HTMLSelectElement).value).toBe('(none)');
    expect((container.querySelector('#cfg-cd') as HTMLSelectElement).value).toBe('(none)');
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
