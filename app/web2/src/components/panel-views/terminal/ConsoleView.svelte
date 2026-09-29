<script lang="ts" module>
  import type { ConsoleEntry } from '@/lib/consoleModel';

  // A value entry's shape, from its tagged JSON: an object (a link to its
  // node), a list or map (expandable), or a scalar (its text).
  export type ValueShape =
    | { kind: 'object'; path: string }
    | { kind: 'list'; items: unknown[] }
    | { kind: 'map'; items: [string, unknown][] }
    | { kind: 'scalar' };

  function isTagged(o: Record<string, unknown>): boolean {
    const keys = Object.keys(o);
    return (
      ('enum' in o && 'index' in o && keys.length === 2) ||
      ('error' in o && keys.length === 1) ||
      ('object' in o && 'path' in o)
    );
  }

  export function valueShape(json: unknown): ValueShape {
    if (Array.isArray(json))
      return json.length ? { kind: 'list', items: json } : { kind: 'scalar' };
    if (json && typeof json === 'object') {
      const o = json as Record<string, unknown>;
      if ('object' in o && typeof o.path === 'string' && o.path)
        return { kind: 'object', path: o.path };
      if (isTagged(o)) return { kind: 'scalar' };
      const items = Object.entries(o);
      return items.length ? { kind: 'map', items } : { kind: 'scalar' };
    }
    return { kind: 'scalar' };
  }

  // One nested value as text: strings quoted, enums by label, objects by
  // path, containers by size.
  export function nestedText(v: unknown): string {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'string') return JSON.stringify(v);
    if (Array.isArray(v)) return `[${v.length}]`;
    if (typeof v === 'object') {
      const o = v as Record<string, unknown>;
      if ('enum' in o && 'index' in o) return String(o.enum ?? o.index);
      if ('object' in o && 'path' in o) return String(o.path || `<${String(o.object)}>`);
      if ('error' in o) return `<error: ${String(o.error)}>`;
      return `{${Object.keys(o).length}}`;
    }
    return String(v);
  }

  export function entryById(
    entries: readonly ConsoleEntry[],
    id: number,
  ): ConsoleEntry | undefined {
    // Entries are in id order: binary search.
    let lo = 0;
    let hi = entries.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const e = entries[mid];
      if (e.id === id) return e;
      if (e.id < id) lo = mid + 1;
      else hi = mid - 1;
    }
    return undefined;
  }
</script>

