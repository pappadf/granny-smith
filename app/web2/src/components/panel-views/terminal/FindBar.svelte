<script lang="ts" module>
  import { findMatches, findParts, type FindPart } from '@/lib/find';

  // Find in the console's output: the query, the entries it hits and the
  // current one.  The view marks hits with it; FindBar edits it.
  export class FindState {
    open = $state(false);
    #query = $state('');
    #caseSensitive = $state(false);
    index = $state(0);
    readonly #items: () => readonly { id: number; text: string }[];

    constructor(items: () => readonly { id: number; text: string }[]) {
      this.#items = items;
    }

    get query(): string {
      return this.#query;
    }
    set query(q: string) {
      this.#query = q;
      this.index = 0;
    }
    get caseSensitive(): boolean {
      return this.#caseSensitive;
    }
    set caseSensitive(c: boolean) {
      this.#caseSensitive = c;
      this.index = 0;
    }

    // Open with something to find.
    readonly active = $derived(this.open && !!this.#query);
    // Ids of the items that match, in order.
    readonly hits = $derived.by(() =>
      this.active
        ? this.#items()
            .filter((e) => findMatches(e.text, this.#query, this.#caseSensitive))
            .map((e) => e.id)
        : [],
    );
    readonly hitSet = $derived(new Set(this.hits));
    readonly current = $derived(this.hits.length ? this.hits[this.index % this.hits.length] : -1);

    // `text` split around the matches (all of it one part while inactive).
    parts(text: string): FindPart[] {
      return findParts(text, this.active ? this.#query : '', this.#caseSensitive);
    }

    step(delta: number): void {
      const n = this.hits.length;
      if (n) this.index = (this.index + delta + n) % n;
    }
  }
</script>

<script lang="ts">
  // The find bar over the console's output: the query, match case,
  // previous / next (Enter, Shift+Enter) and close (Esc).
  let {
    find,
    onstep,
    onclose,
  }: {
    find: FindState;
    // After the current match moved.
    onstep: () => void;
    onclose: () => void;
  } = $props();

  let inputEl = $state<HTMLInputElement | null>(null);

  // Focus the query with its text selected.
  export function focus(): void {
    inputEl?.select();
  }

  function step(delta: number): void {
    if (!find.hits.length) return;
    find.step(delta);
    onstep();
  }

  function onKey(ev: KeyboardEvent): void {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      step(ev.shiftKey ? -1 : 1);
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      onclose();
    }
  }
</script>

<div class="find" role="search">
  <input
    bind:this={inputEl}
    bind:value={find.query}
    class="find-input"
    placeholder="Find"
    aria-label="Find in console output"
    onkeydown={onKey}
  />
  <span class="find-count"
    >{find.hits.length
      ? `${(find.index % find.hits.length) + 1} of ${find.hits.length}`
      : 'No results'}</span
  >
  <button
    class="find-btn"
    class:on={find.caseSensitive}
    title="Match case"
    aria-pressed={find.caseSensitive}
    onclick={() => (find.caseSensitive = !find.caseSensitive)}>Aa</button
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
  <button class="find-btn" title="Close" aria-label="Close find" onclick={onclose}>×</button>
</div>

<style>
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
