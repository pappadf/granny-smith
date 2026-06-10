import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';

// Mock the emulator bridge so vfs.list returns canned partition / volume
// listings — exercises the Filesystem tree's descent into a disk image
// without a running WASM module. Declared via vi.hoisted so the spy exists
// before the (hoisted) vi.mock factory runs.
const { gsEvalMock } = vi.hoisted(() => ({ gsEvalMock: vi.fn() }));

vi.mock('@/bus/emulator', () => ({
  gsEval: (path: string, args?: unknown[]) => gsEvalMock(path, args),
  gsErrorText: (res: unknown) => String(res),
  isModuleReady: () => true,
  getModule: () => null,
}));

import FilesystemView from '@/components/panel-views/filesystem/FilesystemView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import type { OpfsEntry } from '@/bus/types';
import { filesystem, setFsExpanded, clearFsSelection } from '@/state/filesystem.svelte';
import { makeDataTransfer, labels, rowFor } from '../helpers/fsTree';

// Backend whose /opfs root holds a disk image, a plain file, and a target
// folder. Tracks readFile / delete / move so the download and drag tests can
// assert the flow.
class ImgRootOpfs extends MockOpfs {
  readFileCalls: string[] = [];
  deleteCalls: string[] = [];
  moveCalls: [string, string][] = [];
  async list(dir: string): Promise<OpfsEntry[]> {
    if (dir === '/opfs') {
      return [
        { name: 'extracted', path: '/opfs/extracted', kind: 'directory' },
        { name: 'disk.img', path: '/opfs/disk.img', kind: 'file' },
        { name: 'notes.txt', path: '/opfs/notes.txt', kind: 'file' },
      ];
    }
    return [];
  }
  async readFile(path: string): Promise<Blob> {
    this.readFileCalls.push(path);
    const blob = new Blob(['payload']);
    // jsdom's Blob lacks arrayBuffer(); downloadOne materialises the bytes
    // through it before deleting the scratch file, so shim the one method.
    if (typeof blob.arrayBuffer !== 'function') {
      (blob as unknown as { arrayBuffer: () => Promise<ArrayBuffer> }).arrayBuffer = async () =>
        new TextEncoder().encode('payload').buffer as ArrayBuffer;
    }
    return blob;
  }
  async delete(path: string): Promise<void> {
    this.deleteCalls.push(path);
  }
  async move(src: string, dst: string): Promise<void> {
    this.moveCalls.push([src, dst]);
  }
}

const createObjectURL = vi.fn(() => 'blob:mock');

let backend: ImgRootOpfs;

beforeEach(() => {
  backend = new ImgRootOpfs();
  setOpfsBackend(backend);
  filesystem.expanded = { '/opfs': true };
  filesystem.dragSourcePath = null;
  clearFsSelection();
  createObjectURL.mockClear();
  // jsdom lacks these; stub so saveBlob() runs without navigating.
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = createObjectURL;
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = () => {};
  vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
  gsEvalMock.mockReset();
  gsEvalMock.mockImplementation(async (path: string, args?: unknown[]) => {
    if (path === 'storage.cp') return true;
    if (path !== 'vfs.list') return null;
    const dir = (args?.[0] as string) ?? '';
    if (dir === '/opfs/disk.img') {
      return JSON.stringify([
        { name: 'partition1', kind: 'directory', size: 0 },
        { name: 'partition2', kind: 'directory', size: 0 },
      ]);
    }
    if (dir === '/opfs/disk.img/partition1') {
      return JSON.stringify([
        { name: 'System Folder', kind: 'directory', size: 0 },
        { name: 'Read Me', kind: 'file', size: 4522 },
        // A name the HFS reader surfaced from an in-name '/': OPFS rejects ':'.
        { name: 'Install 1:2.img', kind: 'file', size: 819200 },
      ]);
    }
    return JSON.stringify([]);
  });
});

