<script lang="ts">
  // A latching toggle drawn as a small chip: faded off, filled with a ring
  // on (the status bar's Caps Lock).
  import type { Snippet } from 'svelte';
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props {
    pressed: boolean;
    label: string;
    onToggle: () => void;
    icon?: IconName;
    title?: string;
    class?: string;
    children?: Snippet;
  }
  let { pressed, label, onToggle, icon, title, class: cls = '', children }: Props = $props();
</script>

<button
  type="button"
  class="gs-chip {cls}"
  class:on={pressed}
  aria-pressed={pressed}
  aria-label={label}
  {title}
  onclick={onToggle}
>
  {#if icon}<Icon name={icon} size={13} class="gs-chip__icon" />{/if}
  {#if children}<span class="gs-chip__label">{@render children()}</span>{/if}
</button>

<style>
  .gs-chip {
    display: inline-flex;
    align-items: center;
    align-self: center;
    gap: var(--gs-space-1);
    height: var(--gs-chip-height);
    padding: 0 var(--gs-chip-padding-x);
    border: none;
    border-radius: var(--gs-chip-radius);
    background: none;
    color: inherit;
    line-height: inherit;
    opacity: var(--gs-chip-off-opacity);
    cursor: pointer;
  }
  .gs-chip[aria-pressed='true'] {
    opacity: 1;
    font-weight: var(--gs-font-weight-bold);
    background: var(--gs-chip-on-bg);
    box-shadow: inset 0 0 0 var(--gs-border-width) var(--gs-chip-on-ring);
  }
</style>
