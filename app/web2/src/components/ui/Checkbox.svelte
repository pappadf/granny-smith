<script lang="ts">
  // A checkbox with its label: a view option (autoscroll, Advanced).  The box
  // is native, in the accent colour.  `class` lands on the <label>.
  import type { Snippet } from 'svelte';

  interface Props {
    checked?: boolean;
    label?: string;
    disabled?: boolean;
    title?: string;
    // sm: a compact view option in a header (small, muted label).
    size?: 'sm' | 'md';
    onchange?: (checked: boolean) => void;
    class?: string;
    children?: Snippet;
  }
  let {
    checked = $bindable(false),
    label,
    disabled = false,
    title,
    size = 'md',
    onchange,
    class: cls = '',
    children,
  }: Props = $props();
</script>

<label class="gs-check {cls}" data-size={size} {title}>
  <input
    type="checkbox"
    class="gs-check__control"
    bind:checked
    {disabled}
    onchange={(e) => onchange?.((e.target as HTMLInputElement).checked)}
  />
  <span class="gs-check__label"
    >{#if children}{@render children()}{:else}{label}{/if}</span
  >
</label>

<style>
  .gs-check {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-1);
    cursor: pointer;
    user-select: none;
  }
  .gs-check[data-size='sm'] {
    font-size: var(--gs-font-size-xs);
    color: var(--gs-text-muted);
  }
  .gs-check__control {
    margin: 0;
  }
</style>
