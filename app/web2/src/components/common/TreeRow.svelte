<script lang="ts">
  import Icon from './Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props {
    label: string;
    icon?: IconName;
    desc?: string;
    depth: number;
    hasChildren: boolean;
    open: boolean;
    selected?: boolean;
    draggable?: boolean;
    dragSource?: boolean;
    dropTarget?: boolean;
    onClick: (ev: MouseEvent) => void;
    onTwistieClick: (ev: MouseEvent) => void;
    onContextMenu?: (ev: MouseEvent) => void;
    onDoubleClick?: (ev: MouseEvent) => void;
    onDragStart?: (ev: DragEvent) => void;
    onDragOver?: (ev: DragEvent) => void;
    onDragLeave?: (ev: DragEvent) => void;
    onDragEnd?: (ev: DragEvent) => void;
    onDrop?: (ev: DragEvent) => void;
  }
  let {
    label,
    icon,
    desc,
    depth,
    hasChildren,
    open,
    selected = false,
    draggable = false,
    dragSource = false,
    dropTarget = false,
    onClick,
    onTwistieClick,
    onContextMenu,
    onDoubleClick,
    onDragStart,
    onDragOver,
    onDragLeave,
    onDragEnd,
    onDrop,
  }: Props = $props();

  const paddingLeft = $derived(8 + depth * 8);
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<div
  class="tree-row"
  class:selected
  class:drag-source={dragSource}
  class:drop-target={dropTarget}
  style="padding-left: {paddingLeft}px;"
  role="treeitem"
  aria-selected={selected}
  aria-expanded={hasChildren ? open : undefined}
  tabindex="-1"
  {draggable}
  onclick={onClick}
  ondblclick={onDoubleClick}
  oncontextmenu={onContextMenu}
  ondragstart={onDragStart}
  ondragover={onDragOver}
  ondragleave={onDragLeave}
  ondragend={onDragEnd}
  ondrop={onDrop}
>
  <span
    class="twistie"
    class:has={hasChildren}
    class:open
    onclick={onTwistieClick}
    role="button"
    tabindex="-1"
    aria-label={hasChildren ? (open ? 'Collapse' : 'Expand') : ''}
  >
    {hasChildren ? (open ? '▼' : '▶') : ''}
  </span>
  {#if icon}
    <span class="icon"><Icon name={icon} size={16} /></span>
  {/if}
  <span class="label">{label}</span>
  {#if desc}
    <span class="desc">{desc}</span>
  {/if}
</div>

<style>
  .tree-row {
    display: flex;
    align-items: center;
    gap: 4px;
    height: 22px;
    padding-right: 8px;
    cursor: pointer;
    user-select: none;
    color: var(--gs-fg);
    font-size: 13px;
  }
  .tree-row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .tree-row.selected {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .tree-row.drag-source {
    opacity: 0.45;
  }
  .tree-row.drop-target {
    outline: 1px solid var(--gs-drop-border, #0969da);
    outline-offset: -1px;
    background: rgba(9, 105, 218, 0.15);
  }
  .twistie {
    display: inline-block;
    width: 14px;
    text-align: center;
    color: var(--gs-fg-muted);
    font-size: 9px;
    flex-shrink: 0;
  }
  .twistie:not(.has) {
    visibility: hidden;
  }
  .icon {
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    color: var(--gs-fg-muted);
  }
  .label {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .desc {
    color: var(--gs-fg-muted);
    font-size: 12px;
    flex-shrink: 0;
    margin-left: 8px;
  }
</style>
