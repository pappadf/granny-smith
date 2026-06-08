<script lang="ts">
  import { onMount } from 'svelte';
  import Tree, { type TreeNode, type SelectMods } from '@/components/common/Tree.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import RenameDialog from './RenameDialog.svelte';
  import ConfirmDialog from '@/components/dialogs/ConfirmDialog.svelte';
  import { opfs } from '@/bus/opfs';
  import { gsEval } from '@/bus/emulator';
  import { vfsList } from '@/bus/vfs';
  import { acceptFilesRaw } from '@/bus/upload';
  import { isMacArchive, sanitizeName } from '@/lib/archive';
  import { isDiskImage, isInImageSpace, listViaVfs, imageRootOf } from '@/lib/diskImage';
  import { UPLOAD_DIR } from '@/lib/opfsPaths';
  import { showNotification } from '@/state/toasts.svelte';
  import { bumpImagesRevision } from '@/state/images.svelte';
  import { startUpload, finishUpload } from '@/state/uploads.svelte';
  import {
    filesystem,
    toggleFsExpanded,
    setFsDragSource,
    selectOnly,
    toggleSelected,
    setFsSelection,
    clearFsSelection,
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

  // Path arrays of the node(s) currently being dragged — one, or the whole
  // multi-selection. Kept verbatim (not re-derived from the space-joined
  // pathKey) because in-image names routinely contain spaces, which would
  // corrupt the path and the copy-vs-move drop effect.
  let draggedPaths: string[][] | null = null;

  // /opfs is the root.
  let rootNodes = $state<TreeNode[]>([
    { id: '/opfs', label: '/opfs', icon: 'folder', draggable: false },
  ]);

  let dropTargetKey = $state<string | null>(null);
  let renameOpen = $state(false);
  let renameTarget = $state<string | null>(null);

  // Delete-confirmation dialog state.
  let confirmOpen = $state(false);
  let confirmMessage = $state('');
  let pendingDelete = $state<string[][]>([]);

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
    let entries: { name: string; path: string; kind: 'file' | 'directory' }[];
    if (listViaVfs(dir)) {
      entries = await vfsList(dir);
      // A floppy (or any raw, unpartitioned HFS volume) has no partition map —
      // the VFS synthesises a single "partition1". Skip that redundant level
      // and show the volume contents directly under the image. Real multi-
      // partition images (HD / CD with an APM) keep their partition list.
      if (
        !isInImageSpace(dir) &&
        entries.length === 1 &&
        entries[0].kind === 'directory' &&
        /^partition\d+$/i.test(entries[0].name)
      ) {
        entries = await vfsList(entries[0].path);
      }
    } else {
      entries = await opfs.list(dir);
    }
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
    const key = pathKey(path);
    // Dragging a row that's part of a multi-selection drags the whole set;
    // dragging anything else drags just that row.
    const sources =
      filesystem.selected.has(key) && filesystem.selected.size > 1
        ? effectiveTargets(path)
        : [path];
    draggedPaths = sources;
    ev.dataTransfer.setData(DRAG_MIME, JSON.stringify(sources));
    // Sources share a parent, so they're uniformly in-image or not: a
    // read-only image source copies out, an OPFS source moves.
    ev.dataTransfer.effectAllowed = isInImageSpace(path[path.length - 1]) ? 'copy' : 'move';
    setFsDragSource(key);
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
      const sources = draggedPaths;
      if (!sources || !sources.length) return;
      // Don't drop into the dragged subtree, or back into the sources' parent.
      for (const s of sources) if (pathIsAncestorOrSelf(s, path)) return;
      if (pathKey(sources[0].slice(0, -1)) === pathKey(path)) return;
      ev.preventDefault();
      // Copy out of an image; move within OPFS. dropEffect MUST agree with the
      // effectAllowed set at dragstart, or the browser silently rejects the
      // drop (this is what broke dragging out files with spaces in the name).
      ev.dataTransfer.dropEffect = isInImageSpace(sources[0][sources[0].length - 1])
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
    draggedPaths = null;
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
      // Internal tree-to-tree drag (one source, or a same-parent selection).
      let sources: string[][];
      try {
        const parsed = JSON.parse(data) as unknown;
        sources =
          Array.isArray(parsed) && Array.isArray(parsed[0])
            ? (parsed as string[][])
            : [parsed as string[]];
      } catch {
        handleDragEnd();
        return;
      }
      if (!sources.length) {
        handleDragEnd();
        return;
      }
      // Sources share a parent, so they're uniformly in-image (copy out) or
      // OPFS (move).
      const fromImage = isInImageSpace(sources[0][sources[0].length - 1]);
      const verb = fromImage ? 'Copying' : 'Moving';
      const failures: string[] = [];
      let firstReason = '';
      try {
        for (let i = 0; i < sources.length; i++) {
          const s = sources[i];
          const src = s[s.length - 1];
          const name = src.split('/').pop() ?? '';
          // Surface progress immediately and per item — copying a large image
          // out can take a few seconds.
          startUpload(sources.length > 1 ? `${name} (${i + 1}/${sources.length})` : name, verb);
          if (fromImage) {
            // Copy OUT of a read-only image. Sanitise the destination name:
            // classic-Mac names routinely contain '/' (surfaced as ':' by the
            // HFS reader) and other characters OPFS rejects, which otherwise
            // makes the write fail for that item.
            const dst = `${dstParent}/${opfsSafeName(name)}`;
            const args = nodeKind[src] === 'directory' ? ['-r', src, dst] : [src, dst];
            let res = await gsEval('storage.cp', args);
            if (res !== true) {
              // A cached image auto-mount can wedge after intervening OPFS
              // changes (e.g. deleting an earlier copy). Drop it and retry once.
              const root = imageRootOf(src);
              if (root) {
                await gsEval('storage.unmount', [root]);
                res = await gsEval('storage.cp', args);
              }
            }
            if (res !== true) {
              failures.push(name);
              const reason = errText(res);
              if (!firstReason) firstReason = reason;
              console.error('drag copy-out failed:', src, '->', dst, '|', reason);
            }
          } else {
            const dst = `${dstParent}/${name}`;
            try {
              await opfs.move(src, dst);
            } catch (err) {
              failures.push(name);
              if (!firstReason) firstReason = String(err);
              console.error('drag move failed:', src, '->', dst, '|', err);
            }
          }
        }
        if (!fromImage) delete childrenCache[pathKey(sources[0].slice(0, -1))];
        delete childrenCache[pathKey(path)];
        clearFsSelection();
        await refresh();
        bulkToast(fromImage ? 'Copied' : 'Moved', dstParent, sources.length, failures, firstReason);
      } finally {
        finishUpload();
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

  // --- Multi-selection (siblings only) ---------------------------------
  //
  // Selection state lives in filesystem.selected (a Set of pathKeys) with
  // filesystem.anchor as the shift-range anchor. All selected nodes share a
  // parent, so they can be reconstructed from any one of them plus the
  // parent's cached child list.

  // Full path arrays for the rows an action should affect: the whole
  // selection when `path` is part of a multi-selection, else just `path`.
  function effectiveTargets(path: string[]): string[][] {
    const key = pathKey(path);
    if (!filesystem.selected.has(key) || filesystem.selected.size <= 1) return [path];
    const parentArr = path.slice(0, -1);
    const siblings = childrenCache[pathKey(parentArr)] ?? [];
    const targets = siblings
      .filter((n) => filesystem.selected.has(pathKey([...parentArr, n.id])))
      .map((n) => [...parentArr, n.id]);
    return targets.length ? targets : [path];
  }

  // True when every selected key is a sibling of `path` — keeps Cmd/Ctrl
  // toggling within one level.
  function sameParentAsSelection(path: string[]): boolean {
    if (filesystem.selected.size === 0) return true;
    const parentArr = path.slice(0, -1);
    const siblings = childrenCache[pathKey(parentArr)];
    if (!siblings) return false;
    const siblingKeys = new Set(siblings.map((n) => pathKey([...parentArr, n.id])));
    for (const k of filesystem.selected) if (!siblingKeys.has(k)) return false;
    return true;
  }

  // Mouse selection: plain = single; Cmd/Ctrl = toggle (same level only);
  // Shift = contiguous sibling range from the anchor.
  function handleSelect(path: string[], mods?: SelectMods) {
    const key = pathKey(path);
    if (mods?.shift && filesystem.anchor) {
      const parentArr = path.slice(0, -1);
      const siblings = childrenCache[pathKey(parentArr)];
      if (siblings) {
        const keys = siblings.map((n) => pathKey([...parentArr, n.id]));
        const a = keys.indexOf(filesystem.anchor);
        const b = keys.indexOf(key);
        if (a >= 0 && b >= 0) {
          const [lo, hi] = a <= b ? [a, b] : [b, a];
          setFsSelection(keys.slice(lo, hi + 1), filesystem.anchor);
          return;
        }
      }
      selectOnly(key); // anchor isn't a sibling → fresh single selection
    } else if (mods?.meta) {
      if (sameParentAsSelection(path)) toggleSelected(key);
      else selectOnly(key);
    } else {
      selectOnly(key);
    }
  }

  function handleContextMenu(path: string[], ev: MouseEvent) {
    ev.preventDefault();
    // Right-clicking outside the selection selects just that row; right-
    // clicking within it keeps the multi-selection.
    if (!filesystem.selected.has(pathKey(path))) selectOnly(pathKey(path));
    const targets = effectiveTargets(path);
    const multi = targets.length > 1;
    const name = path[path.length - 1].split('/').pop() ?? '';

    // Inside a disk image everything is read-only — only Download applies, and
    // only to file targets. (Targets share a parent, so they're uniformly
    // in-image or not.)
    if (isInImageSpace(path[path.length - 1])) {
      const files = targets.filter((t) => isFile(t));
      if (!files.length) return;
      openContextMenu(
        [
          {
            label: files.length > 1 ? `Download ${files.length} files` : 'Download',
            action: () => doDownload(targets),
          },
        ],
        ev.clientX,
        ev.clientY,
      );
      return;
    }

    // Plain OPFS nodes.
    const items: ContextMenuItem[] = [];
    if (!multi) items.push({ label: 'Rename', action: () => beginRename(path) });
    if (targets.some((t) => isFile(t)))
      items.push({
        label: multi ? 'Download files' : 'Download',
        action: () => doDownload(targets),
      });
    if (!multi && isFile(path) && isMacArchive(name))
      items.push({ label: 'Unpack', action: () => doUnpack(path) });
    items.push({ sep: true });
    items.push({
      label: multi ? `Delete ${targets.length} items` : 'Delete',
      action: () => doDelete(targets),
      danger: true,
    });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Extract a human-readable reason from a gsEval result. The bridge encodes a
  // C-side V_ERROR as {"error": "..."}; anything else stringifies as-is.
  function errText(res: unknown): string {
    if (res && typeof res === 'object' && 'error' in res) {
      return String((res as { error: unknown }).error);
    }
    return String(res);
  }

  // Toast for a bulk move/copy, naming the items that failed (if any) and the
  // first failure's reason.
  function bulkToast(verb: string, dst: string, total: number, failures: string[], reason = '') {
    if (failures.length) {
      const ok = total - failures.length;
      const list = failures.slice(0, 3).join(', ') + (failures.length > 3 ? '…' : '');
      const why = reason ? ` (${reason})` : '';
      showNotification(
        `${verb} ${ok}/${total} to '${dst}'. Failed: ${list}${why}`,
        ok ? 'warning' : 'error',
      );
    } else {
      showNotification(`${verb} ${total} item${total === 1 ? '' : 's'} to '${dst}'`, 'info');
    }
  }

  // Make a name safe for an OPFS / cross-platform destination: replace
  // characters OPFS or Windows reject (notably ':' which the HFS reader
  // produces from an in-name '/') and trim trailing dots/spaces. Spaces and
  // leading dots are kept. Used when copying OUT of a read-only image.
  function opfsSafeName(name: string): string {
    const safe = name.replace(/[\\/:*?"<>|]/g, '_').replace(/[ .]+$/g, '');
    return safe || 'untitled';
  }

  // Extract a peeler-recognised archive into a sibling "<name>_unpacked"
  // folder via the C-side archive module, then reveal it in the tree.
  async function doUnpack(path: string[]) {
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? '';
    const parentDir = target.replace(/\/[^/]+$/, '');
    const base = name.replace(/\.[^.]+$/, '') || name;
    const outDir = `${parentDir}/${base}_unpacked`;
    startUpload(name, 'Unpacking');
    let ok = false;
    try {
      ok = (await gsEval('archive.extract', [target, outDir])) === true;
    } catch {
      ok = false;
    } finally {
      finishUpload();
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

  // Open the styled confirmation; the actual delete runs on confirm.
  function doDelete(targets: string[][]) {
    if (!targets.length) return;
    const names = targets.map((t) => t[t.length - 1].split('/').pop() ?? '');
    confirmMessage =
      targets.length === 1
        ? `Delete '${names[0]}'? This can't be undone.`
        : `Delete these ${targets.length} items? This can't be undone.`;
    pendingDelete = targets;
    confirmOpen = true;
  }

  async function performDelete(targets: string[][]) {
    confirmOpen = false;
    if (!targets.length) return;
    const names = targets.map((t) => t[t.length - 1].split('/').pop() ?? '');
    const label = targets.length === 1 ? `'${names[0]}'` : `${targets.length} items`;
    let failed = 0;
    try {
      for (let i = 0; i < targets.length; i++) {
        const t = targets[i];
        const tname = t[t.length - 1].split('/').pop() ?? '';
        startUpload(
          targets.length > 1 ? `${tname} (${i + 1}/${targets.length})` : tname,
          'Deleting',
        );
        try {
          await opfs.delete(t[t.length - 1]);
        } catch {
          failed++;
        }
      }
    } finally {
      finishUpload();
    }
    delete childrenCache[pathKey(targets[0].slice(0, -1))];
    clearFsSelection();
    await refresh();
    const ok = targets.length - failed;
    if (failed)
      showNotification(
        `Deleted ${ok}/${targets.length}; ${failed} failed`,
        ok ? 'warning' : 'error',
      );
    else showNotification(`Deleted ${label}`, 'info');
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

  // Download the file targets to the host (folders/partitions are skipped).
  async function doDownload(targets: string[][]) {
    const files = targets.filter((t) => isFile(t));
    if (!files.length) {
      showNotification('Only files can be downloaded', 'warning');
      return;
    }
    let failed = 0;
    try {
      for (let i = 0; i < files.length; i++) {
        const fname = files[i][files[i].length - 1].split('/').pop() ?? '';
        startUpload(
          files.length > 1 ? `${fname} (${i + 1}/${files.length})` : fname,
          'Downloading',
        );
        if (!(await downloadOne(files[i]))) failed++;
      }
    } finally {
      finishUpload();
    }
    const ok = files.length - failed;
    if (failed) {
      showNotification(`Downloaded ${ok}/${files.length}`, ok ? 'warning' : 'error');
    } else if (files.length === 1) {
      showNotification(`Downloading '${files[0][files[0].length - 1].split('/').pop()}'`, 'info');
    } else {
      showNotification(`Downloading ${files.length} files`, 'info');
    }
  }

  // Download one file. For a plain OPFS file the bytes are read straight from
  // OPFS. For a file inside a (read-only) disk image we first copy its data
  // fork out to a scratch OPFS path via the VFS-backed storage.cp, read that,
  // then delete the scratch file. Returns false on failure.
  async function downloadOne(path: string[]): Promise<boolean> {
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? 'download';
    try {
      if (isInImageSpace(target)) {
        const scratch = `${UPLOAD_DIR}/.dl-${downloadSeq++}-${sanitizeName(name)}`;
        if ((await gsEval('storage.cp', [target, scratch])) !== true) return false;
        try {
          saveBlob(await opfs.readFile(scratch), name);
        } finally {
          await opfs.delete(scratch);
        }
      } else {
        saveBlob(await opfs.readFile(target), name);
      }
      return true;
    } catch (err) {
      console.error('download failed', err);
      return false;
    }
  }
</script>

<div class="fs-view">
  {#key treeKey}
    <Tree
      nodes={rootNodes}
      lazyCache={childrenCache}
      expanded={filesystem.expanded}
      selectedKeys={filesystem.selected}
      dragSourceKey={filesystem.dragSourcePath}
      {dropTargetKey}
      onToggle={(p) => toggleFsExpanded(pathKey(p))}
      onSelect={handleSelect}
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

<ConfirmDialog
  open={confirmOpen}
  title="Delete"
  message={confirmMessage}
  confirmText="Delete"
  danger
  onConfirm={() => void performDelete(pendingDelete)}
  onClose={() => (confirmOpen = false)}
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
