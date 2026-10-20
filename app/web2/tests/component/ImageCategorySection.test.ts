import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';

// The floppy section's context menu: with several drives, each one by
// number, so a disk can go into drive 2 while drive 1 is empty.  The drive
// helpers are mocked so the test sees which drive the insert asks for.
const { mediaMock, drives } = vi.hoisted(() => ({
  mediaMock: { insertFloppy: vi.fn() },
  drives: { count: 2, present: [false, false] },
}));

vi.mock('@/bus/emulator', () => ({
  gsEval: async (path: string) => {
    const m = /^machine\.floppy\.drive\[(\d+)\]\.present$/.exec(path);
    return m ? drives.present[Number(m[1])] : null;
  },
  gsErrorText: (res: unknown) => String(res),
  gsEvalWithProgress: vi.fn(),
  isModuleReady: () => true,
  getModule: () => null,
}));

vi.mock('@/bus/media', () => ({
  insertFloppy: (...a: unknown[]) => mediaMock.insertFloppy(...a),
  detectFdDriveCount: async () => drives.count,
  ejectMedia: vi.fn(),
  mountImage: vi.fn(),
  machineDevices: async () => [],
  deviceLabel: () => '',
  imageInUseUnder: async (paths: string[]) => paths.find((p) => images.mounted[p]) ?? null,
}));

import ImageCategorySection from '@/components/panel-views/images/ImageCategorySection.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';
import { images } from '@/state/images.svelte';
import { dialogs } from '@/state/dialogs.svelte';
import { toasts, _resetForTests as resetToasts } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { ejectMedia } from '@/bus/media';

beforeEach(() => {
  setOpfsBackend(new MockOpfs());
  dialogs.current = null;
  resetToasts();
  images.mounted = {};
  drives.count = 2;
  drives.present = [false, false];
  mediaMock.insertFloppy.mockReset();
  mediaMock.insertFloppy.mockResolvedValue({
    ok: true,
    mount: { kind: 'fd', bus: 'floppy', drive: 1 },
  });
});

async function menuFor(name: string): Promise<HTMLElement[]> {
  const { container } = render(ImageCategorySection, {
    cat: 'fd',
    open: true,
    onToggle: () => {},
  });
  let row: HTMLElement | undefined;
  await waitFor(() => {
    row = Array.from(container.querySelectorAll<HTMLElement>('.image-row')).find((r) =>
      r.querySelector('.name')?.textContent?.includes(name),
    );
    expect(row).toBeTruthy();
  });
  await fireEvent.contextMenu(row!);
  await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
  return Array.from(document.querySelectorAll<HTMLElement>('.context-menu .item'));
}

const labelsOf = (items: HTMLElement[]) => items.map((e) => e.textContent?.trim() ?? '');

describe('ImageCategorySection — floppy drive choice', () => {
  it('offers each drive by number, and inserts into the one chosen', async () => {
    const items = await menuFor('Disk_Tools.dsk');
    expect(labelsOf(items)).toEqual(
      expect.arrayContaining(['Insert', 'Insert into drive 1', 'Insert into drive 2']),
    );
    await fireEvent.click(items.find((e) => e.textContent?.trim() === 'Insert into drive 2')!);
    await waitFor(() =>
      expect(mediaMock.insertFloppy).toHaveBeenCalledWith(
        '/opfs/images/fd/Disk_Tools.dsk',
        true,
        1,
      ),
    );
  });

  it('marks a drive that holds a disk as unavailable', async () => {
    drives.present = [true, false];
    const items = await menuFor('Disk_Tools.dsk');
    const drive1 = items.find((e) => e.textContent?.trim() === 'Insert into drive 1')!;
    expect(drive1.getAttribute('aria-disabled')).toBe('true');
    const drive2 = items.find((e) => e.textContent?.trim() === 'Insert into drive 2')!;
    expect(drive2.hasAttribute('aria-disabled')).toBe(false);
  });

  it('offers no drive list on a one-drive machine', async () => {
    drives.count = 1;
    const items = labelsOf(await menuFor('Disk_Tools.dsk'));
    expect(items).toContain('Insert');
    expect(items.some((l) => l.startsWith('Insert into drive'))).toBe(false);
  });
});

describe('ImageCategorySection — rename and delete', () => {
  const click = (items: HTMLElement[], label: string) =>
    fireEvent.click(items.find((e) => e.textContent?.trim() === label)!);

  // S1018-02: a rename or delete under a running machine made every later
  // checkpoint, Save State and Resume fail.
  it.each(['Rename', 'Delete'])('refuses %s of a mounted image', async (label) => {
    images.mounted['/opfs/images/fd/Disk_Tools.dsk'] = { kind: 'fd', bus: 'floppy', drive: 0 };
    await click(await menuFor('Disk_Tools.dsk'), label);
    await waitFor(() =>
      expect(toasts.active.some((t) => /while the machine is using it/.test(t.msg))).toBe(true),
    );
    expect(dialogs.current).toBeNull();
  });

  // S1018-70: a name another image has is refused under the field.
  it('refuses a name another image already has', async () => {
    await click(await menuFor('Disk_Tools.dsk'), 'Rename');
    await waitFor(() => expect(dialogs.current?.kind).toBe('text'));
    const q = dialogs.current as { validate: (v: string) => string };
    expect(q.validate('System_7.0_Install.dsk')).toMatch(/already exists/);
    expect(q.validate('Disk_Tools.dsk')).toBe('');
    expect(q.validate('fresh.dsk')).toBe('');
  });
});

// S1018-03: Eject of a floppy the Mac is using lost its unsaved writes
// without a word.  It asks first, and ejects only when confirmed.
describe('ImageCategorySection — eject of a floppy in use', () => {
  const where = { kind: 'fd', bus: 'floppy', drive: 0 } as const;
  beforeEach(() => {
    images.mounted['/opfs/images/fd/Disk_Tools.dsk'] = where;
    vi.mocked(ejectMedia).mockReset();
    vi.mocked(ejectMedia).mockResolvedValue({ ok: true } as Awaited<ReturnType<typeof ejectMedia>>);
  });
  const eject = async () =>
    fireEvent.click(
      (await menuFor('Disk_Tools.dsk')).find((e) => e.textContent?.trim() === 'Eject')!,
    );

  it.each([
    [true, 1],
    [false, 0],
  ])('asks first, warning of lost writes (confirmed: %s)', async (answer, ejects) => {
    machine.status = 'running';
    try {
      await eject();
      await waitFor(() => expect(dialogs.current?.kind).toBe('confirm'));
      const q = dialogs.current as { message: string; resolve: (ok: boolean) => void };
      expect(q.message).toMatch(/not yet written to it is lost/);
      expect(ejectMedia).not.toHaveBeenCalled();
      dialogs.current = null;
      q.resolve(answer);
      await waitFor(() => expect(ejectMedia).toHaveBeenCalledTimes(ejects));
      if (!answer) await new Promise((r) => setTimeout(r, 0));
      expect(ejectMedia).toHaveBeenCalledTimes(ejects);
      expect(images.mounted['/opfs/images/fd/Disk_Tools.dsk'] ? 0 : 1).toBe(ejects);
    } finally {
      machine.status = 'no-machine';
    }
  });

  it('ejects without asking when no machine is running', async () => {
    machine.status = 'no-machine';
    await eject();
    await waitFor(() => expect(ejectMedia).toHaveBeenCalledWith(where));
    expect(dialogs.current).toBeNull();
  });
});
