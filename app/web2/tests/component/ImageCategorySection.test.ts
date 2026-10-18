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
}));

import ImageCategorySection from '@/components/panel-views/images/ImageCategorySection.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';
import { images } from '@/state/images.svelte';

beforeEach(() => {
  setOpfsBackend(new MockOpfs());
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
