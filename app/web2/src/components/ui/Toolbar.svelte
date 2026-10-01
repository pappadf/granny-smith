<script lang="ts">
  import type { Snippet } from 'svelte';

  // A row of controls (role=toolbar).  `bar` is a full-width strip with its
  // own fill and bottom rule (the display toolbar); `inline` is a group
  // inside another header (the debug actions).  ←/→ (↑/↓ when vertical),
  // Home and End move focus between its enabled controls; Tab still
  // reaches each of them.  `class` carries legacy hooks.
  interface Props {
    label: string;
    variant?: 'bar' | 'inline';
    orientation?: 'horizontal' | 'vertical';
    class?: string;
    children: Snippet;
  }
  let {
    label,
    variant = 'bar',
    orientation = 'horizontal',
    class: cls = '',
    children,
  }: Props = $props();

  const FOCUSABLE = 'button:not(:disabled), input:not(:disabled), select:not(:disabled), a[href]';

  // Move focus to the previous / next / first / last control.
  function onKey(ev: KeyboardEvent): void {
    const prev = orientation === 'horizontal' ? 'ArrowLeft' : 'ArrowUp';
    const next = orientation === 'horizontal' ? 'ArrowRight' : 'ArrowDown';
    if (![prev, next, 'Home', 'End'].includes(ev.key)) return;
    const target = ev.target as HTMLElement;
    // Text fields keep their own caret keys.
    if (target instanceof HTMLInputElement && target.type !== 'range') return;
    const items = Array.from(
      (ev.currentTarget as HTMLElement).querySelectorAll<HTMLElement>(FOCUSABLE),
    );
    const at = items.indexOf(target);
    if (at < 0 || !items.length) return;
    let to = at;
    if (ev.key === prev) to = Math.max(0, at - 1);
    else if (ev.key === next) to = Math.min(items.length - 1, at + 1);
    else if (ev.key === 'Home') to = 0;
    else to = items.length - 1;
    ev.preventDefault();
    items[to].focus();
  }
</script>

<!-- svelte-ignore a11y_interactive_supports_focus -->
<div
  class="gs-toolbar {cls}"
  data-variant={variant}
  role="toolbar"
  aria-label={label}
  aria-orientation={orientation}
  onkeydown={onKey}
>
  {@render children()}
</div>

<style>
  .gs-toolbar {
    display: flex;
    align-items: center;
  }
  .gs-toolbar[aria-orientation='vertical'] {
    flex-direction: column;
  }
  .gs-toolbar[data-variant='bar'] {
    height: var(--gs-toolbar-height);
    flex: 0 0 var(--gs-toolbar-height);
    padding: 0 var(--gs-space-2);
    background: var(--gs-toolbar-bg);
    border-bottom: var(--gs-border-width) solid var(--gs-toolbar-border);
    color: var(--gs-toolbar-fg);
    user-select: none;
  }
  .gs-toolbar[data-variant='inline'] {
    display: inline-flex;
    gap: var(--gs-toolbar-gap-inline);
  }
</style>
