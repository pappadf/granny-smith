<script lang="ts">
  // A text button in one of four variants and two sizes: `sm` for in-panel
  // actions, `lg` for dialog actions.  With `href` it is a link styled as a
  // button (Open in new tab, Download).  Call sites keep their legacy classes
  // (`class="btn-primary"`) for the tests that select them; the skin hooks
  // are the gs-button classes.
  import type { Snippet } from 'svelte';
  import type { HTMLButtonAttributes } from 'svelte/elements';
  import Icon from '@/components/common/Icon.svelte';
  import Spinner from './Spinner.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props extends Omit<HTMLButtonAttributes, 'children'> {
    variant?: 'primary' | 'secondary' | 'danger' | 'ghost';
    size?: 'sm' | 'lg';
    // Render a link styled as a button.
    href?: string;
    download?: string | boolean;
    target?: string;
    rel?: string;
    // Working: shows a spinner and sets aria-busy.
    busy?: boolean;
    // A leading icon.
    icon?: IconName;
    // The root element, for focus management.
    ref?: HTMLElement | null;
    children?: Snippet;
  }
  let {
    variant = 'secondary',
    size = 'sm',
    href,
    download,
    target,
    rel,
    busy = false,
    icon,
    ref = $bindable(null),
    type = 'button',
    class: cls = '',
    children,
    ...rest
  }: Props = $props();
</script>

{#snippet content()}
  {#if busy}<Spinner size="sm" tone="current" class="gs-button__spinner" />{/if}
  {#if icon}<Icon name={icon} size={14} class="gs-button__icon" />{/if}
  {#if children}<span class="gs-button__label">{@render children()}</span>{/if}
{/snippet}

{#if href}
  <a
    bind:this={ref}
    class="gs-button {cls}"
    data-variant={variant}
    data-size={size}
    {href}
    {download}
    {target}
    {rel}
    aria-busy={busy || undefined}
    {...rest as Record<string, unknown>}>{@render content()}</a
  >
{:else}
  <button
    bind:this={ref}
    class="gs-button {cls}"
    data-variant={variant}
    data-size={size}
    {type}
    aria-busy={busy || undefined}
    {...rest}>{@render content()}</button
  >
{/if}

<style>
  .gs-button {
    --_bg: var(--gs-button-secondary-bg);
    --_fg: var(--gs-button-secondary-fg);
    --_border: var(--gs-button-secondary-border);
    --_bg-hover: var(--gs-button-secondary-bg-hover);
    --_bg-active: var(--gs-button-secondary-bg-active);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: var(--gs-space-1-5);
    height: var(--gs-button-height);
    padding: 0 var(--gs-button-padding-x);
    border: var(--gs-button-border-width) solid var(--_border);
    border-radius: var(--gs-button-radius);
    background: var(--_bg);
    color: var(--_fg);
    font-size: var(--gs-button-font-size);
    font-weight: var(--gs-button-font-weight);
    line-height: 1;
    text-decoration: none;
    white-space: nowrap;
    cursor: pointer;
  }
  .gs-button[data-size='lg'] {
    height: var(--gs-button-height-lg);
    padding: 0 var(--gs-button-padding-x-lg);
    font-size: var(--gs-button-font-size-lg);
  }
  .gs-button[data-variant='primary'] {
    --_bg: var(--gs-button-primary-bg);
    --_fg: var(--gs-button-primary-fg);
    --_border: var(--gs-button-primary-border);
    --_bg-hover: var(--gs-button-primary-bg-hover);
    --_bg-active: var(--gs-button-primary-bg-active);
  }
  .gs-button[data-variant='danger'] {
    --_bg: var(--gs-button-danger-bg);
    --_fg: var(--gs-button-danger-fg);
    --_border: var(--gs-button-danger-border);
    --_bg-hover: var(--gs-button-danger-bg-hover);
    --_bg-active: var(--gs-button-danger-bg-active);
  }
  .gs-button[data-variant='ghost'] {
    --_bg: var(--gs-button-ghost-bg);
    --_fg: var(--gs-button-ghost-fg);
    --_border: var(--gs-button-ghost-border);
    --_bg-hover: var(--gs-button-ghost-bg-hover);
    --_bg-active: var(--gs-button-ghost-bg-active);
  }
  .gs-button:hover:not(:disabled) {
    background: var(--_bg-hover);
  }
  .gs-button:active:not(:disabled) {
    background: var(--_bg-active);
  }
  .gs-button:disabled,
  .gs-button[aria-disabled='true'] {
    opacity: var(--gs-opacity-disabled);
    cursor: default;
  }
  .gs-button[aria-busy='true'] {
    cursor: progress;
  }
  .gs-button__label {
    display: inline-flex;
    align-items: center;
  }
</style>
