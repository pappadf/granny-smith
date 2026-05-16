import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { _resetForTests } from '@/state/toasts.svelte';
import { layout } from '@/state/layout.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import type { OpfsBackend } from '@/bus/opfs';
import type { OpfsEntry, ImageCategory, RomInfo } from '@/bus/types';

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
  async readJson(): Promise<null> {
    return null;
  }
  async writeJson(): Promise<void> {
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
  it('shows the "no ROMs in storage" help when MockOpfs has empty fixtures', async () => {
    // MockOpfs's scanImages('rom') returns the prototype's hard-coded list —
    // override with an empty StubOpfs for this case.
    setOpfsBackend(new StubOpfs());
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      expect(container.querySelector('.form-help')?.textContent).toContain('No ROMs in storage');
    });
    const submit = container.querySelector('.primary-button') as HTMLButtonElement;
    expect(submit.disabled).toBe(true);
  });

  it('populates the ROM select when OPFS has entries', async () => {
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
      const sel = container.querySelector('#cfg-rom') as HTMLSelectElement | null;
      if (!sel) throw new Error('rom select not rendered yet');
      expect(sel.options.length).toBe(2);
    });
  });

  it('falls back to MockOpfs by default in the test setup', async () => {
    setOpfsBackend(new MockOpfs());
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      const sel = container.querySelector('#cfg-rom') as HTMLSelectElement | null;
      expect(sel?.options.length).toBeGreaterThan(0);
    });
  });
});