<script lang="ts">
  // The Terminal's console: DOM-rendered output entries and a
  // CodeMirror 6 input.  The entries come from state/console.svelte.ts (fed
  // by the core's records); the input's keys are ConsoleInput.ts's.  Here:
  // rendering, auto-scroll, the progress line, copy / context menu, find.
  import { onMount, onDestroy, tick } from 'svelte';
  import { seedPrompt, tabComplete, needsContinuation, whenModuleReady } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import {
    consoleState,
    consoleModel,
    consoleSubmit,
    consoleInterrupt,
    consoleClear,
    refreshPrompt,
  } from '@/state/console.svelte';
  import { commandsText, jobOutputText } from '@/lib/consoleModel';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { ConsoleHistory, copyText } from '@/lib/consoleHistory';
  import type { ConsoleInput } from './ConsoleInput';
  import { registerTerminalInsert, revealInBrowser } from './terminalBridge';
  import { revealInSystem } from '@/state/system.svelte';
  import { normalisePaste } from '@/lib/consoleModel';

  let outputEl = $state<HTMLDivElement | null>(null);
  let inputHost = $state<HTMLDivElement | null>(null);
  let input: ConsoleInput | null = null;
  let destroyed = false;
  let startupError = $state('');

  // The model starts listening now: records replay into it.
  consoleModel();

  // --- auto-scroll ----------------------------------------------------------
  // Only while the view is at the bottom; appending never re-renders what is
  // already there (entries are keyed by id), so selections survive.
  let stick = true;
  function onScroll(): void {
    if (!outputEl) return;
    stick = outputEl.scrollHeight - outputEl.scrollTop - outputEl.clientHeight < 8;
  }
  $effect(() => {
    void consoleState.entries;
    void progressText;
    if (!stick || !outputEl) return;
    void tick().then(() => {
      if (outputEl) outputEl.scrollTop = outputEl.scrollHeight;
    });
  });

  // --- progress ---------------------------------------------------------------
  let now = $state(performance.now());
  let timer: ReturnType<typeof setInterval> | null = null;
  $effect(() => {
    const since = consoleState.runningSince;
    if (since === null) {
      if (timer) clearInterval(timer);
      timer = null;
      return;
    }
    now = performance.now();
    timer ??= setInterval(() => (now = performance.now()), 250);
  });
  const progressText = $derived.by(() => {
    const since = consoleState.runningSince;
    if (since === null) return '';
    const s = Math.floor((now - since) / 1000);
    return s >= 1 ? `running… ${s} s` : '';
  });

  // --- prompt -------------------------------------------------------------------
  // The prompt is state-aware: reseed it on machine transitions (boot,
  // toolbar pause/resume); a running command's own result carries it too.
  $effect(() => {
    void machine.status;
    void machine.model;
    void (async () => {
      await seedPrompt();
      if (!destroyed && consoleState.runningSince === null) refreshPrompt();
    })();
  });

  // --- find -----------------------------------------------------------------------
  let findOpen = $state(false);
  let findQuery = $state('');
  let findCase = $state(false);
  let findIndex = $state(0);
  let findEl = $state<HTMLInputElement | null>(null);

  function hit(text: string): boolean {
    if (!findQuery) return false;
    return findCase
      ? text.includes(findQuery)
      : text.toLowerCase().includes(findQuery.toLowerCase());
  }
  const findHits = $derived(
    findOpen && findQuery ? consoleState.entries.filter((e) => hit(e.text)).map((e) => e.id) : [],
  );
  const hitSet = $derived(new Set(findHits));
  const currentHit = $derived(findHits.length ? findHits[findIndex % findHits.length] : -1);

  function openFind(): void {
    findOpen = true;
    void tick().then(() => findEl?.select());
  }
  function closeFind(): void {
    findOpen = false;
    input?.focus();
  }
  function step(delta: number): void {
    if (!findHits.length) return;
    findIndex = (findIndex + delta + findHits.length) % findHits.length;
    void tick().then(() =>
      outputEl?.querySelector('.entry.find-current')?.scrollIntoView({ block: 'nearest' }),
    );
  }
  function onFindKey(ev: KeyboardEvent): void {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      step(ev.shiftKey ? -1 : 1);
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      closeFind();
    }
  }
  $effect(() => {
    void findQuery;
    void findCase;
    findIndex = 0;
  });

  // Text split around find matches, for <mark>.
  function parts(text: string): { t: string; m: boolean }[] {
    if (!findOpen || !findQuery) return [{ t: text, m: false }];
    const hay = findCase ? text : text.toLowerCase();
    const needle = findCase ? findQuery : findQuery.toLowerCase();
    const out: { t: string; m: boolean }[] = [];
    let i = 0;
    for (;;) {
      const j = hay.indexOf(needle, i);
      if (j < 0) break;
      if (j > i) out.push({ t: text.slice(i, j), m: false });
      out.push({ t: text.slice(j, j + needle.length), m: true });
      i = j + needle.length;
    }
    if (i < text.length) out.push({ t: text.slice(i), m: false });
    return out;
  }

  // --- selection and copy -----------------------------------------------------
  function outputSelection(): string {
    const sel = window.getSelection();
    if (!sel || sel.isCollapsed || !outputEl) return '';
    if (!sel.anchorNode || !outputEl.contains(sel.anchorNode)) return '';
    return sel.toString();
  }

  // Entries the selection touches, in order.
  function selectedEntries(): ConsoleEntry[] {
    const sel = window.getSelection();
    if (!sel || sel.isCollapsed || !outputEl) return [];
    const out: ConsoleEntry[] = [];
    for (const el of outputEl.querySelectorAll<HTMLElement>('.entry')) {
      if (!sel.containsNode(el, true)) continue;
      const e = entryById(consoleState.entries, Number(el.dataset.id));
      if (e) out.push(e);
    }
    return out;
  }

  function onContextMenu(ev: MouseEvent): void {
    ev.preventDefault();
    const el = (ev.target as HTMLElement).closest<HTMLElement>('.entry');
    const clicked = el ? entryById(consoleState.entries, Number(el.dataset.id)) : undefined;
    const selText = outputSelection();
    const touched = selectedEntries();
    const items: ContextMenuItem[] = [
      { label: 'Copy', action: () => void copyText(selText || clicked?.text || '') },
      {
        label: 'Copy as commands',
        action: () =>
          void copyText(commandsText(touched.length ? touched : clicked ? [clicked] : [])),
      },
    ];
    if (clicked && clicked.job !== null)
      items.push({
        label: 'Copy output',
        action: () => void copyText(jobOutputText(consoleState.entries, clicked.job)),
      });
    if (clicked?.kind === 'value' && clicked.json !== undefined)
      items.push({
        label: 'Copy value as JSON',
        action: () => void copyText(JSON.stringify(clicked.json, null, 2)),
      });
    items.push(
      { sep: true },
      { label: 'Paste', action: () => void pasteFromClipboard() },
      {
        label: 'Select all',
        action: () => {
          if (!outputEl) return;
          const r = document.createRange();
          r.selectNodeContents(outputEl);
          const sel = window.getSelection();
          sel?.removeAllRanges();
          sel?.addRange(r);
        },
      },
      { label: 'Clear', action: () => consoleClear() },
    );
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function pasteFromClipboard(): Promise<void> {
    try {
      const text = normalisePaste(await navigator.clipboard.readText());
      if (!input) return;
      input.view.dispatch(input.view.state.replaceSelection(text));
      input.focus();
    } catch {
      // Clipboard read refused.
    }
  }

  // Copy: the text as displayed.  Entries render their text verbatim and
  // the prompt glyph is CSS (not in the selection), so the native copy is
  // right; nothing to override.

  // A click that selects nothing focuses the input.
  function onOutputMouseUp(ev: MouseEvent): void {
    if (ev.button !== 0) return;
    if ((ev.target as HTMLElement).closest('a, summary, button, input')) return;
    if (!outputSelection()) input?.focus();
  }

  // Click: select the node in the command browser; Ctrl/Cmd-click: reveal
  // it in SYSTEM.
  function onObjectLink(ev: MouseEvent, path: string): void {
    ev.preventDefault();
    if (ev.ctrlKey || ev.metaKey) revealInSystem(path);
    else revealInBrowser(path);
  }

  function onRootKey(ev: KeyboardEvent): void {
    if ((ev.ctrlKey || ev.metaKey) && !ev.altKey && ev.key.toLowerCase() === 'f') {
      ev.preventDefault();
      openFind();
    }
  }

  // --- input ------------------------------------------------------------------------
  const history = new ConsoleHistory();

  onMount(() => {
    void (async () => {
      // CodeMirror is code-split: it loads when the console first mounts.
      const { createConsoleInput } = await import('./ConsoleInput');
      if (destroyed || !inputHost) return;
      input = createConsoleInput(inputHost, {
        submit: (text) => consoleSubmit(text),
        needsContinuation: (text) => needsContinuation(text),
        complete: (line, cursor) => tabComplete(line, cursor),
        interrupt: () => {
          input?.setText('');
          void consoleInterrupt();
        },
        clear: () => consoleClear(),
        find: () => openFind(),
        outputSelection,
        history,
      });
      registerTerminalInsert((text) => input?.replaceAll(`${text} `));
    })();
    void (async () => {
      try {
        await whenModuleReady();
      } catch (e) {
        if (!destroyed)
          startupError = `emulator did not start: ${e instanceof Error ? e.message : e}`;
        return;
      }
      if (destroyed) return;
      await seedPrompt();
      refreshPrompt();
    })();
  });

  onDestroy(() => {
    destroyed = true;
    if (timer) clearInterval(timer);
    registerTerminalInsert(null);
    input?.destroy();
    input = null;
  });
</script>

{#snippet textWithMarks(text: string)}
  {#each parts(text) as p, i (i)}{#if p.m}<mark>{p.t}</mark>{:else}{p.t}{/if}{/each}
{/snippet}

{#snippet nested(v: unknown)}
  {@const shape = valueShape(v)}
  {#if shape.kind === 'list' || shape.kind === 'map'}
    <details class="nested">
      <summary>{nestedText(v)}</summary>
      <div class="kv">
        {#each shape.kind === 'list' ? shape.items.map((x, i) => [String(i), x] as [string, unknown]) : shape.items as [k, x], i (i)}
          <div class="kv-row"><span class="kv-key">{k}</span>{@render nested(x)}</div>
        {/each}
      </div>
    </details>
  {:else if shape.kind === 'object'}
    <a href="#{shape.path}" class="obj-link" onclick={(e) => onObjectLink(e, shape.path)}
      >{shape.path}</a
    >
  {:else}
    <span class="kv-val">{nestedText(v)}</span>
  {/if}
{/snippet}

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
  class="console"
  class:busy={consoleState.runningSince !== null}
  role="region"
  aria-label="Emulator shell console"
  onkeydown={onRootKey}
>
  {#if findOpen}
    <div class="find" role="search">
      <input
        bind:this={findEl}
        bind:value={findQuery}
        class="find-input"
        placeholder="Find"
        aria-label="Find in console output"
        onkeydown={onFindKey}
      />
      <span class="find-count"
        >{findHits.length
          ? `${(findIndex % findHits.length) + 1} of ${findHits.length}`
          : 'No results'}</span
      >
      <button
        class="find-btn"
        class:on={findCase}
        title="Match case"
        aria-pressed={findCase}
        onclick={() => (findCase = !findCase)}>Aa</button
      >
      <button
        class="find-btn"
        title="Previous match"
        aria-label="Previous match"
        onclick={() => step(-1)}>↑</button
      >
      <button class="find-btn" title="Next match" aria-label="Next match" onclick={() => step(1)}
        >↓</button
      >
      <button class="find-btn" title="Close" aria-label="Close find" onclick={closeFind}>×</button>
    </div>
  {/if}

  <div
    class="console-output"
    bind:this={outputEl}
    onscroll={onScroll}
    oncontextmenu={onContextMenu}
    onmouseup={onOutputMouseUp}
    role="log"
    aria-live="polite"
  >
    {#each consoleState.entries as e (e.id)}
      <div
        class="entry {e.kind}"
        class:find-hit={hitSet.has(e.id)}
        class:find-current={currentHit === e.id}
        data-id={e.id}
      >
        {#if e.kind === 'value' && e.json !== undefined}
          {@const shape = valueShape(e.json)}
          {#if shape.kind === 'object'}
            <a href="#{shape.path}" class="obj-link" onclick={(ev) => onObjectLink(ev, shape.path)}
              >{@render textWithMarks(e.text)}</a
            >
          {:else if shape.kind === 'list' || shape.kind === 'map'}
            <details class="value-tree">
              <summary>{@render textWithMarks(e.text)}</summary>
              <div class="kv">
                {#each shape.kind === 'list' ? shape.items.map((x, i) => [String(i), x] as [string, unknown]) : shape.items as [k, x], i (i)}
                  <div class="kv-row"><span class="kv-key">{k}</span>{@render nested(x)}</div>
                {/each}
              </div>
            </details>
          {:else}
            {@render textWithMarks(e.text)}
          {/if}
        {:else}
          {@render textWithMarks(e.text)}
        {/if}
      </div>
    {/each}
    {#if progressText}
      <div class="entry progress">{progressText}</div>
    {/if}
    {#if startupError}
      <div class="entry error">{startupError}</div>
    {/if}
  </div>

  <div class="console-input-row">
    <span class="console-prompt">{consoleState.prompt}</span>
    <div class="console-input" bind:this={inputHost}></div>
  </div>
</div>

<style>
  .console {
    position: relative;
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
    background: var(--gs-terminal-bg);
    color: var(--gs-terminal-fg);
    font-family: var(--gs-font-mono);
    font-size: 13px;
    line-height: 1.4;
    box-sizing: border-box;
  }
  .console-output {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    padding: 4px 6px 0;
  }
  .console-output ::selection {
    background: var(--gs-terminal-selection);
  }
  .entry {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    /* Off-screen entries skip layout and paint: 5 000 of them stay cheap. */
    content-visibility: auto;
    contain-intrinsic-size: auto 1.4em;
  }
  .entry.command::before {
    content: '› ';
    color: var(--gs-syntax-dim);
  }
  .entry.stderr {
    color: var(--gs-syntax-error);
    opacity: 0.75;
  }
  .entry.error {
    color: var(--gs-syntax-error);
  }
  .entry.echo,
  .entry.progress {
    color: var(--gs-syntax-dim);
  }
  .entry.find-hit {
    background: rgba(128, 128, 128, 0.12);
  }
  .entry mark {
    background: rgba(234, 92, 0, 0.33);
    color: inherit;
  }
  .entry.find-current mark {
    background: rgba(234, 92, 0, 0.7);
  }
  .obj-link {
    color: inherit;
    text-decoration: underline dotted;
    cursor: pointer;
  }
  .obj-link:hover {
    text-decoration: underline;
  }
  details > summary {
    cursor: pointer;
    list-style: none;
  }
  details > summary::before {
    content: '▸ ';
    color: var(--gs-syntax-dim);
  }
  details[open] > summary::before {
    content: '▾ ';
  }
  .kv {
    padding-left: 1.5em;
  }
  .kv-row {
    display: flex;
    gap: 0.75em;
  }
  .kv-key {
    color: var(--gs-syntax-attribute);
    flex: none;
  }
  .kv-key::after {
    content: ':';
    color: var(--gs-syntax-dim);
  }
  .console-input-row {
    display: flex;
    align-items: flex-start;
    gap: 0.5em;
    padding: 2px 6px 4px;
    flex: none;
    max-height: 40%;
    overflow-y: auto;
  }
  .console-prompt {
    flex: none;
    white-space: pre;
    color: var(--gs-syntax-dim);
  }
  .console-prompt:empty::before {
    content: '›';
  }
  .console-input {
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
  }
  .find {
    position: absolute;
    top: 4px;
    right: 12px;
    z-index: 2;
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 3px 4px;
    background: var(--gs-menu-bg);
    color: var(--gs-menu-fg);
    border: 1px solid var(--gs-border, #454545);
    font-family: var(--gs-font-ui);
    font-size: 12px;
  }
  .find-input {
    width: 14em;
    font: inherit;
    background: var(--gs-bg);
    color: var(--gs-fg);
    border: 1px solid var(--gs-border, #454545);
    padding: 1px 4px;
  }
  .find-count {
    min-width: 5.5em;
    color: var(--gs-syntax-dim);
  }
  .find-btn {
    background: none;
    border: 1px solid transparent;
    color: inherit;
    cursor: pointer;
    padding: 0 4px;
    font: inherit;
  }
  .find-btn.on {
    border-color: var(--gs-focus-border, #007fd4);
  }
</style>
