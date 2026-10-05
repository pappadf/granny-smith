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
  import TextInput from '@/components/ui/TextInput.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';

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
  <TextInput
    bind:ref={inputEl}
    bind:value={find.query}
    class="find-input"
    style="width: 14em"
    placeholder="Find"
    aria-label="Find in console output"
    onkeydown={onKey}
  />
  <span class="find-count"
    >{find.hits.length
      ? `${(find.index % find.hits.length) + 1} of ${find.hits.length}`
      : 'No results'}</span
  >
  <IconButton
    class="find-btn"
    tone="panel"
    size="sm"
    iconSize="md"
    icon="case-sensitive"
    label="Match case"
    pressed={find.caseSensitive}
    onclick={() => (find.caseSensitive = !find.caseSensitive)}
  />
  <IconButton
    class="find-btn"
    tone="panel"
    size="sm"
    iconSize="md"
    icon="arrow-up"
    label="Previous match"
    onclick={() => step(-1)}
  />
  <IconButton
    class="find-btn"
    tone="panel"
    size="sm"
    iconSize="md"
    icon="arrow-down"
    label="Next match"
    onclick={() => step(1)}
  />
  <IconButton
    class="find-btn"
    tone="panel"
    size="sm"
    iconSize="md"
    icon="close"
    label="Close find"
    title="Close"
    onclick={onclose}
  />
</div>

<style>
  .find {
    position: absolute;
    top: 4px;
    right: 12px;
    z-index: var(--gs-z-raised);
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    padding: var(--gs-space-1);
    background: var(--gs-menu-bg);
    color: var(--gs-menu-fg);
    border: var(--gs-border-width) solid var(--gs-border);
    font-family: var(--gs-font-ui);
    font-size: var(--gs-font-size-sm);
  }
  .find-count {
    min-width: 5.5em;
    color: var(--gs-console-muted-fg);
  }
</style>
