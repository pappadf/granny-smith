<script lang="ts">
  // The Terminal's command browser: a structural view of the object model
  // (levels are path segments, as typed), for composing statements.  Every
  // row comes from the model (lib/commandsTree.ts); the tree, the selection
  // and the console-following marks live in state/commandTree.svelte.ts,
  // which this renders.  A leaf shows its segment,
  // the first sentence of its doc and, for an attribute, its type.  Basic
  // and advanced members are both listed, so the browser can follow any
  // path typed in the console.
  //
  // Browsing and writing are separate:
  // - Selecting a row (a click, ↑/↓, type-to-find) only previews it: its
  //   usage text (shell.usage) shows in the details pane under the tree.
  //   The pane closes with its ×, with Esc, or with a second click on the
  //   same row.
  // - Inserting is explicit -- a double-click, Enter on a leaf, or the
  //   pane's Insert button -- and replaces the path token at the console's
  //   cursor with the row's text (`path.`, `path[`, `path["`, `path[i].`,
  //   `path ` for a method, `path` for an attribute, `$name`, `keyword `),
  //   then hands focus to the console.  Esc (with the pane closed) and Tab
  //   hand focus to the console too.
  //
  // It follows the console:
  // - As the user types, the console's shell.complete answer
  //   (state/terminalSync) opens the levels of the path token, marks the
  //   children matching the partial segment and dims the rest; with the
  //   cursor in a method's arguments, that method is selected with the
  //   current argument marked in its usage.
  import { onDestroy, untrack } from 'svelte';
  import { firstSentence, type BrowserRow } from '@/lib/commandsTree';
  import { typeText } from '@/lib/typeDescriptor';
  import { whenModuleReady } from '@/bus/emulator';
  import { focusConsole, registerBrowserReveal, writeToConsole } from './terminalBridge';
  import { terminalSync } from '@/state/terminalSync.svelte';
  import { CommandTree } from '@/state/commandTree.svelte';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';
  import TreeItem from '@/components/ui/TreeItem.svelte';
  import UsagePane from './UsagePane.svelte';
  import { machine } from '@/state/machine.svelte';

  const ct = new CommandTree();
  const tree = ct.tree;
  onDestroy(() => ct.dispose());

  // The argument marked in the usage (the console's cursor is in it).
  let markArg = $state<number | null>(null);
  let listEl = $state<HTMLUListElement | null>(null);

  // Rebuild the root when a machine boots or goes (its members change).
  $effect(() => {
    void machine.status;
    untrack(() => void whenModuleReady().then(() => ct.reload()));
  });

  // --- writing to the console ------------------------------------------------

  // Insert a row into the console (explicit: double-click, Enter, Insert)
  // and hand focus over, so typing carries on where it was written.
  function insert(row: BrowserRow): void {
    if (!row.insert) return;
    writeToConsole(row.insert);
    focusConsole();
  }

  // Whether the details pane shows the selection's usage.
  let detailsOpen = $state(false);
  const selectedRow = $derived(ct.selected);

  function closeDetails(): void {
    detailsOpen = false;
  }

  // Selecting a row: it becomes the selection, and a method or attribute
  // shows its usage in the details pane.  Nothing is written.
  function select(row: BrowserRow, arg: number | null = null): void {
    ct.select(row);
    markArg = arg;
    if (row.kind === 'method' || row.kind === 'attr') detailsOpen = true;
  }

  function scrollToSelected(): void {
    requestAnimationFrame(() =>
      listEl?.querySelector('.cmd-row.selected')?.scrollIntoView?.({ block: 'nearest' }),
    );
  }

  // A click on a row previews it (a second click on the same leaf closes
  // the pane); a group, section or node also opens or closes.
  function onRowClick(row: BrowserRow): void {
    if (row.kind === 'group' || row.kind === 'section') {
      void tree.toggle(row);
      select(row);
      return;
    }
    if (ct.selectedKey === row.key && detailsOpen && !row.expandable) {
      closeDetails();
      return;
    }
    select(row);
    if (row.expandable) void tree.open(row);
  }

  function onTwistieClick(ev: MouseEvent, row: BrowserRow): void {
    ev.stopPropagation();
    void tree.toggle(row);
  }

  // --- following the console ------------------------------------------------------

  async function follow(): Promise<void> {
    const a = await ct.follow(terminalSync.line, terminalSync.result);
    if (a?.kind === 'close') closeDetails();
    else if (a?.kind === 'select') {
      select(a.row, a.arg);
      scrollToSelected();
    }
  }

  $effect(() => {
    void terminalSync.seq;
    untrack(() => void follow());
  });

  // --- keyboard -------------------------------------------------------------------------
  let findBuf = '';
  let findAt = 0;

  function onKey(ev: KeyboardEvent): void {
    const rows = tree.flat;
    if (!rows.length) return;
    const idx = rows.findIndex((f) => f.row.key === ct.selectedKey);
    const cur = idx >= 0 ? rows[idx].row : undefined;
    if (ev.key === 'Escape') {
      ev.preventDefault();
      // First Esc closes the details pane; the next one leaves the browser.
      if (detailsOpen) {
        closeDetails();
        return;
      }
      focusConsole();
      return;
    }
    if (ev.key === 'Tab' && !ev.shiftKey) {
      ev.preventDefault();
      focusConsole();
      return;
    }
    if (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') {
      if (!cur?.expandable) return;
      if (tree.isOpen(cur) !== (ev.key === 'ArrowRight')) {
        ev.preventDefault();
        void tree.toggle(cur);
      }
      return;
    }
    if (ev.key === 'Enter') {
      if (!cur) return;
      ev.preventDefault();
      if (cur.expandable) void tree.toggle(cur);
      else insert(cur);
      return;
    }
    const nav = listKeyFromEvent(ev);
    if (nav) {
      ev.preventDefault();
      const next = cycleListSelection(rows.length, idx, nav);
      if (next === idx) return;
      select(rows[next].row);
      scrollToSelected();
      return;
    }
    // Type-to-find: the next row whose name starts with what was typed.
    if (ev.key.length === 1 && !ev.ctrlKey && !ev.metaKey && !ev.altKey && ev.key !== ' ') {
      const now = Date.now();
      findBuf = now - findAt > 700 ? ev.key : findBuf + ev.key;
      findAt = now;
      const q = findBuf.toLowerCase();
      const n = rows.length;
      const start = findBuf.length > 1 ? Math.max(idx, 0) : idx + 1;
      for (let k = 0; k < n; k++) {
        const r = rows[(start + k) % n].row;
        if (r.word && r.word.toLowerCase().startsWith(q)) {
          ev.preventDefault();
          select(r);
          scrollToSelected();
          return;
        }
      }
    }
  }

  // Select the node at `path` (a console object link), opening the levels
  // above it.  Stops at the deepest level that is shown.
  async function reveal(path: string): Promise<void> {
    const found = await ct.openPath(path, { openLast: false });
    if (!found) return;
    select(found);
    scrollToSelected();
  }
  registerBrowserReveal((path) => void reveal(path));
  onDestroy(() => registerBrowserReveal(null));

  function tooltip(row: BrowserRow): string {
    return row.path ? `${row.doc}${row.doc ? '\n' : ''}${row.path}` : row.doc;
  }
