import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import FilesystemView from '@/components/panel-views/filesystem/FilesystemView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { filesystem, setFsExpanded } from '@/state/filesystem.svelte';

beforeEach(() => {
  setOpfsBackend(new MockOpfs());
  filesystem.expanded = { '/opfs': true };
  filesystem.dragSourcePath = null;
  filesystem.selectedPath = null;
});

describe('FilesystemView', () => {
  it('renders /opfs root', () => {
    const { container } = render(FilesystemView);
    const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
    expect(labels).toContain('/opfs');
  });

  it('expands /opfs and shows the seeded subdirs', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels.length).toBeGreaterThan(1);
      // MockOpfs seeds /opfs/images, /opfs/checkpoints, /opfs/upload, /opfs/config.
      expect(labels).toContain('images');
    });
  });

  it('right-click opens a context menu (mounted on document.body)', async () => {
    const { container } = render(FilesystemView);
    await waitFor(() => {
      expect(container.querySelector('.tree-row')).not.toBeNull();
    });
    const row = container.querySelector('.tree-row') as HTMLElement;
    await fireEvent.contextMenu(row);
    await waitFor(() => {
      expect(document.querySelector('.context-menu')).not.toBeNull();
    });
    // Cleanup — dismiss the menu so the next test starts clean.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });
});
