<script lang="ts">
  import TreeRow from './TreeRow.svelte';
  import Tree from './Tree.svelte';
  import { pathKey } from '@/lib/treePath';
  import type { IconName } from '@/lib/icons';

  export interface TreeNode {
    id: string;
    label: string;
    icon?: IconName;
    desc?: string;
    /** True for non-expandable rows; children won't be requested. */
    leaf?: boolean;
    draggable?: boolean;
    /** Pre-computed children (static mode). When omitted + !leaf, children
     *  are fetched lazily via loadChildren on first expand. */
    children?: TreeNode[];
  }

  interface Props {
    nodes: TreeNode[];
    expanded: Record<string, boolean>;
    selectedKey?: string | null;
    dragSourceKey?: string | null;
    dropTargetKey?: string | null;
    /** Parent path of this Tree instance (root passes []; recursion appends). */
    parentPath?: string[];
    depth?: number;
    onToggle: (path: string[]) => void;
    onSelect?: (path: string[]) => void;
    onActivate?: (path: string[]) => void;
    onContextMenu?: (path: string[], ev: MouseEvent) => void;
    onDragStart?: (path: string[], ev: DragEvent) => void;
    onDragOver?: (path: string[], ev: DragEvent) => void;
    onDragLeave?: (path: string[], ev: DragEvent) => void;
    onDragEnd?: (path: string[], ev: DragEvent) => void;
    onDrop?: (path: string[], ev: DragEvent) => void;
    /** Lazy children loader; result is cached in `lazyChildren`. */
    loadChildren?: (path: string[]) => Promise<TreeNode[]>;
  }
  let {
    nodes,
    expanded,
    selectedKey = null,
    dragSourceKey = null,
    dropTargetKey = null,
    parentPath = [],
    depth = 0,
    onToggle,
    onSelect,
    onActivate,
    onContextMenu,
    onDragStart,
    onDragOver,
    onDragLeave,
    onDragEnd,
    onDrop,
    loadChildren,
  }: Props = $props();

  // Cache of lazy children, keyed by pathKey. Only used when loadChildren
  // is provided.
  const lazyChildren = $state<Record<string, TreeNode[]>>({});
  const loading = $state<Record<string, boolean>>({});

  function pathOf(node: TreeNode): string[] {
    return [...parentPath, node.id];
  }

  function childrenOf(node: TreeNode): TreeNode[] | null {
    if (node.children) return node.children;
    if (node.leaf) return null;
    const k = pathKey(pathOf(node));
    return lazyChildren[k] ?? null;
  }

  function hasChildren(node: TreeNode): boolean {
    if (node.leaf) return false;
    if (node.children?.length) return true;
    // If we don't know yet, assume expandable so the twistie renders.
    if (!loadChildren) return false;
    return true;
  }

  async function maybeLoadChildren(node: TreeNode): Promise<void> {
    if (node.leaf || !loadChildren) return;
    if (node.children) return;
    const k = pathKey(pathOf(node));
    if (lazyChildren[k] || loading[k]) return;
    loading[k] = true;
    try {
      const kids = await loadChildren(pathOf(node));
      lazyChildren[k] = kids;
    } finally {
      loading[k] = false;
    }
  }

  function isOpen(node: TreeNode): boolean {
    return !!expanded[pathKey(pathOf(node))];
  }

  function handleRowClick(node: TreeNode) {
    const p = pathOf(node);
    onSelect?.(p);
    if (hasChildren(node)) {
      onToggle(p);
      if (!isOpen(node)) {
        // (was closed, will be open) — kick off lazy load
        void maybeLoadChildren(node);
      }
    } else {
      onActivate?.(p);
    }
  }

  // Kick off lazy loads when a branch is rendered already-open (e.g. the
  // parent restored an expansion set from session state, or a test mounts
  // with a seeded `expanded` map).
  $effect(() => {
    for (const node of nodes) {
      if (!loadChildren) continue;
      if (node.leaf || node.children) continue;
      if (!isOpen(node)) continue;
      void maybeLoadChildren(node);
    }
  });

  function handleTwistieClick(node: TreeNode, ev: MouseEvent) {
    ev.stopPropagation();
    if (!hasChildren(node)) return;
    const p = pathOf(node);
    if (!isOpen(node)) void maybeLoadChildren(node);
    onToggle(p);
  }
</script>

<ul class="tree" role={depth === 0 ? 'tree' : 'group'}>
  {#each nodes as node (node.id)}
    {@const p = pathOf(node)}
    {@const k = pathKey(p)}
    {@const open = isOpen(node)}
    {@const kids = childrenOf(node)}
    {@const branch = hasChildren(node)}
    <li>
      <TreeRow
        label={node.label}
        icon={node.icon}
        desc={node.desc}
        {depth}
        hasChildren={branch}
        {open}
        selected={selectedKey === k}
        draggable={!!node.draggable}
        dragSource={dragSourceKey === k}
        dropTarget={dropTargetKey === k}
        onClick={() => handleRowClick(node)}
        onTwistieClick={(ev) => handleTwistieClick(node, ev)}
        onContextMenu={onContextMenu ? (ev) => onContextMenu(p, ev) : undefined}
        onDoubleClick={onActivate ? () => onActivate(p) : undefined}
        onDragStart={onDragStart ? (ev) => onDragStart(p, ev) : undefined}
        onDragOver={onDragOver ? (ev) => onDragOver(p, ev) : undefined}
        onDragLeave={onDragLeave ? (ev) => onDragLeave(p, ev) : undefined}
        onDragEnd={onDragEnd ? (ev) => onDragEnd(p, ev) : undefined}
        onDrop={onDrop ? (ev) => onDrop(p, ev) : undefined}
      />
      {#if branch && open && kids}
        <Tree
          nodes={kids}
          {expanded}
          {selectedKey}
          {dragSourceKey}
          {dropTargetKey}
          parentPath={p}
          depth={depth + 1}
          {onToggle}
          {onSelect}
          {onActivate}
          {onContextMenu}
          {onDragStart}
          {onDragOver}
          {onDragLeave}
          {onDragEnd}
          {onDrop}
          {loadChildren}
        />
      {/if}
    </li>
  {/each}
</ul>

<style>
  .tree {
    list-style: none;
    margin: 0;
    padding: 0;
  }
</style>
