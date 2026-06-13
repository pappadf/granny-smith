import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import FilesystemView from '@/components/panel-views/filesystem/FilesystemView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import type { OpfsEntry } from '@/bus/types';
import { filesystem, setFsExpanded, clearFsSelection } from '@/state/filesystem.svelte';
import { images } from '@/state/images.svelte';

// Click the "Delete" button in the styled confirmation dialog.
async function confirmDeleteDialog(container: HTMLElement) {
  const btn = await waitFor(() => {
    const b = Array.from(container.querySelectorAll('.modal-actions button')).find(
      (e) => e.textContent?.trim() === 'Delete',
    );
    expect(b).toBeTruthy();
    return b as HTMLElement;
  });
  await fireEvent.click(btn);
}

beforeEach(() => {
  setOpfsBackend(new MockOpfs());
  filesystem.expanded = { '/opfs': true };
  filesystem.dragSourcePath = null;
  clearFsSelection();
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
    // /opfs starts expanded; use one of its children — the root row itself
    // deliberately has no context menu.
    await waitFor(() => {
      expect(container.querySelectorAll('.tree-row').length).toBeGreaterThan(1);
    });
    const rows = Array.from(container.querySelectorAll('.tree-row')) as HTMLElement[];
    await fireEvent.contextMenu(rows[1]);
    await waitFor(() => {
      expect(document.querySelector('.context-menu')).not.toBeNull();
    });
    // Cleanup — dismiss the menu so the next test starts clean.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });

  it('offers no context menu on the /opfs root row (Delete there would wipe storage)', async () => {
    const { container } = render(FilesystemView);
    await waitFor(() => {
      expect(container.querySelector('.tree-row')).not.toBeNull();
    });
    const root = container.querySelector('.tree-row') as HTMLElement;
    await fireEvent.contextMenu(root);
    // Give the menu a tick to (not) appear.
    await new Promise((r) => setTimeout(r, 20));
    expect(document.querySelector('.context-menu')).toBeNull();
  });
});

// A backend whose /opfs listing carries one peeler-recognised archive, one
// plain file, and a directory — so we can probe the context menu per kind.
class ArchiveFixtureOpfs extends MockOpfs {
  async list(dir: string): Promise<OpfsEntry[]> {
    if (dir === '/opfs') {
      return [
        { name: 'sub', path: '/opfs/sub', kind: 'directory' },
        { name: 'game.sit', path: '/opfs/game.sit', kind: 'file' },
        { name: 'disk.img', path: '/opfs/disk.img', kind: 'file' },
      ];
    }
    return [];
  }
}

describe('FilesystemView — Unpack context action', () => {
  beforeEach(() => {
    setOpfsBackend(new ArchiveFixtureOpfs());
    filesystem.expanded = { '/opfs': true };
    filesystem.dragSourcePath = null;
    clearFsSelection();
  });

  function menuLabels(): string[] {
    return Array.from(document.querySelectorAll('.context-menu .item')).map(
      (e) => e.textContent?.trim() ?? '',
    );
  }

  async function rightClick(container: HTMLElement, label: string) {
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).toContain(label);
    });
    const rows = Array.from(container.querySelectorAll('.tree-row')) as HTMLElement[];
    const row = rows.find((r) => r.querySelector('.label')?.textContent === label);
    expect(row, `row '${label}' not found`).toBeTruthy();
    await fireEvent.contextMenu(row!);
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
  }

  it('offers Unpack for a peeler-recognised archive file', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await rightClick(container, 'game.sit');
    expect(menuLabels()).toContain('Unpack');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });

  it('omits Unpack for a non-archive file', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await rightClick(container, 'disk.img');
    const labels = menuLabels();
    expect(labels).not.toContain('Unpack');
    expect(labels).toContain('Delete');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });

  it('omits Unpack for a directory', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await rightClick(container, 'sub');
    expect(menuLabels()).not.toContain('Unpack');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });
});

// Backend whose /opfs listing is driven by a mutable set, so a delete is
// observable on the next list() — exercises the live-refresh path.
class MutableRootOpfs extends MockOpfs {
  rootFiles = new Set(['alpha.txt', 'beta.txt']);
  async list(dir: string): Promise<OpfsEntry[]> {
    if (dir === '/opfs') {
      return Array.from(this.rootFiles).map((name) => ({
        name,
        path: `/opfs/${name}`,
        kind: 'file' as const,
      }));
    }
    return [];
  }
  async delete(path: string): Promise<void> {
    this.rootFiles.delete(path.split('/').pop() ?? '');
  }
}

describe('FilesystemView — live refresh after mutation', () => {
  beforeEach(() => {
    setOpfsBackend(new MutableRootOpfs());
    filesystem.expanded = { '/opfs': true };
    filesystem.dragSourcePath = null;
    clearFsSelection();
    window.confirm = () => true;
  });

  it('removes a deleted row from the tree without a tab switch', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).toContain('beta.txt');
      expect(labels).toContain('alpha.txt');
    });

    const row = Array.from(container.querySelectorAll('.tree-row')).find(
      (r) => r.querySelector('.label')?.textContent === 'beta.txt',
    ) as HTMLElement;
    await fireEvent.contextMenu(row);
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const del = Array.from(document.querySelectorAll('.context-menu .item')).find(
      (e) => e.textContent?.trim() === 'Delete',
    ) as HTMLElement;
    await fireEvent.click(del);
    await confirmDeleteDialog(container);

    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).not.toContain('beta.txt');
      expect(labels).toContain('alpha.txt');
    });
  });

  it('bumps the image-catalog revision so the config dialog re-scans', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).toContain('beta.txt');
    });
    const before = images.revision;
    const row = Array.from(container.querySelectorAll('.tree-row')).find(
      (r) => r.querySelector('.label')?.textContent === 'beta.txt',
    ) as HTMLElement;
    await fireEvent.contextMenu(row);
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const del = Array.from(document.querySelectorAll('.context-menu .item')).find(
      (e) => e.textContent?.trim() === 'Delete',
    ) as HTMLElement;
    await fireEvent.click(del);
    await confirmDeleteDialog(container);
    await waitFor(() => expect(images.revision).toBeGreaterThan(before));
  });
});
