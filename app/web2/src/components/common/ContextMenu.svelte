<script lang="ts" module>
  import { mount, unmount } from 'svelte';
  import Self from './ContextMenu.svelte';

  import type { IconName } from '@/lib/icons';

  // A menu entry.  `checked` (true or false) makes it a checkable item with
  // a check mark column; `disabled` greys it out and skips it; `icon` and
  // `shortcut` sit before and after the label.
  export type ContextMenuItem =
    | {
        label: string;
        action: () => void;
        danger?: boolean;
        checked?: boolean;
        disabled?: boolean;
        icon?: IconName;
        shortcut?: string;
      }
    | { sep: true };

  // One menu at a time. Subsequent openContextMenu calls dismiss the previous.
  let current: { instance: ReturnType<typeof mount>; target: HTMLElement } | null = null;

  function closeCurrent(): void {
    if (!current) return;
    unmount(current.instance);
    document.body.removeChild(current.target);
    current = null;
  }

  export function openContextMenu(items: ContextMenuItem[], x: number, y: number): void {
    closeCurrent();
    const target = document.createElement('div');
    document.body.appendChild(target);
    const instance = mount(Self, { target, props: { items, x, y, onClose: closeCurrent } });
    current = { instance, target };
  }

  export function closeContextMenu(): void {
    closeCurrent();
  }
</script>

<script lang="ts">
  import { onMount } from 'svelte';
  import Icon from './Icon.svelte';

  interface Props {
    items: ContextMenuItem[];
    x: number;
    y: number;
    onClose: () => void;
  }
  let { items, x, y, onClose }: Props = $props();

  let cardEl = $state<HTMLDivElement | null>(null);
  // Highlight index — set on hover or arrow-key. Skip separators.
  let highlight = $state(-1);

  // Position clamped to viewport. Computed on mount once we know our size.
  // (x, y) captured once from props — subsequent prop changes don't reposition.
  // svelte-ignore state_referenced_locally
  let pos = $state({ left: x, top: y });

  function activableIndices(): number[] {
    const out: number[] = [];
    items.forEach((it, i) => {
      if (!('sep' in it) && !it.disabled) out.push(i);
    });
    return out;
  }

  function moveHighlight(delta: number): void {
    const idxs = activableIndices();
    if (!idxs.length) return;
    if (highlight < 0) {
      highlight = delta > 0 ? idxs[0] : idxs[idxs.length - 1];
      return;
    }
    const pos = idxs.indexOf(highlight);
    const next = idxs[(pos + delta + idxs.length) % idxs.length];
    highlight = next;
  }

  // Whether any entry is checkable: then every entry keeps a check column.
  const hasChecks = $derived(items.some((it) => !('sep' in it) && it.checked !== undefined));
  const hasIcons = $derived(items.some((it) => !('sep' in it) && !!it.icon));

  function activate(item: ContextMenuItem): void {
    if ('sep' in item || item.disabled) return;
    onClose();
    item.action();
  }

  onMount(() => {
    // Clamp position to viewport. requestAnimationFrame so cardEl's
    // rect has been computed by the time we measure.
    requestAnimationFrame(() => {
      if (!cardEl) return;
      const r = cardEl.getBoundingClientRect();
      const vw = window.innerWidth;
      const vh = window.innerHeight;
      let left = x;
      let top = y;
      if (left + r.width > vw - 4) left = Math.max(4, vw - r.width - 4);
      if (top + r.height > vh - 4) top = Math.max(4, vh - r.height - 4);
      pos = { left, top };
      cardEl.focus();
    });
    const onDocDown = (ev: MouseEvent) => {
      if (!cardEl) return;
      if (cardEl.contains(ev.target as Node)) return;
      onClose();
    };
    const onKey = (ev: KeyboardEvent) => {
      if (ev.key === 'Escape') {
        ev.preventDefault();
        onClose();
      } else if (ev.key === 'ArrowDown') {
        ev.preventDefault();
        moveHighlight(1);
      } else if (ev.key === 'ArrowUp') {
        ev.preventDefault();
        moveHighlight(-1);
      } else if (ev.key === 'Home') {
        ev.preventDefault();
        const idxs = activableIndices();
        if (idxs.length) highlight = idxs[0];
      } else if (ev.key === 'End') {
        ev.preventDefault();
        const idxs = activableIndices();
        if (idxs.length) highlight = idxs[idxs.length - 1];
      } else if (ev.key === 'Enter') {
        ev.preventDefault();
        if (highlight >= 0) activate(items[highlight]);
      }
    };
    // mousedown to also catch right-clicks elsewhere
    document.addEventListener('mousedown', onDocDown, true);
    document.addEventListener('keydown', onKey, true);
    return () => {
      document.removeEventListener('mousedown', onDocDown, true);
      document.removeEventListener('keydown', onKey, true);
    };
  });
