<script lang="ts">
  // The Terminal's command browser: a structural view of the object model
  // (levels are path segments, as typed), for composing statements.  Every
  // row comes from the model (lib/commandsTree.ts): a leaf shows its segment,
  // the first sentence of its doc and, for an attribute, its type; the
  // selected leaf shows its usage text (shell.usage) underneath.  Task chips
  // (shell.tasks) filter by dimming; the Advanced toggle controls whether
  // advanced-tier members are shown at all.
  import { onDestroy, untrack } from 'svelte';
  import {
    expand,
    firstSentence,
    invalidate,
    invalidationFor,
    loadTasks,
    loadUsage,
    rootRows,
    typeText,
    visible,
    type BrowserRow,
    type TaskChip,
  } from '@/lib/commandsTree';
  import { onCoreEvent, whenModuleReady } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';
  import { insertIntoTerminal } from './terminalBridge';
  import Icon from '@/components/common/Icon.svelte';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';
  import { machine } from '@/state/machine.svelte';

  let roots = $state<BrowserRow[]>([]);
  // Loaded children per row key, and which rows are open.
  let children = $state<Record<string, BrowserRow[]>>({});
  let expanded = $state<Record<string, boolean>>({});
  let tasks = $state<TaskChip[]>([]);
  let task = $state<string | null>(null); // the selected task chip
  let showAdvanced = $state(false);
  let selectedKey = $state('');
  let usage = $state('');

  // Rebuild the root when a machine boots or goes (its members change).
  $effect(() => {
    void machine.status;
    untrack(() => void reload());
  });

  async function reload(): Promise<void> {
    await whenModuleReady();
    invalidate('');
    const [r, t] = await Promise.all([rootRows(), loadTasks()]);
    roots = r;
    tasks = t;
    // Re-read every level still open.
    const open = Object.keys(expanded).filter((k) => expanded[k]);
    children = {};
    for (const k of open) {
      const row = findRow(k);
      if (row) children[k] = await expand(row);
    }
  }

  // Events that change what a level holds drop its cache and re-read it.
  const unsubscribe = onCoreEvent((ev) => {
    const prefix = invalidationFor(`${ev.kind}:${ev.event}`);
    if (prefix === null) return;
    if (prefix === '') {
      void reload();
      return;
    }
    invalidate(prefix);
    for (const k of Object.keys(children)) {
      const row = findRow(k);
      if (
        row &&
        expanded[k] &&
        (row.path === prefix ||
          row.path.startsWith(`${prefix}.`) ||
          row.path.startsWith(`${prefix}[`))
      )
        void expand(row).then((rows) => (children[k] = rows));
    }
  });
  onDestroy(unsubscribe);

  function findRow(key: string): BrowserRow | undefined {
    const stack = [...roots];
    while (stack.length) {
      const r = stack.pop()!;
      if (r.key === key) return r;
      const c = children[r.key];
      if (c) stack.push(...c);
    }
    return undefined;
  }

  // Does a row belong to the selected task?  With no task, everything does.
  function matches(row: BrowserRow): boolean {
    return !task || row.task === task;
  }

  // An object whose own task is another one is a subtree with no match: it
  // stays collapsed while the filter is on.
  function filteredOut(row: BrowserRow): boolean {
    return (
      !!task && row.expandable && row.kind !== 'group' && row.task !== null && row.task !== task
    );
  }

  function isOpen(row: BrowserRow): boolean {
    return !!expanded[row.key] && !filteredOut(row);
  }

  interface FlatRow {
    row: BrowserRow;
    depth: number;
  }

  // The rows on screen, depth-first through the open levels.
  const flat = $derived.by(() => {
    const out: FlatRow[] = [];
    const walk = (rows: BrowserRow[], depth: number) => {
      for (const row of rows) {
        if (row.kind !== 'divider' && !visible(row, showAdvanced)) continue;
        out.push({ row, depth });
        if (row.expandable && isOpen(row) && children[row.key]) walk(children[row.key], depth + 1);
      }
    };
    walk(roots, 0);
    return out;
  });

  async function toggle(row: BrowserRow): Promise<void> {
    if (!row.expandable) return;
    if (expanded[row.key]) {
      expanded[row.key] = false;
      return;
    }
    // A collection is re-read on every expansion: entries come and go.
    if (!children[row.key] || row.kind === 'collection') children[row.key] = await expand(row);
    expanded[row.key] = true;
  }

  function isLeaf(row: BrowserRow): boolean {
    return !row.expandable && row.kind !== 'divider';
  }

  // Selecting a row: a leaf writes its text into the prompt and shows its
  // usage; any other row just becomes the selection.
  async function select(row: BrowserRow, insert: boolean): Promise<void> {
    if (row.kind === 'divider') return;
    selectedKey = row.key;
    usage = '';
    if (row.kind === 'method' || row.kind === 'attr') {
      const key = row.key;
      const text = await loadUsage(row.path);
      if (selectedKey === key) usage = text;
    }
    if (insert && isLeaf(row) && row.insert) {
      if (!insertIntoTerminal(row.insert))
        showNotification('Open the Terminal tab to insert a command', 'warning');
    }
  }

  function onRowClick(row: BrowserRow): void {
    void select(row, true);
    if (row.expandable) void toggle(row);
  }

  function onTwistieClick(ev: MouseEvent, row: BrowserRow): void {
    ev.stopPropagation();
    void toggle(row);
  }

  function onChip(id: string): void {
    task = task === id ? null : id;
  }

  // ---- keyboard ------------------------------------------------------------
  function onKey(ev: KeyboardEvent): void {
    const rows = flat.filter((f) => f.row.kind !== 'divider');
    if (!rows.length) return;
    const idx = rows.findIndex((f) => f.row.key === selectedKey);
    const cur = idx >= 0 ? rows[idx].row : undefined;
    if (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') {
      if (!cur?.expandable) return;
      const want = ev.key === 'ArrowRight';
      if (!!expanded[cur.key] !== want) {
        ev.preventDefault();
        void toggle(cur);
      }
      return;
    }
    if (ev.key === 'Enter') {
      if (!cur) return;
      ev.preventDefault();
      if (isLeaf(cur)) void select(cur, true);
      else void toggle(cur);
      return;
    }
    const k = listKeyFromEvent(ev);
    if (!k || k === 'ArrowLeft' || k === 'ArrowRight') return;
    const next = cycleListSelection(rows.length, idx, k, { wrap: false });
    if (next === idx || next < 0) return;
    ev.preventDefault();
    void select(rows[next].row, false);
  }

  function tooltip(row: BrowserRow): string {
    return row.path ? `${row.doc}${row.doc ? '\n' : ''}${row.path}` : row.doc;
  }