</script>

<div class="cmd-browser">
  <ul class="cmd-tree" role="tree" tabindex="0" onkeydown={onKey} bind:this={listEl}>
    {#each tree.flat as { row, depth } (row.key)}
      {@const open = tree.isOpen(row)}
      {@const selected = ct.selectedKey === row.key}
      <li
        class="cmd-row kind-{row.kind}"
        class:selected
        role="treeitem"
        aria-selected={selected}
        aria-expanded={row.expandable ? open : undefined}
        aria-level={depth + 1}
      >
        <TreeItem
          class="cmd-line"
          role="button"
          tabindex={-1}
          title={tooltip(row)}
          {depth}
          kind={row.kind}
          variant={row.kind === 'section' ? 'category' : 'default'}
          filter={ct.matchKeys.has(row.key)
            ? 'match'
            : ct.otherKeys.has(row.key)
              ? 'dim'
              : undefined}
          hasChildren={row.expandable}
          {open}
          {selected}
          onclick={() => onRowClick(row)}
          ondblclick={() => insert(row)}
          onDisclosureClick={(e) => onTwistieClick(e, row)}
        >
          {#snippet content()}
            <span class="gs-tree-item__label name">{row.name}</span>
            {#if row.kind === 'attr'}
              <span class="type">{typeText(row.type)}{row.readonly ? ' ro' : ''}</span>
            {/if}
            {#if row.kind !== 'section'}
              <span class="doc">{row.expandable ? row.doc : firstSentence(row.doc)}</span>
            {/if}
          {/snippet}
        </TreeItem>
      </li>
    {/each}
  </ul>
  {#if detailsOpen && selectedRow}
    <UsagePane
      row={selectedRow}
      {markArg}
      onClose={closeDetails}
      onInsert={insert}
      onShown={scrollToSelected}
    />
  {/if}
</div>

<style>
  .cmd-browser {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
    container-type: inline-size;
  }
  .cmd-tree {
    list-style: none;
    user-select: none;
    white-space: nowrap;
    margin: 0;
    padding: var(--gs-space-1) 0;
    overflow-y: auto;
    /* Takes what the details pane leaves, but keeps a few rows. */
    flex: 1 1 0;
    min-height: 72px;
  }
  .cmd-tree:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  /* Same size as the rows under it, set apart by weight only. */
  .kind-section .name,
  .kind-group .name {
    font-family: inherit;
  }
  .kind-group .name {
    font-weight: var(--gs-font-weight-semibold);
  }
  .name {
    font-size: var(--gs-font-size-base);
    flex: 0 0 auto;
    min-width: 14ch;
    font-family: var(--gs-font-mono);
  }
  .kind-method .name {
    color: var(--gs-syntax-method);
  }
  .kind-attr .name {
    color: var(--gs-syntax-attribute);
  }
  .kind-alias .name {
    color: var(--gs-syntax-alias);
  }
  .kind-keyword .name {
    color: var(--gs-syntax-keyword);
  }
  .type {
    font-size: var(--gs-font-size-xs);
    color: var(--gs-syntax-type);
    flex: 0 0 auto;
  }
  .doc {
    font-size: var(--gs-font-size-sm);
    color: var(--gs-syntax-dim);
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
</style>