</script>

<!-- Legacy hooks: context-menu, item, highlight, danger, sep. -->
<div
  class="gs-menu context-menu"
  role="menu"
  tabindex="-1"
  style="left: {pos.left}px; top: {pos.top}px;"
  bind:this={cardEl}
>
  {#each items as item, i (i)}
    {#if 'sep' in item}
      <div class="gs-menu__separator sep" role="separator"></div>
    {:else}
      <div
        class="gs-menu__item item"
        class:danger={item.danger}
        class:highlight={highlight === i}
        role={item.checked === undefined ? 'menuitem' : 'menuitemcheckbox'}
        aria-checked={item.checked}
        aria-disabled={item.disabled || undefined}
        tabindex="-1"
        onclick={() => activate(item)}
        onmouseenter={() => (highlight = item.disabled ? -1 : i)}
      >
        {#if hasChecks}
          <span class="gs-menu__check"
            >{#if item.checked}<Icon name="check" size={14} />{/if}</span
          >
        {/if}
        {#if hasIcons}
          <span class="gs-menu__icon"
            >{#if item.icon}<Icon name={item.icon} size={14} />{/if}</span
          >
        {/if}
        <span class="gs-menu__label">{item.label}</span>
        {#if item.shortcut}<span class="gs-menu__shortcut">{item.shortcut}</span>{/if}
      </div>
    {/if}
  {/each}
</div>

<style>
  .gs-menu {
    position: fixed;
    background: var(--gs-menu-bg);
    color: var(--gs-menu-fg);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-menu-radius);
    box-shadow: var(--gs-shadow-popup);
    min-width: 160px;
    padding: var(--gs-space-1) 0;
    z-index: var(--gs-z-menu);
    user-select: none;
  }
  /* The menu takes focus to read keys; the highlighted item shows where
     the keyboard is. */
  .gs-menu:focus-visible {
    outline: none;
  }
  .gs-menu__item {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1-5);
    height: var(--gs-menu-item-height);
    padding: 0 var(--gs-menu-item-padding-x);
    font-size: var(--gs-font-size-base);
    cursor: pointer;
  }
  .gs-menu__item.highlight {
    background: var(--gs-menu-hover-bg);
    color: var(--gs-menu-hover-fg);
  }
  .gs-menu__item.danger {
    color: var(--gs-danger-fg);
  }
  .gs-menu__item.danger.highlight {
    color: var(--gs-menu-hover-fg);
  }
  .gs-menu__item[aria-disabled='true'] {
    color: var(--gs-text-disabled);
    cursor: default;
  }
  .gs-menu__check,
  .gs-menu__icon {
    flex: none;
    width: var(--gs-size-icon-md);
    display: inline-flex;
    align-items: center;
  }
  .gs-menu__label {
    flex: 1 1 auto;
    white-space: nowrap;
  }
  .gs-menu__shortcut {
    flex: none;
    margin-left: var(--gs-space-6);
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-sm);
  }
  .gs-menu__item.highlight .gs-menu__shortcut {
    color: inherit;
  }
  .gs-menu__separator {
    height: var(--gs-border-width);
    background: var(--gs-menu-separator);
    margin: var(--gs-space-1) 0;
  }
</style>