</script>

<div class="cmd-browser">
  <div class="chips" role="toolbar" aria-label="Filter by task">
    {#each tasks as chip (chip.id)}
      <button
        class="chip"
        class:active={task === chip.id}
        title={chip.doc}
        aria-label={`${chip.label} tasks`}
        aria-pressed={task === chip.id}
        onclick={() => onChip(chip.id)}>{chip.label}</button
      >
    {/each}
    <button
      class="chip adv"
      class:active={showAdvanced}
      title="Show advanced members"
      aria-pressed={showAdvanced}
      onclick={() => (showAdvanced = !showAdvanced)}>Advanced</button
    >
  </div>

  <ul class="cmd-tree" role="tree" tabindex="0" onkeydown={onKey}>
    {#each flat as { row, depth } (row.key)}
      {#if row.kind === 'divider'}
        <li class="divider" role="presentation">{row.name}</li>
      {:else}
        {@const open = isOpen(row)}
        {@const selected = selectedKey === row.key}
        <li
          class="cmd-row kind-{row.kind}"
          class:selected
          class:dim={!matches(row)}
          role="treeitem"
          aria-selected={selected}
          aria-expanded={row.expandable ? open : undefined}
          style="--depth: {depth}"
        >
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div
            class="cmd-line"
            role="button"
            tabindex="-1"
            title={tooltip(row)}
            onclick={() => onRowClick(row)}
          >
            <span
              class="twistie"
              class:has={row.expandable}
              class:open
              onclick={(e) => onTwistieClick(e, row)}
              role="button"
              tabindex="-1"
              aria-label={row.expandable ? (open ? 'Collapse' : 'Expand') : ''}
            >
              {#if row.expandable}<Icon name="chevron" size={12} />{/if}
            </span>
            <span class="name">{row.name}</span>
            {#if row.kind === 'attr'}
              <span class="type">{typeText(row.type)}{row.readonly ? ' ro' : ''}</span>
            {/if}
            <span class="doc">{row.expandable ? row.doc : firstSentence(row.doc)}</span>
          </div>
          {#if selected && usage}
            <pre class="usage">{usage}</pre>
          {/if}
        </li>
      {/if}
    {/each}
  </ul>
</div>

<style>
  .cmd-browser {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
    container-type: inline-size;
  }
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    padding: 6px 8px;
    border-bottom: 1px solid var(--gs-border, rgba(128, 128, 128, 0.25));
  }
  .chip {
    font-size: 11px;
    padding: 1px 8px;
    border-radius: 10px;
    border: 1px solid var(--gs-border, rgba(128, 128, 128, 0.4));
    background: transparent;
    color: var(--gs-fg-muted);
    cursor: pointer;
  }
  .chip.active {
    background: var(--gs-accent-bg, rgba(80, 140, 220, 0.25));
    color: var(--gs-fg-bright);
    border-color: var(--gs-accent, rgba(80, 140, 220, 0.8));
  }
  .chip.adv {
    margin-left: auto;
  }
  .cmd-tree {
    list-style: none;
    margin: 0;
    padding: 4px 0;
    overflow-y: auto;
    flex: 1 1 auto;
    min-height: 0;
  }
  .cmd-tree:focus {
    outline: none;
  }
  .cmd-tree:focus-visible {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
  .divider {
    font-size: 10px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--gs-fg-muted);
    padding: 6px 10px 2px;
    user-select: none;
  }
  .cmd-row.selected > .cmd-line {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .cmd-row.dim > .cmd-line {
    opacity: 0.45;
  }
  .cmd-line {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 2px 8px 2px calc(8px + var(--depth) * 14px);
    cursor: pointer;
    height: 22px;
    color: var(--gs-fg);
    user-select: none;
    white-space: nowrap;
  }
  .cmd-line:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .twistie {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 14px;
    color: var(--gs-fg-muted);
    flex-shrink: 0;
    transform: rotate(-90deg);
    transition: transform 80ms ease-out;
  }
  .twistie.open {
    transform: rotate(0deg);
  }
  .twistie:not(.has) {
    visibility: hidden;
  }
  .name {
    font-size: 13px;
    flex: 0 0 auto;
    min-width: 14ch;
    font-family: var(--gs-font-mono, monospace);
  }
  .kind-method .name {
    color: var(--gs-syntax-method, #dcdcaa);
  }
  .kind-attr .name {
    color: var(--gs-syntax-attribute, #9cdcfe);
  }
  .kind-alias .name {
    color: var(--gs-syntax-alias, #9cdcfe);
  }
  .kind-keyword .name {
    color: var(--gs-syntax-keyword, #c586c0);
  }
  .kind-group > .cmd-line > .name {
    font-weight: 600;
    font-family: inherit;
  }
  .type {
    font-size: 11px;
    color: var(--gs-syntax-type, #4ec9b0);
    flex: 0 0 auto;
  }
  .doc {
    font-size: 12px;
    color: var(--gs-syntax-dim, var(--gs-fg-muted));
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* A narrow browser (or the vertical split) drops the doc column. */
  @container (max-width: 280px) {
    .doc {
      display: none;
    }
  }
  .usage {
    margin: 2px 12px 6px calc(30px + var(--depth) * 14px);
    padding: 6px 10px;
    font-size: 12px;
    line-height: 1.4;
    white-space: pre-wrap;
    color: var(--gs-fg);
    background: var(--gs-info-bg, rgba(80, 140, 220, 0.08));
    border-left: 2px solid var(--gs-info-border, rgba(80, 140, 220, 0.5));
    font-family: var(--gs-font-mono, monospace);
  }
</style>
