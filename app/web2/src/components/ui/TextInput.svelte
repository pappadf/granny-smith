<script lang="ts">
  // A single-line text field.  Focus shows as the border colour (no ring);
  // `invalid` sets aria-invalid and a danger border.  `size` lg is the
  // dialog field.  Extra attributes (id, placeholder, aria-label, onkeydown…)
  // reach the <input>.
  import type { HTMLInputAttributes } from 'svelte/elements';

  interface Props extends Omit<HTMLInputAttributes, 'size' | 'value'> {
    value?: string | number | null;
    // inline: sits in a text row (SYSTEM's value column); sm: a panel
    // field; lg: a dialog field.
    size?: 'inline' | 'sm' | 'lg';
    mono?: boolean;
    // A hex address or value: monospace, upper-case digits.
    hex?: boolean;
    // No box until focused (an inline field in a toolbar).
    bare?: boolean;
    invalid?: boolean;
    // Width in characters (layout).
    widthCh?: number;
    ref?: HTMLInputElement | null;
  }
  let {
    value = $bindable(''),
    size = 'sm',
    mono = false,
    hex = false,
    bare = false,
    invalid = false,
    widthCh,
    ref = $bindable(null),
    type = 'text',
    class: cls = '',
    style,
    ...rest
  }: Props = $props();
</script>

<input
  bind:this={ref}
  bind:value
  class="gs-input {cls}"
  data-size={size}
  data-mono={mono || hex || undefined}
  data-hex={hex || undefined}
  data-bare={bare || undefined}
  aria-invalid={invalid || undefined}
  {type}
  style={widthCh ? `width: ${widthCh}ch;${style ?? ''}` : style}
  {...rest}
/>

<style>
  .gs-input {
    height: var(--gs-input-height);
    padding: 0 var(--gs-input-padding-x);
    border: var(--gs-border-width) solid var(--gs-input-border);
    border-radius: var(--gs-input-radius);
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    font-size: var(--gs-input-font-size);
    min-width: 0;
  }
  .gs-input[data-size='lg'] {
    height: var(--gs-input-height-lg);
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-input-font-size-lg);
  }
  .gs-input[data-size='inline'] {
    height: auto;
    padding: 0 var(--gs-space-1);
    font-size: var(--gs-font-size-sm);
  }
  .gs-input[data-mono] {
    font-family: var(--gs-font-mono);
  }
  .gs-input[data-hex] {
    text-transform: uppercase; /* hex digits */
  }
  .gs-input[data-bare] {
    background: transparent;
    border-color: transparent;
    color: var(--gs-text-strong);
    text-align: center;
  }
  .gs-input:focus-visible {
    outline: none;
  }
  .gs-input:focus {
    border-color: var(--gs-input-border-focus);
  }
  .gs-input[aria-invalid='true'] {
    border-color: var(--gs-input-border-invalid);
  }
  .gs-input:disabled {
    opacity: var(--gs-opacity-disabled);
  }
</style>
