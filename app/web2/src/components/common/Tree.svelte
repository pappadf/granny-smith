<script lang="ts">
  import TreeRow from './TreeRow.svelte';
  import Tree from './Tree.svelte';
  import { pathKey } from '@/lib/treePath';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';
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
    /** Shared lazy-children cache. The root Tree creates one; recursive
     *  instances receive it via prop so the root can walk the flat
     *  visible list for keyboard navigation. */
    lazyCache?: Record<string, TreeNode[]>;
    onToggle: (path: string[]) => void;
    onSelect?: (path: string[]) => void;
    onActivate?: (path: string[]) => void;
    onContextMenu?: (path: string[], ev: MouseEvent) => void;
    onDragStart?: (path: string[], ev: DragEvent) => void;
    onDragOver?: (path: string[], ev: DragEvent) => void;
    onDragLeave?: (path: string[], ev: DragEvent) => void;
    onDragEnd?: (path: string[], ev: DragEvent) => void;
    onDrop?: (path: string[], ev: DragEvent) => void;
    /** Lazy children loader; result is cached in `lazyCache`. */
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
    lazyCache,
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

  // Root creates its own cache via $state; recursive children receive
  // the root's cache via `lazyCache` and use that. Mutations through
  // `cache[k] = ...` are reactive in either case (root: directly on
  // its own $state object; child: the prop is a reactive object owned
  // by the root). The lazyCache prop is captured once at mount —
  // a child Tree's cache reference doesn't switch out from under it.
  const localCache: Record<string, TreeNode[]> = $state({});
  // svelte-ignore state_referenced_locally
  const cache = lazyCache ?? localCache;
  const loading = $state<Record<string, boolean>>({});

  function pathOf(node: TreeNode): string[] {
    return [...parentPath, node.id];
  }

  function childrenOf(node: TreeNode): TreeNode[] | null {
    if (node.children) return node.children;
    if (node.leaf) return null;
    const k = pathKey(pathOf(node));
    return cache[k] ?? null;
  }

  function hasChildren(node: TreeNode): boolean {
    if (node.leaf) return false;
    if (node.children?.length) return true;
    if (!loadChildren) return false;
    return true;
  }

  async function maybeLoadChildren(node: TreeNode): Promise<void> {
    if (node.leaf || !loadChildren) return;
    if (node.children) return;
    const k = pathKey(pathOf(node));
    if (cache[k] || loading[k]) return;
    loading[k] = true;
    try {
      const kids = await loadChildren(pathOf(node));
      cache[k] = kids;
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
        void maybeLoadChildren(node);
      }
    } else {
      onActivate?.(p);
    }
  }

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

  // ---- Keyboard navigation (root only) -----------------------------
  //
  // Walks the visible rows depth-first via childrenOf() — which consults
  // the shared cache — to give ↑/↓ a flat-list feel even across
  // lazy-loaded subtrees. ←/→ collapse/expand the selected branch;
  // Enter activates. Home/End jump to the first/last visible row.

  function flatten(): Array<{ path: string[]; node: TreeNode; branch: boolean; open: boolean }> {
    const out: Array<{ path: string[]; node: TreeNode; branch: boolean; open: boolean }> = [];
    const walk = (ns: TreeNode[], parent: string[]) => {
      for (const n of ns) {
        const p = [...parent, n.id];
        const branch = hasChildren(n);
        const open = !!expanded[pathKey(p)];
        out.push({ path: p, node: n, branch, open });
        if (branch && open) {
          const kids = n.children ?? cache[pathKey(p)];
          if (kids) walk(kids, p);
        }
      }
    };
    walk(nodes, parentPath);
    return out;
  }

  function onRootKey(ev: KeyboardEvent) {
    if (depth !== 0) return;
    const flat = flatten();
    if (!flat.length) return;
    const currentIdx = selectedKey ? flat.findIndex((r) => pathKey(r.path) === selectedKey) : -1;

    // ←/→: collapse/expand the current row.
    if (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') {
      if (currentIdx < 0) return;
      const row = flat[currentIdx];
      if (!row.branch) {
        // On a leaf, ← jumps to the parent row (if any).
        if (ev.key === 'ArrowLeft' && row.path.length > parentPath.length + 1) {
          ev.preventDefault();
          const parentP = row.path.slice(0, -1);
          onSelect?.(parentP);
        }
        return;
      }
      const wantOpen = ev.key === 'ArrowRight';
      if (row.open !== wantOpen) {
        ev.preventDefault();
        onToggle(row.path);
        if (wantOpen) void maybeLoadChildren(row.node);
      }
      return;
    }

    // Enter activates / toggles.
    if (ev.key === 'Enter') {
      if (currentIdx < 0) return;
      ev.preventDefault();
      const row = flat[currentIdx];
      handleRowClick(row.node);
      return;
    }

    // ↑/↓/Home/End/PgUp/PgDn navigate.
    const k = listKeyFromEvent(ev);
    if (!k) return;
    if (k === 'ArrowLeft' || k === 'ArrowRight') return;
    const next = cycleListSelection(flat.length, currentIdx, k, { wrap: false });
    if (next === currentIdx) return;
    ev.preventDefault();
    onSelect?.(flat[next].path);
  }
</script>

{#if depth === 0}
  <ul class="tree" role="tree" tabindex="0" onkeydown={onRootKey}>
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
            lazyCache={cache}
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
{:else}
  <ul class="tree" role="group">
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
            lazyCache={cache}
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
{/if}

<style>
  .tree {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .tree:focus {
    outline: none;
  }
  .tree:focus-visible {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
</style>
