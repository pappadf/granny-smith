<script lang="ts">
  // The Terminal's console: DOM-rendered output entries and a
  // CodeMirror 6 input.  The entries come from state/console.svelte.ts (fed
  // by the core's records); the input's keys are ConsoleInput.ts's; what the
  // input asks the shell while typing is inputAssist's; auto-scroll is
  // lib/stickToBottom; values render as ValueTree.  Here: the entries, the
  // progress line, the prompt, copy / paste / context menu.
  import { onMount, onDestroy, tick } from 'svelte';
  import { seedPrompt, needsContinuation, whenModuleReady } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import { appConsole, type Console } from '@/state/console.svelte';
  import {
    commandsText,
    entryById,
    jobOutputText,
    normalisePaste,
    type ConsoleEntry,
  } from '@/lib/consoleModel';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { ConsoleHistory } from '@/lib/consoleHistory';
  import { copyText } from '@/lib/clipboard';
  import { highlightParts } from '@/lib/highlight';
  import { stickToBottom } from '@/lib/stickToBottom.svelte';
  import { revealInSystem } from '@/state/system.svelte';
  import type { ConsoleInput } from './ConsoleInput';
  import { registerConsoleInput, revealInBrowser } from './terminalBridge';
  import { InputAssist } from './inputAssist.svelte';
  import FindBar, { FindState } from './FindBar.svelte';
  import ValueTree from './ValueTree.svelte';

  // The console to show: the app's, or a test's own.
  let { console: con = appConsole }: { console?: Console } = $props();
  const consoleState = $derived(con.state);

  let outputEl = $state<HTMLDivElement | null>(null);
  let inputHost = $state<HTMLDivElement | null>(null);
  let input: ConsoleInput | null = null;
  let destroyed = false;
  let startupError = $state('');

  const assist = new InputAssist(() => input);
  const find = new FindState(() => consoleState.entries);
  let findBar = $state<FindBar | null>(null);

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

  // Appending never re-renders what is already there (entries are keyed by
  // id), so selections survive the auto-scroll.
  const scroll = stickToBottom(() => [consoleState.entries, progressText]);

  // --- prompt -------------------------------------------------------------------
  // The prompt is state-aware: reseed it on machine transitions (boot,
  // toolbar pause/resume); a running command's own result carries it too.
  // What the shell can complete changes with them.
  $effect(() => {
    void machine.status;
    void machine.model;
    assist.forget();
    void (async () => {
      await seedPrompt();
      if (!destroyed && consoleState.runningSince === null) con.refreshPrompt();
    })();
  });

  // --- find -----------------------------------------------------------------------
  function openFind(): void {
    find.open = true;
    void tick().then(() => findBar?.focus());
  }
  function closeFind(): void {
    find.open = false;
    input?.focus();
  }
  function revealCurrent(): void {
    void tick().then(() =>
      outputEl?.querySelector('.entry.find-current')?.scrollIntoView({ block: 'nearest' }),
    );
  }

  // --- selection, copy, paste ---------------------------------------------------
  function outputSelection(): string {
    const sel = window.getSelection();
    if (!sel || sel.isCollapsed || !outputEl) return '';
    if (!sel.anchorNode || !outputEl.contains(sel.anchorNode)) return '';
    return sel.toString();
  }

  // The entry an element of the output belongs to.
  function entryOf(el: Element | null): ConsoleEntry | undefined {
    const e = el?.closest<HTMLElement>('.entry');
    return e ? entryById(consoleState.entries, Number(e.dataset.id)) : undefined;
  }

  // Entries the selection touches, in order.
  function selectedEntries(): ConsoleEntry[] {
    const sel = window.getSelection();
    if (!sel || sel.isCollapsed || !outputEl) return [];
    return Array.from(outputEl.querySelectorAll('.entry'))
      .filter((el) => sel.containsNode(el, true))
      .map(entryOf)
      .filter((e): e is ConsoleEntry => !!e);
  }

  function selectAll(): void {
    if (!outputEl) return;
    const r = document.createRange();
    r.selectNodeContents(outputEl);
    const sel = window.getSelection();
    sel?.removeAllRanges();
    sel?.addRange(r);
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

  function onContextMenu(ev: MouseEvent): void {
    ev.preventDefault();
    const clicked = entryOf(ev.target as Element);
    const selText = outputSelection();
    const touched = selectedEntries();
    const copy = (text: string) => () => void copyText(text);
    const items: ContextMenuItem[] = [
      { label: 'Copy', action: copy(selText || clicked?.text || '') },
      {
        label: 'Copy as commands',
        action: copy(commandsText(touched.length ? touched : clicked ? [clicked] : [])),
      },
    ];
    if (clicked && clicked.job !== null)
      items.push({
        label: 'Copy output',
        action: copy(jobOutputText(consoleState.entries, clicked.job)),
      });
    if (clicked?.kind === 'value' && clicked.json !== undefined)
      items.push({
        label: 'Copy value as JSON',
        action: copy(JSON.stringify(clicked.json, null, 2)),
      });
    items.push(
      { sep: true },
      { label: 'Paste', action: () => void pasteFromClipboard() },
      { label: 'Select all', action: selectAll },
      { label: 'Clear', action: () => con.model.clear() },
    );
    openContextMenu(items, ev.clientX, ev.clientY);
  }

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
        submit: (text) => {
          // Submitting always brings the view back to the bottom.
          scroll.follow();
          con.submit(text, assist.spansFor(text));
        },
        needsContinuation: (text) => needsContinuation(text),
        complete: (line, cursor) => assist.complete(line, cursor),
        interrupt: () => {
          input?.setText('');
          void con.interrupt();
        },
        clear: () => con.model.clear(),
        find: openFind,
        outputSelection,
        history,
        onChange: (text, cursor) => assist.onChange(text, cursor),
        showHint: () => assist.showHint(),
        escape: () => assist.escapeHint(),
      });
      registerConsoleInput(input);
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
      con.refreshPrompt();
    })();
  });

  onDestroy(() => {
    destroyed = true;
    if (timer) clearInterval(timer);
    registerConsoleInput(null);
    assist.dispose();
    input?.destroy();
    input = null;
  });
