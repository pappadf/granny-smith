<script lang="ts">
  import { commandsTree, type CommandNode } from '@/lib/commandsTree';
  import { showNotification } from '@/state/toasts.svelte';
  import { insertIntoTerminal } from './terminalBridge';
  import CommandBrowser from './CommandBrowser.svelte';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';

  interface Props {
    nodes?: CommandNode[];
    depth?: number;
    /** Shared expansion / selection / keyboard state (root creates it). */
    expandedState?: Record<string, boolean>;
    selectedKey?: string;
    onSelect?: (k: string) => void;
  }
  let {
    nodes = commandsTree,
    depth = 0,
    expandedState,
    selectedKey = $bindable(''),
    onSelect,
  }: Props = $props();

  const localExpanded: Record<string, boolean> = $state({});
  // svelte-ignore state_referenced_locally
  const expanded = expandedState ?? localExpanded;

  function keyOf(n: CommandNode, atDepth = depth): string {
    return n.insert ? `${atDepth}:${n.insert}` : `${atDepth}:${n.name}`;
  }

  function isOpen(n: CommandNode): boolean {
    // Top-level categories start collapsed; only expand on user click.
    return !!expanded[keyOf(n)];
  }

  function toggle(n: CommandNode) {
    if (!n.children?.length) return;
    const k = keyOf(n);
    expanded[k] = !expanded[k];
  }

  function onRowClick(n: CommandNode) {
    onSelect?.(keyOf(n));
    if (n.insert) {
      const ok = insertIntoTerminal(n.insert);
      if (ok) showNotification(`Inserted: ${n.insert}`, 'info');
      else showNotification('Open the Terminal tab to insert a command', 'warning');
    } else {
      toggle(n);
    }
  }

  function onTwistieClick(ev: MouseEvent, n: CommandNode) {
    ev.stopPropagation();
    toggle(n);
  }

  // ---- Keyboard navigation (root only) -----------------------------
  function flatten(): Array<{ key: string; node: CommandNode; atDepth: number }> {
    const out: Array<{ key: string; node: CommandNode; atDepth: number }> = [];
    const walk = (ns: CommandNode[], d: number) => {
      for (const n of ns) {
        const k = keyOf(n, d);
        out.push({ key: k, node: n, atDepth: d });
        if (n.children?.length && expanded[k]) walk(n.children, d + 1);
      }
    };
    walk(commandsTree, 0);
    return out;
  }

  function onRootKey(ev: KeyboardEvent) {
    if (depth !== 0) return;
    const flat = flatten();
    if (!flat.length) return;
    const currentIdx = selectedKey ? flat.findIndex((r) => r.key === selectedKey) : -1;

    if (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') {
      if (currentIdx < 0) return;
      const row = flat[currentIdx];
      const hasChildren = !!row.node.children?.length;
      if (!hasChildren) return;
      const wantOpen = ev.key === 'ArrowRight';
      if (!!expanded[row.key] !== wantOpen) {
        ev.preventDefault();
        toggle(row.node);
      }
      return;
    }

    if (ev.key === 'Enter') {
      if (currentIdx < 0) return;
      ev.preventDefault();
      onRowClick(flat[currentIdx].node);
      return;
    }

    const k = listKeyFromEvent(ev);
    if (!k) return;
    if (k === 'ArrowLeft' || k === 'ArrowRight') return;
    const next = cycleListSelection(flat.length, currentIdx, k, { wrap: false });
    if (next === currentIdx) return;
    ev.preventDefault();
    selectedKey = flat[next].key;
    onSelect?.(flat[next].key);
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
<ul
  class="cmd-tree"
  class:root={depth === 0}
  role={depth === 0 ? 'tree' : 'group'}
  tabindex={depth === 0 ? 0 : undefined}
  onkeydown={depth === 0 ? onRootKey : undefined}
>
  {#each nodes as node (keyOf(node))}
    {@const hasChildren = !!node.children?.length}
    {@const open = isOpen(node)}
    {@const k = keyOf(node)}
    {@const isSelected = selectedKey === k}
    <li
      class="cmd-row"
      class:category={!node.insert}
      class:selected={isSelected}
      role="treeitem"
      aria-selected={isSelected}
      aria-expanded={hasChildren ? open : undefined}
    >
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <div class="cmd-line" role="button" tabindex="-1" onclick={() => onRowClick(node)}>
        <span
          class="twistie"
          class:has={hasChildren}
          class:open
          onclick={(e) => onTwistieClick(e, node)}
          role="button"
          tabindex="-1"
          aria-label={hasChildren ? (open ? 'Collapse' : 'Expand') : ''}
          >{hasChildren ? (open ? '▼' : '▶') : ''}</span
        >
        <span class="name">{node.name}</span>
        {#if node.insert}
          <span class="insert-hint" title="Click to insert into terminal prompt">↵</span>
        {/if}
      </div>
      {#if open}
        <div class="desc">{node.desc}</div>
      {/if}
      {#if hasChildren && open}
        <CommandBrowser
          nodes={node.children}
          depth={depth + 1}
          expandedState={expanded}
          {selectedKey}
          {onSelect}
        />
      {/if}
    </li>
  {/each}
</ul>

<style>
  .cmd-tree {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .cmd-tree.root {
    padding: 6px 0;
    overflow-y: auto;
    height: 100%;
  }
  .cmd-tree:not(.root) {
    padding-left: 18px;
  }
  .cmd-row {
    margin: 0;
  }
  .cmd-row.selected > .cmd-line {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .cmd-tree.root:focus {
    outline: none;
  }
  .cmd-tree.root:focus-visible {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
  .cmd-line {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 2px 8px;
    cursor: pointer;
    height: 22px;
    color: var(--gs-fg);
    user-select: none;
  }
  .cmd-line:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .twistie {
    display: inline-block;
    width: 14px;
    color: var(--gs-fg-muted);
    font-size: 9px;
    text-align: center;
    flex-shrink: 0;
  }
  .twistie:not(.has) {
    visibility: hidden;
  }
  .name {
    font-size: 13px;
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .cmd-row.category > .cmd-line > .name {
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    font-size: 11px;
    color: var(--gs-fg-bright);
  }
  .insert-hint {
    color: var(--gs-fg-muted);
    opacity: 0;
    transition: opacity 100ms;
    font-size: 12px;
    margin-left: 6px;
  }
  .cmd-line:hover .insert-hint {
    opacity: 1;
  }
  .desc {
    color: var(--gs-fg-muted);
    background: var(--gs-info-bg, rgba(80, 140, 220, 0.08));
    border-left: 2px solid var(--gs-info-border, rgba(80, 140, 220, 0.5));
    padding: 6px 10px;
    margin: 2px 16px 4px 24px;
    font-size: 12px;
    line-height: 1.4;
    white-space: pre-wrap;
  }
</style>
