import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import FilesystemView from '@/components/panel-views/filesystem/FilesystemView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import type { OpfsEntry } from '@/bus/types';
import { filesystem, setFsExpanded, clearFsSelection } from '@/state/filesystem.svelte';

// Root with four sibling files plus a destination folder. Tracks delete/move.
class MultiOpfs extends MockOpfs {
  rootFiles = new Set(['a.txt', 'b.txt', 'c.txt', 'd.txt']);
  moveCalls: [string, string][] = [];
  async list(dir: string): Promise<OpfsEntry[]> {
    if (dir === '/opfs') {
      return [
        { name: 'dst', path: '/opfs/dst', kind: 'directory' as const },
        ...[...this.rootFiles].sort().map((n) => ({
          name: n,
          path: `/opfs/${n}`,
          kind: 'file' as const,
        })),
      ];
    }
    return [];
  }
  async delete(path: string): Promise<void> {
    this.rootFiles.delete(path.split('/').pop() ?? '');
  }
  async move(src: string, dst: string): Promise<void> {
    this.moveCalls.push([src, dst]);
  }
}

function makeDataTransfer(): DataTransfer {
  const store: Record<string, string> = {};
  return {
    setData: (t: string, v: string) => {
      store[t] = v;
    },
    getData: (t: string) => store[t] ?? '',
    get types() {
      return Object.keys(store);
    },
    files: [] as unknown as FileList,
    items: [] as unknown as DataTransferItemList,
    dropEffect: 'none',
    effectAllowed: 'all',
    setDragImage: () => {},
  } as unknown as DataTransfer;
}

function rowFor(container: HTMLElement, label: string): HTMLElement {
  const row = Array.from(container.querySelectorAll('.tree-row')).find(
    (r) => r.querySelector('.label')?.textContent === label,
  );
  if (!row) throw new Error(`row '${label}' not found`);
  return row as HTMLElement;
}

function selectedLabels(container: HTMLElement): string[] {
  return Array.from(container.querySelectorAll('.tree-row.selected .label'))
    .map((e) => e.textContent ?? '')
    .sort();
}

let backend: MultiOpfs;
beforeEach(() => {
  backend = new MultiOpfs();
  setOpfsBackend(backend);
  filesystem.expanded = { '/opfs': true };
  filesystem.dragSourcePath = null;
  clearFsSelection();
  window.confirm = () => true;
});

async function renderExpanded() {
  const { container } = render(FilesystemView);
  setFsExpanded('/opfs', true);
  await waitFor(() => {
    const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
    expect(labels).toContain('a.txt');
    expect(labels).toContain('d.txt');
  });
  return container;
}

describe('FilesystemView — multi-select', () => {
  it('shift+click selects a contiguous sibling range', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'c.txt'), { shiftKey: true });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['a.txt', 'b.txt', 'c.txt']));
  });

  it('cmd/ctrl+click toggles individual siblings', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'c.txt'), { metaKey: true });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['a.txt', 'c.txt']));
    // Toggle a.txt back off.
    await fireEvent.click(rowFor(container, 'a.txt'), { metaKey: true });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['c.txt']));
  });

  it('plain click collapses the selection back to one', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'c.txt'), { shiftKey: true });
    await waitFor(() => expect(selectedLabels(container).length).toBe(3));
    await fireEvent.click(rowFor(container, 'b.txt'));
    await waitFor(() => expect(selectedLabels(container)).toEqual(['b.txt']));
  });

  it('bulk-deletes the whole selection from one context menu', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'b.txt'), { shiftKey: true });

    await fireEvent.contextMenu(rowFor(container, 'a.txt'));
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const del = Array.from(document.querySelectorAll('.context-menu .item')).find(
      (e) => e.textContent?.trim() === 'Delete 2 items',
    ) as HTMLElement;
    expect(del).toBeTruthy();
    await fireEvent.click(del);

    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).not.toContain('a.txt');
      expect(labels).not.toContain('b.txt');
      expect(labels).toContain('c.txt');
      expect(labels).toContain('d.txt');
    });
  });

  it('drags the whole selection to another folder (multi-move)', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'b.txt'), { shiftKey: true });

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'a.txt'), { dataTransfer: dt });
    await fireEvent.drop(rowFor(container, 'dst'), { dataTransfer: dt });

    await waitFor(() => {
      expect(backend.moveCalls).toContainEqual(['/opfs/a.txt', '/opfs/dst/a.txt']);
      expect(backend.moveCalls).toContainEqual(['/opfs/b.txt', '/opfs/dst/b.txt']);
    });
  });

  it('right-clicking outside the selection resets it to that row', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    await fireEvent.click(rowFor(container, 'b.txt'), { shiftKey: true });
    // Right-click a row that isn't part of the selection.
    await fireEvent.contextMenu(rowFor(container, 'd.txt'));
    await waitFor(() => expect(selectedLabels(container)).toEqual(['d.txt']));
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });
});
