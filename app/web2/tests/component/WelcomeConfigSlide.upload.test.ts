// "Upload image…" in a New Machine media dropdown (N-53): a successful upload
// selects the uploaded image, and a cancelled or rejected one leaves the
// previous pick -- in the state AND in the <select> the user sees.  The
// picker itself is mocked (tests/unit/filePicker.test.ts covers it).
import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine } from '@/state/machine.svelte';
import { setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

const pick = vi.hoisted(() => ({ result: null as string | null, calls: 0 }));
let opfs: MockOpfs;

vi.mock('@/bus/upload', () => ({
  pickAndUploadAs: async () => {
    pick.calls++;
    if (pick.result) opfs.addFile(pick.result, 40 << 20);
    return pick.result;
  },
}));

vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path === 'machine.rom.identify') {
        const p = (args?.[0] as string) ?? '';
        if (p.endsWith('plus-v3-4d1f8172.rom'))
          return {
            recognised: true,
            checksum: 'plus-checksum',
            name: 'Macintosh Plus ROM',
            compatible: ['plus'],
            size: 128 * 1024,
          };
        return null;
      }
      if (path === 'machine.profile')
        return {
          name: 'Macintosh Plus',
          ram_options: [4096],
          ram_default: 4096,
          floppy_slots: [{ label: 'Internal Floppy', kind: 'standard' }],
        };
      return null;
    },
  };
});

beforeEach(() => {
  _resetForTests();
  opfs = new MockOpfs();
  setOpfsBackend(opfs);
  machine.status = 'no-machine';
  pick.result = null;
  pick.calls = 0;
  setWelcomeSlide('configuration');
});

async function hdSelect(container: HTMLElement): Promise<HTMLSelectElement> {
  await waitFor(() => {
    const sel = container.querySelector('#cfg-hd') as HTMLSelectElement | null;
    if (!sel || !Array.from(sel.options).some((o) => o.value === 'hd1.img'))
      throw new Error('not ready');
  });
  return container.querySelector('#cfg-hd') as HTMLSelectElement;
}

// The form re-renders around the rescan an upload triggers: query afresh.
const hdValue = (container: HTMLElement) =>
  (container.querySelector('#cfg-hd') as HTMLSelectElement | null)?.value;

function choose(sel: HTMLSelectElement, value: string) {
  sel.value = value;
  sel.dispatchEvent(new Event('change', { bubbles: true }));
}

describe('WelcomeConfigSlide upload', () => {
  it('selects the image a successful upload stored', async () => {
    const { container } = render(WelcomeConfigSlide);
    const sel = await hdSelect(container);
    pick.result = '/opfs/images/hd/uploaded.img';
    choose(sel, 'Upload image...');
    await waitFor(() => expect(hdValue(container)).toBe('uploaded.img'));
    expect(pick.calls).toBe(1);
  });

  it('a cancelled upload keeps the previous pick on screen', async () => {
    const { container } = render(WelcomeConfigSlide);
    const sel = await hdSelect(container);
    choose(sel, 'hd2.img');
    await waitFor(() => expect(hdValue(container)).toBe('hd2.img'));
    choose(sel, 'Upload image...');
    await waitFor(() => expect(pick.calls).toBe(1));
    await waitFor(() => expect(hdValue(container)).toBe('hd2.img'));
  });
});
