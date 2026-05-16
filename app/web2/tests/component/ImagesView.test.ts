import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import ImagesView from '@/components/panel-views/images/ImagesView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { images } from '@/state/images.svelte';

beforeEach(() => {
  setOpfsBackend(new MockOpfs());
  // Open every category so we can see all rows in one render.
  images.collapsed = { rom: false, vrom: false, fd: false, hd: false, cd: false };
  images.mounted = {};
});

describe('ImagesView', () => {
  it('renders one CollapsibleSection per category', async () => {
    const { container } = render(ImagesView);
    await waitFor(() => {
      const titles = Array.from(container.querySelectorAll('.title')).map((e) => e.textContent);
      expect(titles).toEqual(['ROM', 'VROM', 'Floppy Disk', 'Hard Disk', 'CD-ROM']);
    });
  });

  it('lists the seeded fixture images for ROM', async () => {
    const { container } = render(ImagesView);
    await waitFor(() => {
      const names = Array.from(container.querySelectorAll('.image-row .name')).map(
        (e) => e.textContent,
      );
      expect(names).toContain('Plus_v3.rom');
      expect(names).toContain('SE30.rom');
    });
  });

  it('clicking a section header toggles the collapsed bit', async () => {
    const { container } = render(ImagesView);
    await waitFor(() => {
      expect(container.querySelectorAll('.image-row').length).toBeGreaterThan(0);
    });
    const romHeader = Array.from(container.querySelectorAll('.header')).find((h) =>
      h.textContent?.includes('ROM'),
    ) as HTMLElement;
    await fireEvent.click(romHeader);
    expect(images.collapsed.rom).toBe(true);
  });
});
