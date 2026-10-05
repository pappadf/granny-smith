<script lang="ts">
  // A native <select> with a drawn closed box and chevron.  Its list is the
  // app's own menu (so every skin draws it), not the operating system's: a
  // click, Enter, Space or an arrow key opens a menu of the options, and
  // choosing one sets the select's value and fires its input and change
  // events.  On touch the platform's picker stays.  Children are the
  // <option>s.
  // `class` and `style` (layout only) land on the wrapper; every other
  // attribute (id, onchange, aria-label…) reaches the <select>.
  import type { Snippet } from 'svelte';
  import type { HTMLSelectAttributes } from 'svelte/elements';
  import Icon from '@/components/common/Icon.svelte';
  import { openContextMenu } from '@/components/common/ContextMenu.svelte';

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

  let lastPointer = 'mouse';

  function choose(el: HTMLSelectElement, i: number) {
    if (el.selectedIndex === i) return;
    el.selectedIndex = i;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  function openList() {
    const el = ref;
    if (!el || el.disabled) return;
    const r = el.getBoundingClientRect();
    const opts = Array.from(el.options);
    openContextMenu(
      opts.map((o, i) => ({
        label: o.text,
        checked: i === el.selectedIndex,
        disabled: o.disabled,
        action: () => choose(el, i),
      })),
      r.left,
      r.bottom + 2,
      { minWidth: r.width, highlight: el.selectedIndex, returnFocus: el },
    );
  }

  function onMouseDown(ev: MouseEvent) {
    if (lastPointer === 'touch' || ev.button !== 0) return;
    ev.preventDefault();
    ref?.focus();
    openList();
  }

  function onKeyDown(ev: KeyboardEvent) {
    if (lastPointer === 'touch') return;
    if (
      ['Enter', ' ', 'ArrowDown', 'ArrowUp', 'F4'].includes(ev.key) &&
      !ev.ctrlKey &&
      !ev.metaKey
    ) {
      ev.preventDefault();
      openList();
    }
  }
</script>

<span class="gs-select {cls}" data-size={size} data-mono={mono || undefined} {style}>
  <select
    bind:this={ref}
    bind:value
    class="gs-select__control"
    aria-invalid={invalid || undefined}
    {...rest}
    onpointerdown={(ev) => (lastPointer = ev.pointerType)}
    onmousedown={onMouseDown}
    onkeydown={onKeyDown}
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
    text-overflow: ellipsis;
    white-space: nowrap;
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
  .gs-select :global(.gs-select__arrow) {
    position: absolute;
    right: var(--gs-space-1-5);
    width: var(--gs-select-arrow-size);
    height: var(--gs-select-arrow-size);
    color: var(--gs-text-muted);
    pointer-events: none;
  }
</style>
