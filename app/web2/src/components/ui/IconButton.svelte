<script lang="ts">
  // A button showing one icon.  `label` is required: it names the button for
  // assistive tech and is its tooltip.  `pressed` makes it a toggle
  // (aria-pressed).  `tone` picks the toolbar (rounder) or panel shape.
  import type { HTMLButtonAttributes } from 'svelte/elements';
  import Icon, { type IconSize } from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props extends Omit<HTMLButtonAttributes, 'children'> {
    icon: IconName;
    label: string;
    size?: 'sm' | 'md';
    tone?: 'toolbar' | 'panel';
    pressed?: boolean;
    // Live: what it controls is active right now (a camera capturing).
    live?: boolean;
    // A resting state drawn faded until hovered or focused.
    rest?: 'normal' | 'faded';
    // The icon's named size (the sprite's glyphs are drawn for 16).
    iconSize?: IconSize;
    // The tooltip, when it should differ from the label.
    title?: string;
    ref?: HTMLButtonElement | null;
  }
  let {
    icon,
    label,
    size = 'md',
    tone = 'toolbar',
    pressed,
    live = false,
    rest: restStyle = 'normal',
    iconSize = 'base',
    title,
    ref = $bindable(null),
    type = 'button',
    class: cls = '',
    ...rest
  }: Props = $props();
</script>

<button
  bind:this={ref}
  class="gs-icon-button {cls}"
  data-size={size}
  data-tone={tone}
  data-rest={restStyle}
  data-state={live ? 'live' : undefined}
  {type}
  aria-label={label}
  title={title ?? label}
  aria-pressed={pressed}
  {...rest}
>
  <Icon name={icon} size={iconSize} class="gs-icon-button__icon" />
</button>

<style>
  .gs-icon-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex: none;
    width: var(--gs-icon-button-size);
    height: var(--gs-icon-button-size);
    padding: var(--gs-icon-button-padding);
    border: none;
    border-radius: var(--gs-icon-button-radius);
    background: transparent;
    color: var(--gs-icon-button-fg);
    cursor: pointer;
  }
  .gs-icon-button[data-tone='panel'] {
    border-radius: var(--gs-icon-button-radius-panel);
  }
  .gs-icon-button[data-size='sm'] {
    width: var(--gs-icon-button-size-sm);
    height: var(--gs-icon-button-size-sm);
    padding: 0;
  }
  .gs-icon-button:hover:not(:disabled) {
    background: var(--gs-icon-button-bg-hover);
  }
  .gs-icon-button:active:not(:disabled) {
    background: var(--gs-icon-button-bg-active);
  }
  .gs-icon-button[aria-pressed='true'] {
    background: var(--gs-icon-button-bg-pressed);
  }
  .gs-icon-button[data-state='live'] {
    color: var(--gs-icon-button-fg-on);
  }
  .gs-icon-button[data-rest='faded'] {
    opacity: var(--gs-icon-button-rest-opacity);
  }
  .gs-icon-button[data-rest='faded']:hover,
  .gs-icon-button[data-rest='faded']:focus-visible {
    opacity: 1;
  }
  .gs-icon-button:disabled {
    opacity: var(--gs-opacity-disabled);
    cursor: default;
  }
</style>
