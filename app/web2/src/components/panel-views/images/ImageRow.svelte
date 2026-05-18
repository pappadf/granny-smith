<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props {
    name: string;
    desc?: string;
    icon: IconName;
    mounted?: boolean;
    selected?: boolean;
    onClick?: (ev: MouseEvent) => void;
    onDoubleClick?: (ev: MouseEvent) => void;
    onContextMenu?: (ev: MouseEvent) => void;
  }
  let {
    name,
    desc,
    icon,
    mounted = false,
    selected = false,
    onClick,
    onDoubleClick,
    onContextMenu,
  }: Props = $props();
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<div
  class="image-row"
  class:selected
  class:mounted
  role="row"
  tabindex="-1"
  onclick={onClick}
  ondblclick={onDoubleClick}
  oncontextmenu={onContextMenu}
>
  <span class="icon"><Icon name={icon} size={16} /></span>
  <span class="name">{name}</span>
  {#if mounted}
    <span class="badge">mounted</span>
  {/if}
  {#if desc}
    <span class="desc">{desc}</span>
  {/if}
</div>

<style>
  .image-row {
    display: flex;
    align-items: center;
    gap: 6px;
    height: 22px;
    padding: 0 8px 0 28px; /* 22 px indent for the icon line-up */
    cursor: pointer;
    user-select: none;
    color: var(--gs-fg);
    font-size: 13px;
  }
  .image-row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .image-row.selected {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .image-row.mounted .name {
    font-weight: 600;
  }
  .icon {
    flex-shrink: 0;
    color: var(--gs-fg-muted);
    display: inline-flex;
    align-items: center;
  }
  .name {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .badge {
    background: var(--gs-badge-bg, rgba(35, 134, 54, 0.25));
    color: var(--gs-badge-fg, #4ac26b);
    border-radius: 9999px;
    padding: 0 8px;
    height: 16px;
    line-height: 16px;
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .desc {
    color: var(--gs-fg-muted);
    font-size: 12px;
    flex-shrink: 0;
  }
</style>
