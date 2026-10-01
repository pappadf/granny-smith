<script lang="ts">
  // A native <select> with a drawn closed box and chevron.  Where the browser
  // has customizable selects (appearance: base-select) the option popup is
  // drawn like the app's menus, from the menu tokens; elsewhere it stays
  // native and follows the colour scheme.  Children are the <option>s.
  // `class` and `style` (layout only) land on the wrapper; every other
  // attribute (id, onchange, aria-label…) reaches the <select>.
  import type { Snippet } from 'svelte';
  import type { HTMLSelectAttributes } from 'svelte/elements';
  import Icon from '@/components/common/Icon.svelte';

  interface Props extends Omit<HTMLSelectAttributes, 'size' | 'value' | 'children'> {
    value?: unknown;
    // inline: in a text row; sm: a panel field; md: a form field.
    size?: 'inline' | 'sm' | 'md';
    mono?: boolean;
    invalid?: boolean;
    ref?: HTMLSelectElement | null;
    children?: Snippet;
  }
  let {
    value = $bindable(),
    size = 'md',
    mono = false,
    invalid = false,
    ref = $bindable(null),
    class: cls = '',
    style,
    children,
    ...rest
  }: Props = $props();
</script>

<span class="gs-select {cls}" data-size={size} data-mono={mono || undefined} {style}>
  <select
    bind:this={ref}
    bind:value
    class="gs-select__control"
    aria-invalid={invalid || undefined}
    {...rest}
  >
    {@render children?.()}
  </select>
  <Icon name="chevron" size="xs" class="gs-select__arrow" />
</span>

<style>
  .gs-select {
    position: relative;
    display: inline-flex;
    align-items: center;
    min-width: 0;
  }
  .gs-select__control {
    appearance: none;
    width: 100%;
    height: var(--gs-select-height);
    padding: 0 calc(var(--gs-space-1-5) + var(--gs-select-arrow-size) + var(--gs-space-1)) 0
      var(--gs-space-1-5);
    border: var(--gs-border-width) solid var(--gs-input-border);
    border-radius: var(--gs-input-radius);
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    font-size: var(--gs-font-size-base);
    cursor: pointer;
  }
  .gs-select[data-size='sm'] .gs-select__control {
    height: var(--gs-input-height);
    font-size: var(--gs-input-font-size);
  }
  .gs-select[data-size='inline'] .gs-select__control {
    height: auto;
    font-size: var(--gs-font-size-sm);
  }
  .gs-select[data-mono] .gs-select__control {
    font-family: var(--gs-font-mono);
  }
  .gs-select__control:focus-visible {
    outline: none;
  }
  .gs-select__control:focus {
    border-color: var(--gs-input-border-focus);
  }
  .gs-select__control[aria-invalid='true'] {
    border-color: var(--gs-input-border-invalid);
  }
  .gs-select__control:disabled {
    opacity: var(--gs-opacity-disabled);
    cursor: default;
  }
  /* The popup, drawn like a context menu (.gs-menu) where the browser lets
     CSS draw it.  The closed box keeps the rules above; its own picker icon
     is hidden in favour of the drawn chevron. */
  @supports (appearance: base-select) {
    .gs-select__control,
    .gs-select__control::picker(select) {
      appearance: base-select;
    }
    .gs-select__control {
      display: flex;
      align-items: center;
    }
    .gs-select__control::picker-icon {
      display: none;
    }
    .gs-select__control::picker(select) {
      min-width: anchor-size(width);
      max-height: 60vh;
      margin: var(--gs-space-0-5) 0;
      padding: var(--gs-space-1) 0;
      border: var(--gs-border-width) solid var(--gs-border);
      border-radius: var(--gs-menu-radius);
      background: var(--gs-menu-bg);
      color: var(--gs-menu-fg);
      box-shadow: var(--gs-shadow-popup);
    }
    .gs-select__control :global(option) {
      display: flex;
      align-items: center;
      gap: var(--gs-space-1-5);
      min-height: var(--gs-menu-item-height);
      padding: 0 var(--gs-menu-item-padding-x) 0 var(--gs-space-1-5);
      font-size: var(--gs-font-size-base);
      white-space: nowrap;
      cursor: pointer;
    }
    .gs-select__control :global(option)::checkmark {
      width: 1em;
      text-align: center;
    }
    .gs-select__control :global(option):not(:checked)::checkmark {
      visibility: hidden;
    }
    .gs-select__control :global(option):hover,
    .gs-select__control :global(option):focus-visible {
      outline: none;
      background: var(--gs-menu-hover-bg);
      color: var(--gs-menu-hover-fg);
    }
    .gs-select__control :global(option):disabled {
      opacity: var(--gs-opacity-disabled);
      background: transparent;
      color: inherit;
    }
  }
  .gs-select :global(.gs-select__arrow) {
    position: absolute;
    right: var(--gs-space-1-5);
    width: var(--gs-select-arrow-size);
    height: var(--gs-select-arrow-size);
    color: var(--gs-text-muted);
    pointer-events: none;
  }
</style>
