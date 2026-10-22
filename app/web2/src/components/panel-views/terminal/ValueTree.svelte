<script lang="ts">
  // A value from its tagged JSON: an object as a link to its node, a list
  // or map as an expandable tree of its items (each a ValueTree), anything
  // else as its text.  `text` is the top level's own text (the value entry
  // as printed), drawn through `label`; nested values show lib/
  // taggedValue's nestedText.  A list or map printed on several lines is
  // summed up by its first line and its size while closed, and by its size
  // alone while open, so the items below do not repeat it (S1018-79).
  import type { Snippet } from 'svelte';
  import { nestedText, valueShape } from '@/lib/taggedValue';
  import ValueTree from './ValueTree.svelte';

  let {
    value,
    text: own,
    label,
    onlink,
  }: {
    value: unknown;
    text?: string;
    label?: Snippet<[string]>;
    // A click on an object link.
    onlink: (ev: MouseEvent, path: string) => void;
  } = $props();

  const shape = $derived(valueShape(value));
  const top = $derived(label !== undefined && own !== undefined);
  const nl = $derived(own === undefined ? -1 : own.indexOf('\n'));
  let open = $state(false);
  const count = $derived(
    shape.kind === 'list' || shape.kind === 'map'
      ? `${shape.items.length} ${shape.items.length === 1 ? 'item' : 'items'}`
      : '',
  );
</script>

{#snippet text(fallback: string)}
  {#if top}{@render label!(own!)}{:else}{fallback}{/if}
{/snippet}

{#if shape.kind === 'object'}
  {@const path = shape.path}
  <a href="#{path}" class="obj-link" onclick={(ev) => onlink(ev, path)}>{@render text(path)}</a>
{:else if shape.kind === 'list' || shape.kind === 'map'}
  <details class={top ? 'value-tree' : 'nested'} bind:open>
    <summary class="gs-summary"
      >{#if top && nl >= 0}{#if open}<span class="count">{count}</span>{:else}{@render label!(
            own!.slice(0, nl),
          )}<span class="count">{` … ${count}`}</span>{/if}{:else}{@render text(
          nestedText(value),
        )}{/if}</summary
    >
    <div class="kv">
      {#each shape.items as [k, x], i (i)}
        <div class="kv-row"><span class="kv-key">{k}</span><ValueTree value={x} {onlink} /></div>
      {/each}
    </div>
  </details>
{:else if top}
  {@render label!(own!)}
{:else}
  <span class="kv-val">{nestedText(value)}</span>
{/if}

<style>
  .obj-link {
    color: inherit;
    text-decoration: underline dotted;
    cursor: pointer;
  }
  .obj-link:hover {
    text-decoration: underline;
  }
  .count {
    color: var(--gs-syntax-dim);
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
</style>
