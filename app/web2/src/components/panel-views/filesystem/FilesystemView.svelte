<script lang="ts">
  import { onMount } from 'svelte';
  import Tree, { type TreeNode } from '@/components/common/Tree.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import RenameDialog from './RenameDialog.svelte';
  import { opfs } from '@/bus/opfs';
  import { gsEval } from '@/bus/emulator';
  import { vfsList } from '@/bus/vfs';
  import { acceptFilesRaw } from '@/bus/upload';
  import { isMacArchive, sanitizeName } from '@/lib/archive';
  import { isDiskImage, isInImageSpace, listViaVfs } from '@/lib/diskImage';
  import { UPLOAD_DIR } from '@/lib/opfsPaths';
  import { showNotification } from '@/state/toasts.svelte';
  import { bumpImagesRevision } from '@/state/images.svelte';
  import {
    filesystem,
    toggleFsExpanded,
    setFsDragSource,
    setFsSelected,
  } from '@/state/filesystem.svelte';
  import { iconForFsEntry } from '@/lib/iconForFsEntry';
  import { pathIsAncestorOrSelf, pathKey } from '@/lib/treePath';

  const DRAG_MIME = 'application/x-gs-tree-path';

  // Cache: pathKey → loaded children. Cleared on user-initiated mutations.
  const childrenCache = $state<Record<string, TreeNode[]>>({});

  // Node id (full path) → entry kind. The TreeNode `leaf` flag can't carry
  // this: a disk image is a file yet rendered expandable (leaf=false), so we
  // track kind separately to decide what's downloadable. Read imperatively in
  // the context-menu handler, so it doesn't need to be reactive.
  const nodeKind: Record<string, 'file' | 'directory'> = {};

  // Monotonic suffix for unique scratch filenames when extracting a file out
  // of a read-only image for download.
  let downloadSeq = 0;

  // The path array of the node currently being dragged. Kept verbatim (not
  // re-derived from the space-joined pathKey) because in-image names routinely
  // contain spaces — splitting the key on ' ' would corrupt the path and, in
  // turn, the copy-vs-move drop effect.
  let draggedPath: string[] | null = null;

  // /opfs is the root.
  let rootNodes = $state<TreeNode[]>([
    { id: '/opfs', label: '/opfs', icon: 'folder', draggable: false },
  ]);

  let dropTargetKey = $state<string | null>(null);
  let renameOpen = $state(false);
  let renameTarget = $state<string | null>(null);

  // Bumped by refresh() to remount the tree after a mutation. The Tree
  // shares childrenCache (below), so a remount re-reads whatever keys we
  // invalidated and leaves untouched ones cached.
  let treeKey = $state(0);

  function entriesToNodes(
    entries: { name: string; path: string; kind: 'file' | 'directory' }[],
  ): TreeNode[] {
    return entries
      .slice()
      .sort((a, b) => {
        if (a.kind !== b.kind) return a.kind === 'directory' ? -1 : 1;
        return a.name.localeCompare(b.name);
      })
      .map((e) => {
        nodeKind[e.path] = e.kind;
        // A disk image that is a real OPFS file (not itself inside another
        // image) is expandable: expanding it lists its partitions via VFS.
        const expandableImage = e.kind === 'file' && isDiskImage(e.name) && !isInImageSpace(e.path);
        return {
          id: e.path,
          label: e.name,
          icon: iconForFsEntry(e),
          leaf: e.kind === 'file' && !expandableImage,
          // Every entry is draggable. An OPFS node moves; a node inside a
          // (read-only) image is instead copied out to the drop target.
          draggable: true,
        };
      });
  }

  async function loadChildren(path: string[]): Promise<TreeNode[]> {
    const dir = path[path.length - 1];
    const k = pathKey(path);
    if (childrenCache[k]) return childrenCache[k];
    // Paths that cross a disk-image boundary (the image file itself, or any
    // partition / directory inside it) are listed through the read-only VFS;
    // everything else is plain OPFS.
    const entries = listViaVfs(dir) ? await vfsList(dir) : await opfs.list(dir);
    const nodes = entriesToNodes(entries);
    childrenCache[k] = nodes;
    return nodes;
  }

  // Force the tree to re-read its (shared) children cache. Callers delete
  // the affected dir keys from childrenCache first; remounting then refetches
  // exactly those, so an upload / move / delete / unpack shows up live
  // instead of only after a tab switch or reload.
  async function refresh(): Promise<void> {
    treeKey++;
    // A Filesystem-tab mutation can add/remove/rename anything under
    // /opfs/images/ (e.g. copying a disk into /opfs/images/fd/). Signal the
    // image-catalog watchers — the New Machine dialog and the Images tab —
    // so their dropdowns re-scan instead of staying stale until a reload.
    bumpImagesRevision();
    await Promise.resolve();
  }

  onMount(() => {
    void loadChildren(['/opfs']);
  });

  function handleDragStart(path: string[], ev: DragEvent) {
    if (!ev.dataTransfer) return;
    draggedPath = path;
    ev.dataTransfer.setData(DRAG_MIME, JSON.stringify(path));
    // A node inside a read-only image is copied out; an OPFS node is moved.
    ev.dataTransfer.effectAllowed = isInImageSpace(path[path.length - 1]) ? 'copy' : 'move';
    setFsDragSource(pathKey(path));
  }

  function handleDragOver(path: string[], ev: DragEvent) {
    if (!ev.dataTransfer) return;
    const types = ev.dataTransfer.types;
    const isInternal = !!types && Array.from(types).includes(DRAG_MIME);
    const isExternalFile = !!types && Array.from(types).includes('Files');
    if (!isInternal && !isExternalFile) return;
    // Target must be a folder for either kind of drop.
    const parentKey = pathKey(path.slice(0, -1));
    const cached = childrenCache[parentKey];
    const node = cached?.find((n) => n.id === path[path.length - 1]);
    const isFolder = node && node.leaf === false;
    if (!isFolder) return;
    // Disk images and their contents are read-only — refuse drops onto an
    // image file node or anything inside it (would otherwise write bogus
    // OPFS entries under the image path).
    if (listViaVfs(path[path.length - 1])) return;
    if (isInternal) {
      const sourcePath = draggedPath;
      if (!sourcePath) return;
      if (pathIsAncestorOrSelf(sourcePath, path)) return;
      if (pathKey(sourcePath.slice(0, -1)) === pathKey(path)) return;
      ev.preventDefault();
      // Copy out of an image; move within OPFS. dropEffect MUST agree with the
      // effectAllowed set at dragstart, or the browser silently rejects the
      // drop (this is what broke dragging out files with spaces in the name).
      ev.dataTransfer.dropEffect = isInImageSpace(sourcePath[sourcePath.length - 1])
        ? 'copy'
        : 'move';
      dropTargetKey = pathKey(path);
    } else {
      // External file drop — copy into the folder, no validation.
      ev.preventDefault();
      ev.dataTransfer.dropEffect = 'copy';
      dropTargetKey = pathKey(path);
    }
  }

  function handleDragLeave(path: string[]) {
    if (dropTargetKey === pathKey(path)) dropTargetKey = null;
  }

  function handleDragEnd() {
    setFsDragSource(null);
    dropTargetKey = null;
    draggedPath = null;
  }

  async function handleDrop(path: string[], ev: DragEvent) {
    ev.preventDefault();
    const data = ev.dataTransfer?.getData(DRAG_MIME);
    const externalFiles = !data ? Array.from(ev.dataTransfer?.files ?? []) : [];
    if (!data && externalFiles.length === 0) {
      handleDragEnd();
      return;
    }
    const dstParent = path[path.length - 1];
    // Never write into a disk image (read-only) — drop into its OPFS parent.
    if (listViaVfs(dstParent)) {
      handleDragEnd();
      return;
    }

    if (data) {
      // Internal tree-to-tree drag.
      let sourcePath: string[];
      try {
        sourcePath = JSON.parse(data) as string[];
      } catch {
        handleDragEnd();
        return;
      }
      const src = sourcePath[sourcePath.length - 1];
      const name = src.split('/').pop() ?? '';
      const dst = `${dstParent}/${name}`;

      if (isInImageSpace(src)) {
        // The source lives in a read-only image — copy it OUT to the OPFS
        // drop target via the VFS-backed storage.cp (recursive for a folder)
        // rather than moving it.
        const recursive = nodeKind[src] === 'directory';
        try {
          const args = recursive ? ['-r', src, dst] : [src, dst];
          const ok = (await gsEval('storage.cp', args)) === true;
          if (ok) {
            delete childrenCache[pathKey(path)];
            await refresh();
            showNotification(`Copied '${name}' to '${dstParent}'`, 'info');
          } else {
            showNotification(`Failed to copy '${name}' out of the disk image`, 'error');
          }
        } catch {
          showNotification('Copy failed', 'error');
        } finally {
          handleDragEnd();
        }
        return;
      }

      try {
        await opfs.move(src, dst);
        // Invalidate both parents.
        delete childrenCache[pathKey(sourcePath.slice(0, -1))];
        delete childrenCache[pathKey(path)];
        await refresh();
        showNotification(`Moved '${name}' to '${dstParent}'`, 'info');
      } catch {
        showNotification('Move failed', 'error');
      } finally {
        handleDragEnd();
      }
      return;
    }

    // External-file drop. The Filesystem view is the low-level OPFS
    // browser, so we deliberately skip media-type validation here —
    // the user can put any file anywhere. Use Path 2 (Display drop)
    // or Path 4 (Images-tab category drop) if you want validation.
    try {
      await acceptFilesRaw(externalFiles, dstParent);
      delete childrenCache[pathKey(path)];
      await refresh();
    } finally {
      handleDragEnd();
    }
  }

  // True when the row at `path` is a file (has bytes), per its tracked entry
  // kind. Note a disk-image node is a file even though it renders expandable.
  function isFile(path: string[]): boolean {
    return nodeKind[path[path.length - 1]] === 'file';
  }

  function handleContextMenu(path: string[], ev: MouseEvent) {
    ev.preventDefault();
    setFsSelected(pathKey(path));
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? '';

    // Inside a disk image everything is read-only — the only sensible action
    // is downloading a file's contents out of the image. Directories and
    // partitions offer nothing, so no menu opens for them.
    if (isInImageSpace(target)) {
      if (!isFile(path)) return;
      openContextMenu(
        [{ label: 'Download', action: () => doDownload(path) }],
        ev.clientX,
        ev.clientY,
      );
      return;
    }

    // Plain OPFS node.
    const items: ContextMenuItem[] = [{ label: 'Rename', action: () => beginRename(path) }];
    if (isFile(path)) {
      items.push({ label: 'Download', action: () => doDownload(path) });
      // "Unpack" only for files whose extension peeler recognises
      // (.sit / .hqx / .cpt / .bin / .sea).
      if (isMacArchive(name)) items.push({ label: 'Unpack', action: () => doUnpack(path) });
    }
    items.push({ sep: true });
    items.push({ label: 'Delete', action: () => doDelete(path), danger: true });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Extract a peeler-recognised archive into a sibling "<name>_unpacked"
  // folder via the C-side archive module, then reveal it in the tree.
  async function doUnpack(path: string[]) {
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? '';
    const parentDir = target.replace(/\/[^/]+$/, '');
    const base = name.replace(/\.[^.]+$/, '') || name;
    const outDir = `${parentDir}/${base}_unpacked`;
    showNotification(`Unpacking '${name}'…`, 'info');
    let ok = false;
    try {
      ok = (await gsEval('archive.extract', [target, outDir])) === true;
    } catch {
      ok = false;
    }
    if (!ok) {
      showNotification(`Failed to unpack '${name}'`, 'error');
      return;
    }
    // Reveal the new folder: invalidate the parent listing and refresh.
    delete childrenCache[pathKey(path.slice(0, -1))];
    await refresh();
    showNotification(`Unpacked '${name}' to '${base}_unpacked'`, 'info');
  }

  function beginRename(path: string[]) {
    renameTarget = path[path.length - 1];
    renameOpen = true;
  }

  async function commitRename(newName: string) {
    if (!renameTarget) return;
    const old = renameTarget;
    renameOpen = false;
    renameTarget = null;
    try {
      await opfs.rename(old, newName);
      const parent = old.replace(/\/[^/]+$/, '');
      delete childrenCache[pathKey([parent])];
      await refresh();
      showNotification(`Renamed to '${newName}'`, 'info');
    } catch {
      showNotification('Rename failed', 'error');
    }
  }

  async function doDelete(path: string[]) {
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? '';
    if (typeof window !== 'undefined' && typeof window.confirm === 'function') {
      if (!window.confirm(`Delete '${name}'?`)) return;
    }
    try {
      await opfs.delete(target);
      delete childrenCache[pathKey(path.slice(0, -1))];
      await refresh();
      showNotification(`Deleted '${name}'`, 'info');
    } catch {
      showNotification('Delete failed', 'error');
    }
  }

  // Save a Blob to the user's machine via a transient object-URL anchor.
  function saveBlob(blob: Blob, filename: string) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  // Download a file to the host. For a plain OPFS file the bytes are read
  // straight from OPFS. For a file inside a (read-only) disk image we first
  // copy its data fork out to a scratch OPFS path via the VFS-backed
  // storage.cp, read that, then delete the scratch file.
  async function doDownload(path: string[]) {
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? 'download';
    if (!isFile(path)) {
      showNotification('Only files can be downloaded', 'warning');
      return;
    }
    try {
      if (isInImageSpace(target)) {
        const scratch = `${UPLOAD_DIR}/.dl-${downloadSeq++}-${sanitizeName(name)}`;
        const copied = (await gsEval('storage.cp', [target, scratch])) === true;
        if (!copied) {
          showNotification(`Failed to read '${name}' from the disk image`, 'error');
          return;
        }
        try {
          saveBlob(await opfs.readFile(scratch), name);
        } finally {
          await opfs.delete(scratch);
        }
      } else {
        saveBlob(await opfs.readFile(target), name);
      }
      showNotification(`Downloading '${name}'`, 'info');
    } catch (err) {
      console.error('download failed', err);
      showNotification(`Download failed: ${name}`, 'error');
    }
  }
</script>

<div class="fs-view">
  {#key treeKey}
    <Tree
      nodes={rootNodes}
      lazyCache={childrenCache}
      expanded={filesystem.expanded}
      selectedKey={filesystem.selectedPath}
      dragSourceKey={filesystem.dragSourcePath}
      {dropTargetKey}
      onToggle={(p) => toggleFsExpanded(pathKey(p))}
      onSelect={(p) => setFsSelected(pathKey(p))}
      onContextMenu={handleContextMenu}
      onDragStart={handleDragStart}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDragEnd={handleDragEnd}
      onDrop={handleDrop}
      {loadChildren}
    />
  {/key}
</div>

<RenameDialog
  open={renameOpen}
  initial={renameTarget?.split('/').pop() ?? ''}
  onSubmit={commitRename}
  onClose={() => {
    renameOpen = false;
    renameTarget = null;
  }}
/>

<style>
  .fs-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    padding: 4px 0;
  }
</style>
