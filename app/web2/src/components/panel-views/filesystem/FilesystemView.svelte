<script lang="ts">
  import { onMount } from 'svelte';
  import Tree, { type TreeNode } from '@/components/common/Tree.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import RenameDialog from './RenameDialog.svelte';
  import { opfs } from '@/bus/opfs';
  import { showNotification } from '@/state/toasts.svelte';
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

  // /opfs is the root.
  let rootNodes = $state<TreeNode[]>([
    { id: '/opfs', label: '/opfs', icon: 'folder', draggable: false },
  ]);

  let dropTargetKey = $state<string | null>(null);
  let renameOpen = $state(false);
  let renameTarget = $state<string | null>(null);

  function entriesToNodes(
    entries: { name: string; path: string; kind: 'file' | 'directory' }[],
  ): TreeNode[] {
    return entries
      .slice()
      .sort((a, b) => {
        if (a.kind !== b.kind) return a.kind === 'directory' ? -1 : 1;
        return a.name.localeCompare(b.name);
      })
      .map((e) => ({
        id: e.path,
        label: e.name,
        icon: iconForFsEntry(e),
        leaf: e.kind === 'file',
        draggable: true,
      }));
  }

  async function loadChildren(path: string[]): Promise<TreeNode[]> {
    const dir = path[path.length - 1];
    const k = pathKey(path);
    if (childrenCache[k]) return childrenCache[k];
    const entries = await opfs.list(dir);
    const nodes = entriesToNodes(entries);
    childrenCache[k] = nodes;
    return nodes;
  }

  // Force a re-render of the tree by flushing the rootNodes binding.
  async function refresh(): Promise<void> {
    const root = rootNodes;
    rootNodes = [];
    await Promise.resolve();
    rootNodes = root;
  }

  onMount(() => {
    void loadChildren(['/opfs']);
  });

  function handleDragStart(path: string[], ev: DragEvent) {
    if (!ev.dataTransfer) return;
    ev.dataTransfer.setData(DRAG_MIME, JSON.stringify(path));
    ev.dataTransfer.effectAllowed = 'move';
    setFsDragSource(pathKey(path));
  }

  function handleDragOver(path: string[], ev: DragEvent) {
    if (!ev.dataTransfer) return;
    const types = ev.dataTransfer.types;
    if (!types || !Array.from(types).includes(DRAG_MIME)) return;
    const sourceKey = filesystem.dragSourcePath;
    if (!sourceKey) return;
    const sourcePath = sourceKey.split(' ');
    // Target must be a folder.
    const parentKey = pathKey(path.slice(0, -1));
    const cached = childrenCache[parentKey];
    const node = cached?.find((n) => n.id === path[path.length - 1]);
    const isFolder = node && node.leaf === false;
    if (!isFolder) return;
    if (pathIsAncestorOrSelf(sourcePath, path)) return;
    const sourceParent = sourcePath.slice(0, -1).join(' ');
    if (sourceParent === pathKey(path)) return;
    ev.preventDefault();
    ev.dataTransfer.dropEffect = 'move';
    dropTargetKey = pathKey(path);
  }

  function handleDragLeave(path: string[]) {
    if (dropTargetKey === pathKey(path)) dropTargetKey = null;
  }

  function handleDragEnd() {
    setFsDragSource(null);
    dropTargetKey = null;
  }

  async function handleDrop(path: string[], ev: DragEvent) {
    ev.preventDefault();
    const data = ev.dataTransfer?.getData(DRAG_MIME);
    if (!data) {
      handleDragEnd();
      return;
    }
    let sourcePath: string[];
    try {
      sourcePath = JSON.parse(data) as string[];
    } catch {
      handleDragEnd();
      return;
    }
    const src = sourcePath[sourcePath.length - 1];
    const name = src.split('/').pop() ?? '';
    const dstParent = path[path.length - 1];
    const dst = `${dstParent}/${name}`;
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
  }

  function handleContextMenu(path: string[], ev: MouseEvent) {
    ev.preventDefault();
    const items: ContextMenuItem[] = [
      { label: 'Rename', action: () => beginRename(path) },
      { label: 'Download', action: () => doDownload(path) },
      { sep: true },
      { label: 'Delete', action: () => doDelete(path), danger: true },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
    setFsSelected(pathKey(path));
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

  function doDownload(path: string[]) {
    // OPFS file download via Blob — folder download is Phase 7 (zip).
    const target = path[path.length - 1];
    const name = target.split('/').pop() ?? '';
    showNotification(`Download of '${name}' will land in a later phase`, 'warning');
  }
</script>

<div class="fs-view">
  <Tree
    nodes={rootNodes}
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
