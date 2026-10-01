<script lang="ts">
  // An inline, borderless value editor (a register): the border appears on
  // hover and focus; `changed` highlights a value that moved since the last
  // step; `invalid` flashes a refused entry.
  import type { HTMLInputAttributes } from 'svelte/elements';

  interface Props extends Omit<HTMLInputAttributes, 'value'> {
    value?: string;
    changed?: boolean;
    invalid?: boolean;
    widthCh?: number;
    ref?: HTMLInputElement | null;
  }
  let {
    value = $bindable(''),
    changed = false,
    invalid = false,
    widthCh,
    ref = $bindable(null),
    class: cls = '',
    ...rest
  }: Props = $props();
</script>

<input
  bind:this={ref}
  bind:value
  class="gs-inline-input {cls}"
  data-state={changed ? 'changed' : undefined}
  aria-invalid={invalid || undefined}
  style={widthCh ? `width: ${widthCh}ch` : undefined}
  {...rest}
/>

<style>
  .gs-inline-input {
    /* content-box, so a width in ch is the digits' own width. */
    box-sizing: content-box;
    height: var(--gs-inline-input-height);
    padding: 0 var(--gs-space-1);
    border: var(--gs-border-width) solid transparent;
    border-radius: var(--gs-radius-xs);
    background: transparent;
    color: var(--gs-code-reg-value);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    text-transform: uppercase; /* hex digits */
  }
  .gs-inline-input:hover {
    border-color: var(--gs-input-border);
  }
  .gs-inline-input:focus-visible {
    outline: none;
  }
  .gs-inline-input:focus {
    border-color: var(--gs-input-border-focus);
    background: var(--gs-input-bg);
  }
  .gs-inline-input[data-state='changed'] {
    background: var(--gs-code-changed-bg);
    color: var(--gs-code-changed-fg);
  }
  .gs-inline-input[aria-invalid='true'] {
    border-color: var(--gs-code-invalid);
  }
  .gs-inline-input[readonly] {
    cursor: default;
  }
</style>
