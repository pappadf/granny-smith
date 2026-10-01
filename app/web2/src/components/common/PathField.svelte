<script lang="ts">
  // A path argument's field: text, plus a Browse list of the emulator's file
  // system (files.list), one directory at a time.  Picking a directory opens
  // it; picking a file (or "Use this folder") fills the field.
  import { gsEval } from '@/bus/emulator';

  interface Props {
    value: string;
    label?: string;
    onInput: (text: string) => void;
  }
  let { value, label = 'Path', onInput }: Props = $props();

  interface Entry {
    name: string;
    kind: string;
  }

  // svelte-ignore state_referenced_locally
  let text = $state(value);
  let open = $state(false);
  let dir = $state('/opfs');
  let entries = $state<Entry[]>([]);
  let error = $state('');

  function parentOf(p: string): string {
    const i = p.replace(/\/+$/, '').lastIndexOf('/');
    return i > 0 ? p.slice(0, i) : '/';
  }

  async function list(d: string): Promise<void> {
    error = '';
    const r = await gsEval('files.list', [d]);
    if (!Array.isArray(r)) {
      error = `cannot list ${d}`;
      entries = [];
      return;
    }
    dir = d;
    entries = r
      .filter((e): e is Entry => !!e && typeof e === 'object' && typeof e.name === 'string')
      .sort((a, b) =>
        a.kind === b.kind ? a.name.localeCompare(b.name) : a.kind === 'directory' ? -1 : 1,
      );
  }

  function set(t: string): void {
    text = t;
    onInput(t);
  }

  function browse(): void {
    open = !open;
    if (!open) return;
    // Start where the field points, else at /opfs.
    const start = text.trim() ? (text.endsWith('/') ? text : parentOf(text)) : '/opfs';
    void list(start || '/opfs').then(() => {
      if (error) void list('/opfs');
    });
  }

  function pick(e: Entry): void {
    const full = `${dir.replace(/\/+$/, '')}/${e.name}`;
    if (e.kind === 'directory') void list(full);
    else {
      set(full);
      open = false;
    }
  }
</script>

<span class="path-field">
  <span class="row">
    <input
      type="text"
      aria-label={label}
      spellcheck="false"
      autocomplete="off"
      value={text}
      oninput={(ev) => set((ev.currentTarget as HTMLInputElement).value)}
    />
    <button type="button" class="browse" aria-expanded={open} onclick={browse}>Browse…</button>
  </span>
  {#if open}
    <div class="listing" role="listbox" aria-label="Files in {dir}">
      <div class="dir">{dir}</div>
      {#if dir !== '/'}
        <button type="button" class="entry directory" onclick={() => void list(parentOf(dir))}
          >..</button
        >
      {/if}
      {#each entries as e (e.name)}
        <button
          type="button"
          class="entry {e.kind}"
          role="option"
          aria-selected="false"
          onclick={() => pick(e)}>{e.name}{e.kind === 'directory' ? '/' : ''}</button
        >
      {/each}
      <button
        type="button"
        class="use-dir"
        onclick={() => {
          set(dir);
          open = false;
        }}>Use this folder</button
      >
      {#if error}<div class="error">{error}</div>{/if}
    </div>
  {/if}
</span>

<style>
  .path-field {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
    min-width: 0;
  }
  .row {
    display: flex;
    gap: var(--gs-space-1);
  }
  input {
    flex: 1;
    min-width: 12ch;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
    color: var(--gs-text);
    background: var(--gs-input-bg);
    border: var(--gs-border-width) solid var(--gs-border);
    padding: var(--gs-space-px) var(--gs-space-1);
  }
  .browse,
  .use-dir {
    font: inherit;
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
  }
  .listing {
    display: flex;
    flex-direction: column;
    max-height: 160px;
    overflow-y: auto;
    border: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-surface-app);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
  }
  .dir {
    color: var(--gs-text-muted);
    padding: var(--gs-space-0-5) var(--gs-space-1);
  }
  .entry {
    text-align: left;
    background: none;
    border: none;
    color: var(--gs-text);
    padding: var(--gs-space-px) var(--gs-space-2);
    cursor: pointer;
    font: inherit;
  }
  .entry:hover {
    background: var(--gs-menu-hover-bg);
    color: var(--gs-menu-hover-fg);
  }
  .entry.directory {
    color: var(--gs-syntax-type);
  }
  .error {
    color: var(--gs-syntax-error);
    padding: var(--gs-space-0-5) var(--gs-space-1);
  }
</style>
