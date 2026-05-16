<script lang="ts" module>
  import { mount, unmount } from 'svelte';
  import Self from './ContextMenu.svelte';

  export type ContextMenuItem =
    | { label: string; action: () => void; danger?: boolean }
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
      if (!('sep' in it)) out.push(i);
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

  function activate(item: ContextMenuItem): void {
    if ('sep' in item) return;
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

<div
  class="context-menu"
  role="menu"
  tabindex="-1"
  style="left: {pos.left}px; top: {pos.top}px;"
  bind:this={cardEl}
>
  {#each items as item, i (i)}
    {#if 'sep' in item}
      <div class="sep" role="separator"></div>
    {:else}
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <div
        class="item"
        class:danger={item.danger}
        class:highlight={highlight === i}
        role="menuitem"
        tabindex="-1"
        onclick={() => activate(item)}
        onmouseenter={() => (highlight = i)}
      >
        {item.label}
      </div>
    {/if}
  {/each}
</div>

<style>
  .context-menu {
    position: fixed;
    background: var(--gs-menu-bg);
    color: var(--gs-menu-fg);
    border: 1px solid var(--gs-border);
    border-radius: 4px;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.5);
    min-width: 160px;
    padding: 4px 0;
    z-index: 2800;
    outline: none;
    user-select: none;
  }
  .item {
    height: 22px;
    line-height: 22px;
    padding: 0 12px;
    font-size: 13px;
    cursor: pointer;
  }
  .item.highlight {
    background: var(--gs-menu-hover-bg);
    color: var(--gs-menu-hover-fg);
  }
  .item.danger {
    color: var(--gs-toast-error);
  }
  .sep {
    height: 1px;
    background: var(--gs-border);
    margin: 4px 0;
  }
</style>