</script>

{#snippet marked(text: string)}
  {#each find.parts(text) as p, i (i)}{#if p.m}<mark>{p.t}</mark>{:else}{p.t}{/if}{/each}
{/snippet}

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
  class="console"
  class:busy={consoleState.runningSince !== null}
  role="region"
  aria-label="Emulator shell console"
  onkeydown={onRootKey}
>
  {#if find.open}
    <FindBar bind:this={findBar} {find} onstep={revealCurrent} onclose={closeFind} />
  {/if}

  <div
    class="console-output"
    bind:this={outputEl}
    {@attach scroll.attach}
    oncontextmenu={onContextMenu}
    onmouseup={onOutputMouseUp}
    role="log"
    aria-live="polite"
  >
    {#each consoleState.entries as e (e.id)}
      <div
        class="entry {e.kind}"
        class:find-hit={find.hitSet.has(e.id)}
        class:find-current={find.current === e.id}
        data-id={e.id}
      >
        {#if e.kind === 'value' && e.json !== undefined}
          <ValueTree value={e.json} onlink={onObjectLink}>
            {#snippet label()}{@render marked(e.text)}{/snippet}
          </ValueTree>
        {:else if e.kind === 'command' && e.spans && !find.active}
          {#each highlightParts(e.text, e.spans) as p, i (i)}{#if p.cls}<span class="hl-{p.cls}"
                >{p.text}</span
              >{:else}{p.text}{/if}{/each}
        {:else}
          {@render marked(e.text)}
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

  {#if assist.hint}
    <div class="sig-hint" role="tooltip">
      {assist.hint.before}<u class="sig-arg">{assist.hint.arg}</u>{assist.hint.after}
    </div>
  {/if}
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
    background: var(--gs-console-bg);
    color: var(--gs-console-fg);
    font-family: var(--gs-console-font);
    font-size: var(--gs-console-font-size);
    line-height: var(--gs-console-line-height);
    box-sizing: border-box;
  }
  .console-output {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    padding: var(--gs-space-1) var(--gs-space-1-5) 0;
  }
  .console-output ::selection {
    background: var(--gs-console-selection);
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
  /* Syntax colours (and the underline of an unresolved segment) are global:
     styles/syntax.css. */
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
    background: var(--gs-console-progress-bg);
  }
  .entry mark {
    background: var(--gs-console-find-hit-bg);
    color: inherit;
  }
  .entry.find-current mark {
    background: var(--gs-console-find-current-bg);
  }
  .sig-hint {
    flex: none;
    margin: 0 var(--gs-space-1-5);
    padding: var(--gs-space-0-5) var(--gs-space-2);
    white-space: pre-wrap;
    background: var(--gs-menu-bg);
    color: var(--gs-menu-fg);
    border: var(--gs-border-width) solid var(--gs-border);
    font-size: var(--gs-console-popup-font-size);
  }
  .sig-arg {
    text-decoration-thickness: 2px;
    color: var(--gs-syntax-attribute);
  }
  .console-input-row {
    display: flex;
    align-items: flex-start;
    gap: 0.5em;
    padding: var(--gs-space-0-5) var(--gs-space-1-5) var(--gs-space-1);
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
</style>
