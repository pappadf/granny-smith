<script lang="ts">
  // The Terminal's command browser: a structural view of the object model
  // (levels are path segments, as typed), for composing statements.  Every
  // row comes from the model (lib/commandsTree.ts): a leaf shows its segment,
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
  import {
    aliasGroupKey,
    expand,
    firstSentence,
    loadAliases,
    loadUsageInfo,
    rootRows,
    visible,
    type BrowserRow,
    type UsageInfo,
  } from '@/lib/commandsTree';
  import { typeText } from '@/lib/typeDescriptor';
  import { whenModuleReady } from '@/bus/emulator';
  import { covers, invalidate, onMembersChanged } from '@/bus/memberStore';
  import { focusConsole, registerBrowserReveal, writeToConsole } from './terminalBridge';
  import { pathPrefixes } from '@/lib/objectPath';
  import { terminalSync } from '@/state/terminalSync.svelte';
  import { completionFocus } from '@/lib/pathToken';
  import { utf8ToUtf16 } from '@/lib/utf8';
  import { highlightParts, loadHighlight, type HlSpan } from '@/lib/highlight';
  import Icon from '@/components/common/Icon.svelte';
  import { machine } from '@/state/machine.svelte';

  let roots = $state<BrowserRow[]>([]);
  // Loaded children per row key, and which rows are open.
  let children = $state<Record<string, BrowserRow[]>>({});
  let expanded = $state<Record<string, boolean>>({});
  let selectedKey = $state('');
  let usage = $state<UsageInfo | null>(null);
  // The argument marked in the usage (the console's cursor is in it).
  let markArg = $state<number | null>(null);
  // Following the console: rows matching the partial segment, and the
  // other rows of that level (dimmed).
  let matchKeys = $state<Set<string>>(new Set());
  let otherKeys = $state<Set<string>>(new Set());
  let listEl = $state<HTMLUListElement | null>(null);

  // Rebuild the root when a machine boots or goes (its members change).
  $effect(() => {
    void machine.status;
    untrack(() => void reload());
  });

  async function reload(): Promise<void> {
    await whenModuleReady();
    invalidate('');
    roots = await rootRows();
    // Sections start open, except Aliases and Language; a section the user
    // collapsed stays collapsed across reloads.
    for (const r of roots) if (!(r.key in expanded)) expanded[r.key] = r.key.startsWith('section:');
    // Re-read every level still open.
    const open = Object.keys(expanded).filter((k) => expanded[k]);
    children = {};
    for (const k of open) {
      const row = findRow(k);
      if (row) children[k] = await expand(row);
    }
  }

  // The section holding the root member `name` (Commands or a domain), with
  // its rows loaded; opened when `openIt`.
  async function sectionOf(name: string, openIt: boolean): Promise<BrowserRow | undefined> {
    for (const s of roots) {
      if (!s.key.startsWith('section:')) continue;
      if (!children[s.key]) children[s.key] = await expand(s);
      if (children[s.key].some((r) => r.name === name)) {
        if (openIt) await open(s);
        return s;
      }
    }
    return undefined;
  }

  // Every root member, across the Commands and domain sections.
  async function rootMembers(): Promise<BrowserRow[]> {
    const out: BrowserRow[] = [];
    for (const s of roots) {
      if (!s.key.startsWith('section:')) continue;
      if (!children[s.key]) children[s.key] = await expand(s);
      out.push(...children[s.key]);
    }
    return out;
  }

  // A change in the model re-reads the open levels it dropped.
  const unsubscribe = onMembersChanged((c) => {
    if (c.reload) {
      void reload();
      return;
    }
    for (const k of Object.keys(children)) {
      const row = findRow(k);
      if (row && expanded[k] && c.dropped.some((p) => covers(p, row.path)))
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

  function isOpen(row: BrowserRow): boolean {
    return !!expanded[row.key];
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
        if (!visible(row)) continue;
        out.push({ row, depth });
        // A section's rows sit at its own indent: it is a headline.
        if (row.expandable && isOpen(row) && children[row.key])
          walk(children[row.key], row.kind === 'section' ? depth : depth + 1);
      }
    };
    walk(roots, 0);
    return out;
  });

  async function open(row: BrowserRow): Promise<void> {
    if (!row.expandable || expanded[row.key]) return;
    // A collection is re-read on every expansion: entries come and go.
    if (!children[row.key] || row.kind === 'collection') children[row.key] = await expand(row);
    expanded[row.key] = true;
  }

  async function toggle(row: BrowserRow): Promise<void> {
    if (!row.expandable) return;
    if (expanded[row.key]) expanded[row.key] = false;
    else await open(row);
  }

  function isLeaf(row: BrowserRow): boolean {
    return !row.expandable;
  }

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
  const selectedRow = $derived(selectedKey ? findRow(selectedKey) : undefined);

  function closeDetails(): void {
    detailsOpen = false;
  }

  // Selecting a row: it becomes the selection, and a method or attribute
  // shows its usage in the details pane.  Nothing is written.
  async function select(row: BrowserRow, arg: number | null = null) {
    const changed = selectedKey !== row.key;
    selectedKey = row.key;
    markArg = arg;
    if (row.kind === 'method' || row.kind === 'attr') detailsOpen = true;
    if (!changed && usage) return;
    usage = null;
    usageHl = {};
    if (row.kind === 'method' || row.kind === 'attr') {
      const key = row.key;
      const u = await loadUsageInfo(row.path);
      if (selectedKey !== key) return;
      usage = u;
      if (u) void highlightUsage(u, key);
    }
  }

  // The usage block's code lines -- the signature and the examples --
  // coloured by shell.highlight (line index → spans).
  let usageHl = $state<Record<number, HlSpan[]>>({});
  const EXAMPLE_LEAD = /^(e\.g\. {2}| {6})/;
  const NEWLINE = '\n';

  function codeLines(u: UsageInfo): Array<{ index: number; lead: number; text: string }> {
    const lines = u.text.split('\n');
    const out: Array<{ index: number; lead: number; text: string }> = [];
    if (u.signature && lines[0] === u.signature) out.push({ index: 0, lead: 0, text: lines[0] });
    let inExamples = false;
    for (let i = 1; i < lines.length; i++) {
      const m = EXAMPLE_LEAD.exec(lines[i]);
      if (m && (m[1].startsWith('e.g.') || inExamples)) {
        inExamples = true;
        out.push({ index: i, lead: m[1].length, text: lines[i].slice(m[1].length) });
      } else inExamples = false;
    }
    return out;
  }

  let usageSeq = 0;
  async function highlightUsage(u: UsageInfo, key: string): Promise<void> {
    const seq = ++usageSeq;
    const lines = codeLines(u);
    const spans = await Promise.all(lines.map((l) => loadHighlight(l.text)));
    if (selectedKey !== key || seq !== usageSeq) return;
    const next: Record<number, HlSpan[]> = {};
    lines.forEach((l, k) => {
      next[l.index] = spans[k].map((s) => ({ ...s, from: s.from + l.lead, to: s.to + l.lead }));
    });
    usageHl = next;
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
      void toggle(row);
      void select(row);
      return;
    }
    if (selectedKey === row.key && detailsOpen && !row.expandable) {
      closeDetails();
      return;
    }
    void select(row);
    if (row.expandable) void open(row);
  }

  // A double-click inserts the row into the console.
  function onRowDblClick(row: BrowserRow): void {
    if (row.kind === 'group' || row.kind === 'section') return;
    insert(row);
  }

  function onTwistieClick(ev: MouseEvent, row: BrowserRow): void {
    ev.stopPropagation();
    void toggle(row);
  }

  // --- following the console ------------------------------------------------------

  // Open the levels down to `path` (a node, or a collection / its entry);
  // answers its row when it is shown.
  async function openTo(path: string): Promise<BrowserRow | undefined> {
    let found: BrowserRow | undefined;
    const prefixes = pathPrefixes(path);
    if (prefixes.length) await sectionOf(prefixes[0], true);
    for (const prefix of prefixes) {
      const row = flat.find((f) => f.row.path === prefix && f.row.kind !== 'group')?.row;
      if (!row) return undefined;
      found = row;
      await open(row);
    }
    return found;
  }

  // A row's name as a candidate spells it (`[0]` → `0`, `["scsi"]` → `scsi`).
  function bare(name: string): string {
    return name.replace(/^\["?/, '').replace(/"?\]$/, '').replace(/^\$/, '');
  }

  function mark(level: BrowserRow[], names: string[], partial: string): BrowserRow[] {
    if (!partial) {
      matchKeys = new Set();
      otherKeys = new Set();
      return [];
    }
    const want = new Set(names);
    const hits = level.filter((r) => r.kind !== 'section' && want.has(bare(r.name)));
    matchKeys = new Set(hits.map((r) => r.key));
    otherKeys = new Set(
      level.filter((r) => r.kind !== 'section' && !matchKeys.has(r.key)).map((r) => r.key),
    );
    return hits;
  }

  // Each console update starts a follow; one that a newer update overtook
  // (it awaits the model) stops before it marks or selects anything.
  let followSeq = 0;

  async function follow(): Promise<void> {
    const seq = ++followSeq;
    const stale = () => seq !== followSeq;
    const r = terminalSync.result;
    // An emptied input (a command was run, or the line cleared) has nothing
    // to document: close the pane and drop the marks.
    if (terminalSync.line.trim() === '') {
      detailsOpen = false;
      matchKeys = new Set();
      otherKeys = new Set();
      return;
    }
    if (!r) {
      matchKeys = new Set();
      otherKeys = new Set();
      return;
    }
    const f = completionFocus(
      terminalSync.line,
      r.span,
      r.candidates.map((c) => c.text),
    );

    if (f.alias !== null) {
      await followAlias(f.alias, f.names, stale);
      return;
    }

    // Open the levels of the token; mark the children matching the partial.
    const parentRow = f.parent ? await openTo(f.parent) : undefined;
    if (stale()) return;
    if (f.parent && !parentRow) {
      mark([], [], '');
      return;
    }
    const level = parentRow ? (children[parentRow.key] ?? []) : await rootMembers();
    if (stale()) return;
    const hits = mark(level, f.names, f.partial);

    const method = r.context.method;
    if (method) {
      // In a method's arguments: that method, with the argument marked.
      const row = await openTo(method);
      if (row && !stale()) {
        await select(row, r.context.argIndex);
        scrollToSelected();
      }
      return;
    }
    if (f.partial && hits.length) {
      const exact = hits.find((h) => bare(h.name) === f.partial) ?? hits[0];
      if (!parentRow) await sectionOf(exact.name, true);
      if (stale()) return;
      await select(exact);
      scrollToSelected();
    }
  }

  async function followAlias(name: string, names: string[], stale: () => boolean) {
    const group = roots.find((r) => r.key === 'group:aliases');
    if (!group) return;
    await open(group);
    const all = await loadAliases();
    if (stale()) return;
    const pick =
      all.find((a) => a.name === name) ??
      (names.length === 1 ? all.find((a) => a.name === names[0]) : undefined);
    if (!pick) return;
    const subRow = (children[group.key] ?? []).find((r) => r.key === aliasGroupKey(pick));
    if (!subRow) return;
    await open(subRow);
    const row = (children[subRow.key] ?? []).find((r) => r.key === `alias:${pick.name}`);
    if (row && !stale()) {
      await select(row);
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
    const rows = flat;
    if (!rows.length) return;
    const idx = rows.findIndex((f) => f.row.key === selectedKey);
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
      if (isLeaf(cur)) insert(cur);
      else void toggle(cur);
      return;
    }
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      const next =
        idx < 0
          ? 0
          : ev.key === 'ArrowDown'
            ? Math.min(rows.length - 1, idx + 1)
            : Math.max(0, idx - 1);
      if (next === idx) return;
      void select(rows[next].row);
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
        if (r.kind !== 'section' && bare(r.name).toLowerCase().startsWith(q)) {
          ev.preventDefault();
          void select(r);
          scrollToSelected();
          return;
        }
      }
    }
  }

  // Select the node at `path` (a console object link), opening the levels
  // above it.  Stops at the deepest level that is shown.
  async function reveal(path: string): Promise<void> {
    let found: BrowserRow | undefined;
    const prefixes = pathPrefixes(path);
    if (prefixes.length) await sectionOf(prefixes[0], true);
    for (const prefix of prefixes) {
      const row = flat.find((f) => f.row.path === prefix)?.row;
      if (!row) break;
      found = row;
      if (prefix !== path) await open(row);
    }
    if (!found) return;
    await select(found);
    scrollToSelected();
  }
  registerBrowserReveal((path) => void reveal(path));
  onDestroy(() => registerBrowserReveal(null));

  // The usage text as lines of runs: code lines coloured, the marked
  // argument (the console's cursor is in it) underlined on the signature.
  interface UsageRun {
    text: string;
    cls: string | null;
    mark: boolean;
  }
  // The pane opening (or its text arriving) shrinks the tree; keep the
  // selected row in view above it.
  $effect(() => {
    if (detailsOpen && usage) scrollToSelected();
  });

  const usageLines = $derived.by((): UsageRun[][] | null => {
    if (!usage) return null;
    const span = markArg !== null ? usage.argSpans[markArg] : null;
    const sig = usage.signature;
    const mark =
      span && sig && usage.text.startsWith(sig)
        ? [utf8ToUtf16(sig, span[0]), utf8ToUtf16(sig, span[1])]
        : null;
    return usage.text.split('\n').map((line, i) => {
      const runs: UsageRun[] = highlightParts(line, usageHl[i] ?? []).map((p) => ({
        text: p.text,
        cls: p.cls,
        mark: false,
      }));
      if (i !== 0 || !mark) return runs;
      // Cut the runs at the mark's edges.
      const out: UsageRun[] = [];
      let at = 0;
      for (const r of runs) {
        const a = at;
        const b = at + r.text.length;
        at = b;
        const cuts = [a, Math.max(a, Math.min(b, mark[0])), Math.max(a, Math.min(b, mark[1])), b];
        for (let k = 0; k < 3; k++)
          if (cuts[k + 1] > cuts[k])
            out.push({ text: line.slice(cuts[k], cuts[k + 1]), cls: r.cls, mark: k === 1 });
      }
      return out;
    });
  });

  function tooltip(row: BrowserRow): string {
    return row.path ? `${row.doc}${row.doc ? '\n' : ''}${row.path}` : row.doc;
  }