describe('FilesystemView — disk-image descent', () => {
  it('collapses a lone synthetic partition (floppy shows volume contents directly)', async () => {
    // A floppy has no partition map — the VFS reports a single partition1.
    gsEvalMock.mockImplementation(async (path: string, args?: unknown[]) => {
      if (path !== 'vfs.list') return null;
      const dir = (args?.[0] as string) ?? '';
      if (dir === '/opfs/disk.img')
        return JSON.stringify([{ name: 'partition1', kind: 'directory', size: 0 }]);
      if (dir === '/opfs/disk.img/partition1')
        return JSON.stringify([
          { name: 'System Folder', kind: 'directory', size: 0 },
          { name: 'Read Me', kind: 'file', size: 4522 },
        ]);
      return JSON.stringify([]);
    });

    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));

    await waitFor(() => {
      const l = labels(container);
      expect(l).toContain('System Folder');
      expect(l).toContain('Read Me');
    });
    // The redundant partition level is hidden.
    expect(labels(container)).not.toContain('partition1');
  });

  it('expands a disk image into its partitions, then the volume contents', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));

    // disk.img is expandable (a twistie row), notes.txt is a plain leaf.
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => {
      expect(labels(container)).toContain('partition1');
      expect(labels(container)).toContain('partition2');
    });
    expect(gsEvalMock).toHaveBeenCalledWith('vfs.list', ['/opfs/disk.img']);

    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => {
      expect(labels(container)).toContain('System Folder');
      expect(labels(container)).toContain('Read Me');
    });
    expect(gsEvalMock).toHaveBeenCalledWith('vfs.list', ['/opfs/disk.img/partition1']);
  });

  it('shows no context menu for a read-only node inside an image', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));

    await fireEvent.contextMenu(rowFor(container, 'partition1'));
    // Give any menu a chance to mount, then assert none did.
    await Promise.resolve();
    expect(document.querySelector('.context-menu')).toBeNull();
  });

  it('still offers a full context menu on the image file itself', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));

    await fireEvent.contextMenu(rowFor(container, 'disk.img'));
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const items = Array.from(document.querySelectorAll('.context-menu .item')).map((e) =>
      e.textContent?.trim(),
    );
    expect(items).toContain('Delete');
    expect(items).toContain('Rename');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });

  it('downloads a file inside an image by extracting it via storage.cp', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));
    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => expect(labels(container)).toContain('Read Me'));

    await fireEvent.contextMenu(rowFor(container, 'Read Me'));
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const download = Array.from(document.querySelectorAll('.context-menu .item')).find(
      (e) => e.textContent?.trim() === 'Download',
    ) as HTMLElement;
    expect(download).toBeTruthy();
    await fireEvent.click(download);

    await waitFor(() => {
      const cp = gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp');
      expect(cp).toBeTruthy();
      const [src, scratch] = cp![1] as [string, string];
      expect(src).toBe('/opfs/disk.img/partition1/Read Me');
      // The data fork is copied to a scratch path, read, then cleaned up.
      expect(backend.readFileCalls).toContain(scratch);
      expect(backend.deleteCalls).toContain(scratch);
      expect(createObjectURL).toHaveBeenCalled();
    });
  });

  it('downloads a plain OPFS file by reading it directly (no storage.cp)', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('notes.txt'));

    await fireEvent.contextMenu(rowFor(container, 'notes.txt'));
    await waitFor(() => expect(document.querySelector('.context-menu')).not.toBeNull());
    const download = Array.from(document.querySelectorAll('.context-menu .item')).find(
      (e) => e.textContent?.trim() === 'Download',
    ) as HTMLElement;
    expect(download).toBeTruthy();
    await fireEvent.click(download);

    await waitFor(() => {
      expect(backend.readFileCalls).toContain('/opfs/notes.txt');
      expect(createObjectURL).toHaveBeenCalled();
    });
    expect(gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp')).toBeUndefined();
  });

  it('copies a file OUT of an image when dragged to an OPFS folder', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));
    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => expect(labels(container)).toContain('Read Me'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'Read Me'), { dataTransfer: dt });
    // A read-only image source copies (it can't be moved out).
    expect(dt.effectAllowed).toBe('copy');
    await fireEvent.drop(rowFor(container, 'extracted'), { dataTransfer: dt });

    await waitFor(() => {
      const cp = gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp');
      expect(cp).toBeTruthy();
      expect(cp![1]).toEqual(['/opfs/disk.img/partition1/Read Me', '/opfs/extracted/Read Me']);
    });
    // The source is never deleted — the image is read-only.
    expect(backend.deleteCalls).toHaveLength(0);
  });

  it('copies a folder OUT of an image recursively (-r)', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));
    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => expect(labels(container)).toContain('System Folder'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'System Folder'), { dataTransfer: dt });
    await fireEvent.drop(rowFor(container, 'extracted'), { dataTransfer: dt });

    await waitFor(() => {
      const cp = gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp');
      expect(cp).toBeTruthy();
      expect(cp![1]).toEqual([
        '-r',
        '/opfs/disk.img/partition1/System Folder',
        '/opfs/extracted/System Folder',
      ]);
    });
  });

  it('sanitises an OPFS-hostile name (colon) when copying out of an image', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));
    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => expect(labels(container)).toContain('Install 1:2.img'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'Install 1:2.img'), { dataTransfer: dt });
    await fireEvent.drop(rowFor(container, 'extracted'), { dataTransfer: dt });

    await waitFor(() => {
      const cp = gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp');
      expect(cp).toBeTruthy();
      // Source keeps its ':'; destination is sanitised so the OPFS write
      // succeeds instead of silently failing.
      expect(cp![1]).toEqual([
        '/opfs/disk.img/partition1/Install 1:2.img',
        '/opfs/extracted/Install 1_2.img',
      ]);
    });
  });

  it('moves (not copies) a plain OPFS file dragged within OPFS', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('notes.txt'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'notes.txt'), { dataTransfer: dt });
    expect(dt.effectAllowed).toBe('move');
    await fireEvent.drop(rowFor(container, 'extracted'), { dataTransfer: dt });

    await waitFor(() => {
      expect(backend.moveCalls).toContainEqual(['/opfs/notes.txt', '/opfs/extracted/notes.txt']);
    });
    expect(gsEvalMock.mock.calls.find((c) => c[0] === 'storage.cp')).toBeUndefined();
  });

  // Regression: dragOver must set a dropEffect compatible with the dragStart
  // effectAllowed, or the browser silently rejects the drop. The source path
  // here ("Read Me") contains a space, which previously got mangled when the
  // path was re-derived from the space-joined key, flipping copy → move.
  it('sets dropEffect=copy when dragging a spaced in-image file over OPFS', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('disk.img'));
    await fireEvent.click(rowFor(container, 'disk.img'));
    await waitFor(() => expect(labels(container)).toContain('partition1'));
    await fireEvent.click(rowFor(container, 'partition1'));
    await waitFor(() => expect(labels(container)).toContain('Read Me'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'Read Me'), { dataTransfer: dt });
    expect(dt.effectAllowed).toBe('copy');
    await fireEvent.dragOver(rowFor(container, 'extracted'), { dataTransfer: dt });
    expect(dt.dropEffect).toBe('copy');
  });

  it('sets dropEffect=move when dragging an OPFS file over an OPFS folder', async () => {
    const { container } = render(FilesystemView);
    setFsExpanded('/opfs', true);
    await waitFor(() => expect(labels(container)).toContain('notes.txt'));

    const dt = makeDataTransfer();
    await fireEvent.dragStart(rowFor(container, 'notes.txt'), { dataTransfer: dt });
    await fireEvent.dragOver(rowFor(container, 'extracted'), { dataTransfer: dt });
    expect(dt.dropEffect).toBe('move');
  });
});
