<script lang="ts">
  // A value from its tagged JSON: an object as a link to its node, a list
  // or map as an expandable tree of its items (each a ValueTree), anything
  // else as its text.  `label` is the top level's own text (the value entry
  // as printed); nested values show lib/taggedValue's nestedText.
  import type { Snippet } from 'svelte';
  import { nestedText, valueShape } from '@/lib/taggedValue';
  import ValueTree from './ValueTree.svelte';

  let {
    value,
    label,
    onlink,
  }: {
    value: unknown;
    label?: Snippet;
    // A click on an object link.
    onlink: (ev: MouseEvent, path: string) => void;
  } = $props();

  const shape = $derived(valueShape(value));
</script>

{#snippet text(fallback: string)}
  {#if label}{@render label()}{:else}{fallback}{/if}
{/snippet}

{#if shape.kind === 'object'}
  {@const path = shape.path}
  <a href="#{path}" class="obj-link" onclick={(ev) => onlink(ev, path)}>{@render text(path)}</a>
{:else if shape.kind === 'list' || shape.kind === 'map'}
  <details class={label ? 'value-tree' : 'nested'}>
    <summary class="gs-summary">{@render text(nestedText(value))}</summary>
    <div class="kv">
      {#each shape.items as [k, x], i (i)}
        <div class="kv-row"><span class="kv-key">{k}</span><ValueTree value={x} {onlink} /></div>
      {/each}
    </div>
  </details>
{:else if label}
  {@render label()}
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