</script>

<div class="cmd-browser">
  <ul class="cmd-tree" role="tree" tabindex="0" onkeydown={onKey} bind:this={listEl}>
    {#each flat as { row, depth } (row.key)}
      {@const open = isOpen(row)}
      {@const selected = selectedKey === row.key}
      <li
        class="cmd-row kind-{row.kind}"
        class:selected
        class:dim={otherKeys.has(row.key)}
        class:match={matchKeys.has(row.key)}
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
          ondblclick={() => onRowDblClick(row)}
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
          {#if row.kind !== 'section'}
            <span class="doc">{row.expandable ? row.doc : firstSentence(row.doc)}</span>
          {/if}
        </div>
      </li>
    {/each}
  </ul>
  {#if detailsOpen && usageLines && selectedRow}
    <section class="details" aria-label="Usage">
      <header class="details-head">
        <span class="details-name">{selectedRow.name}</span>
        <button class="details-close" aria-label="Close" title="Close (Esc)" onclick={closeDetails}
          >×</button
        >
      </header>
      <pre
        class="usage">{#each usageLines as runs, li (li)}{#if li > 0}{NEWLINE}{/if}{#each runs as r, ri (ri)}{#if r.mark}<mark
                class="usage-arg {r.cls ? `hl-${r.cls}` : ''}">{r.text}</mark
              >{:else if r.cls}<span class="hl-{r.cls}">{r.text}</span
              >{:else}{r.text}{/if}{/each}{/each}</pre>
      {#if selectedRow.insert}
        <footer class="details-foot">
          <button
            class="details-insert"
            title="Insert into the console (double-click, or Enter)"
            onclick={() => selectedRow && insert(selectedRow)}>Insert</button
          >
        </footer>
      {/if}
    </section>
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
    margin: 0;
    padding: 4px 0;
    overflow-y: auto;
    /* Takes what the details pane leaves, but keeps a few rows. */
    flex: 1 1 0;
    min-height: 72px;
  }
  .cmd-tree:focus {
    outline: none;
  }
  .cmd-tree:focus-visible {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
  /* A section headline: a bold row over its rows, which share its indent. */
  .cmd-row.kind-section > .cmd-line {
    padding-top: 6px;
  }
  /* Same size as the rows under it, set apart by weight only. */
  .cmd-row.kind-section .name {
    font-weight: 600;
    font-family: inherit;
    color: var(--gs-fg-bright, var(--gs-fg));
  }
  .cmd-row.selected > .cmd-line {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .cmd-row.dim > .cmd-line {
    opacity: 0.45;
  }
  .cmd-row.match > .cmd-line .name {
    text-decoration: underline;
    text-decoration-color: var(--gs-focus, #0969da);
    text-underline-offset: 3px;
  }
  .usage :global(.hl-keyword) {
    color: var(--gs-syntax-keyword);
  }
  .usage :global(.hl-decl),
  .usage :global(.hl-interp) {
    color: var(--gs-syntax-decl);
  }
  .usage :global(.hl-variable) {
    color: var(--gs-syntax-variable);
  }
  .usage :global(.hl-alias) {
    color: var(--gs-syntax-alias);
  }
  .usage :global(.hl-number) {
    color: var(--gs-syntax-number);
  }
  .usage :global(.hl-string) {
    color: var(--gs-syntax-string);
  }
  .usage :global(.hl-comment) {
    color: var(--gs-syntax-comment);
  }
  .usage :global(.hl-method) {
    color: var(--gs-syntax-method);
  }
  .usage :global(.hl-attribute) {
    color: var(--gs-syntax-attribute);
  }
  .usage :global(.hl-enum) {
    color: var(--gs-syntax-enum);
  }
  .usage :global(.hl-unknown) {
    color: var(--gs-syntax-unknown);
  }
  .usage-arg {
    background: none;
    color: inherit;
    text-decoration: underline;
    text-decoration-thickness: 2px;
  }
  .usage-arg:empty {
    display: none;
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
  /* The details pane: the selection's usage, under the tree. */
  /* It sizes to its content up to 60% of the browser; a longer usage text
     scrolls inside it, with the header and Insert button kept in view. */
  .details {
    flex: 0 0 auto;
    max-height: 60%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-top: 1px solid var(--gs-border, rgba(128, 128, 128, 0.3));
    background: var(--gs-info-bg, rgba(80, 140, 220, 0.08));
  }
  .details-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 2px 4px 0 10px;
  }
  .details-name {
    font-family: var(--gs-font-mono, monospace);
    font-size: 12px;
    color: var(--gs-fg-muted);
  }
  .details-close {
    border: none;
    background: transparent;
    color: var(--gs-fg-muted);
    font-size: 16px;
    line-height: 1;
    padding: 2px 6px;
    cursor: pointer;
  }
  .details-close:hover {
    color: var(--gs-fg);
  }
  .details-foot {
    flex: 0 0 auto;
    display: flex;
    justify-content: flex-end;
    padding: 0 8px 6px;
  }
  .details-insert {
    font-size: 12px;
    padding: 2px 12px;
    border-radius: 3px;
    border: 1px solid var(--gs-accent, rgba(80, 140, 220, 0.8));
    background: var(--gs-accent-bg, rgba(80, 140, 220, 0.25));
    color: var(--gs-fg-bright, var(--gs-fg));
    cursor: pointer;
  }
  .usage {
    flex: 0 1 auto;
    min-height: 0;
    margin: 0;
    padding: 4px 10px 6px;
    overflow: auto;
    font-size: 12px;
    line-height: 1.4;
    white-space: pre-wrap;
    color: var(--gs-fg);
    font-family: var(--gs-font-mono, monospace);
  }
</style>
