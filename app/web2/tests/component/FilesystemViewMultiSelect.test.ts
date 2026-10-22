import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import FilesystemView from '@/components/panel-views/filesystem/FilesystemView.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';
import type { OpfsEntry } from '@/bus/types';
import { filesystem, setFsExpanded, clearFsSelection } from '@/state/filesystem.svelte';
import { makeDataTransfer, rowFor } from '../helpers/fsTree';

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

    // Confirm in the styled dialog.
    const confirmBtn = await waitFor(() => {
      const b = Array.from(container.querySelectorAll('.modal-actions button')).find(
        (e) => e.textContent?.trim() === 'Delete',
      );
      expect(b).toBeTruthy();
      return b as HTMLElement;
    });
    await fireEvent.click(confirmBtn);

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

  it('arrow keys navigate from the clicked row (multi-select trees pass only selectedKeys)', async () => {
    const container = await renderExpanded();
    await fireEvent.click(rowFor(container, 'a.txt'));
    expect(selectedLabels(container)).toEqual(['a.txt']);
    // Keyboard nav must derive the current row from the selection set — the
    // Filesystem tree no longer passes the legacy single-select selectedKey.
    const tree = container.querySelector('ul[role="tree"]') as HTMLElement;
    await fireEvent.keyDown(tree, { key: 'ArrowDown' });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['b.txt']));
    await fireEvent.keyDown(tree, { key: 'ArrowDown' });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['c.txt']));
    await fireEvent.keyDown(tree, { key: 'ArrowUp' });
    await waitFor(() => expect(selectedLabels(container)).toEqual(['b.txt']));
  });

  it('Enter and → open the /opfs root again after Enter closed it (S1018-73)', async () => {
    const container = await renderExpanded();
    const tree = container.querySelector('ul[role="tree"]') as HTMLElement;
    const rootRow = () => rowFor(container, '/opfs');
    // Keys go to whatever has focus, as in a browser. A clicked row takes
    // focus (tabindex -1); jsdom's click does not do that by itself.
    const key = (k: string) =>
      fireEvent.keyDown(document.activeElement ?? document.body, { key: k });
    await fireEvent.click(rowFor(container, 'dst'));
    rowFor(container, 'dst').focus();
    await key('ArrowUp');
    await waitFor(() => expect(selectedLabels(container)).toEqual(['/opfs']));
    expect(rootRow().getAttribute('aria-expanded')).toBe('true');
    // Closing the root takes away the focused 'dst' row; focus must stay in
    // the tree or every later key goes nowhere.
    await key('Enter');
    await waitFor(() => expect(rootRow().getAttribute('aria-expanded')).toBe('false'));
    await waitFor(() => expect(tree.contains(document.activeElement)).toBe(true));
    expect(selectedLabels(container)).toEqual(['/opfs']);
    await key('Enter');
    await waitFor(() => expect(rootRow().getAttribute('aria-expanded')).toBe('true'));
    await key('ArrowLeft');
    await waitFor(() => expect(rootRow().getAttribute('aria-expanded')).toBe('false'));
    await key('ArrowRight');
    await waitFor(() => expect(rootRow().getAttribute('aria-expanded')).toBe('true'));
    await waitFor(() => expect(container.textContent).toContain('a.txt'));
    expect(selectedLabels(container)).toEqual(['/opfs']);
  });

  it('→ and Enter open and close a folder below the root (S1018-73)', async () => {
    const container = await renderExpanded();
    const tree = container.querySelector('ul[role="tree"]') as HTMLElement;
    const key = (k: string) =>
      fireEvent.keyDown(document.activeElement ?? document.body, { key: k });
    backend.list = async (dir: string) =>
      dir === '/opfs/dst'
        ? [{ name: 'inner.txt', path: '/opfs/dst/inner.txt', kind: 'file' as const }]
        : MultiOpfs.prototype.list.call(backend, dir);
    // The keys act on the row's whole path: they used to drop the '/opfs'
    // ancestor, so Enter selected a row that is not there and toggled
    // nothing, and → listed the folder under a key no row reads.
    await fireEvent.click(rowFor(container, 'a.txt'));
    rowFor(container, 'a.txt').focus();
    await key('ArrowUp');
    await waitFor(() => expect(selectedLabels(container)).toEqual(['dst']));
    await key('ArrowRight');
    await waitFor(() => expect(container.textContent).toContain('inner.txt'));
    expect(rowFor(container, 'dst').getAttribute('aria-expanded')).toBe('true');
    await key('Enter');
    await waitFor(() =>
      expect(rowFor(container, 'dst').getAttribute('aria-expanded')).toBe('false'),
    );
    expect(selectedLabels(container)).toEqual(['dst']);
    await key('Enter');
    await waitFor(() => expect(container.textContent).toContain('inner.txt'));
    await fireEvent.click(rowFor(container, 'inner.txt'));
    rowFor(container, 'inner.txt').focus();
    await key('ArrowUp');
    await waitFor(() => expect(selectedLabels(container)).toEqual(['dst']));
    await key('Enter');
    await waitFor(() =>
      expect(rowFor(container, 'dst').getAttribute('aria-expanded')).toBe('false'),
    );
    await waitFor(() => expect(tree.contains(document.activeElement)).toBe(true));
    expect(selectedLabels(container)).toEqual(['dst']);
  });
});
