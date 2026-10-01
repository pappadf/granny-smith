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
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<div
  class="tree-row"
  class:selected
  class:drag-source={dragSource}
  class:drop-target={dropTarget}
  style="--depth: {depth}"
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
    {#if hasChildren}<Icon name="chevron" size={12} />{/if}
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
    padding-left: calc(var(--gs-tree-indent-base) + var(--gs-tree-indent) * var(--depth));
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    height: var(--gs-size-row);
    padding-right: var(--gs-space-2);
    cursor: pointer;
    user-select: none;
    color: var(--gs-text);
    font-size: var(--gs-font-size-base);
  }
  .tree-row:hover {
    background: var(--gs-row-hover);
  }
  .tree-row.selected {
    background: var(--gs-row-selected);
  }
  .tree-row.drag-source {
    opacity: 0.45;
  }
  .tree-row.drop-target {
    outline: var(--gs-focus-width) solid var(--gs-drop-border);
    outline-offset: var(--gs-focus-offset);
    background: var(--gs-tree-drop-bg);
  }
  .twistie {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: var(--gs-size-icon-md);
    color: var(--gs-text-muted);
    flex-shrink: 0;
    /* Chevron points down when open, rotates to point right when
       closed. Same pattern as the section twistie. */
    transform: rotate(-90deg);
    transition: transform var(--gs-duration-instant) var(--gs-ease-out);
  }
  .twistie.open {
    transform: rotate(0deg);
  }
  .twistie:not(.has) {
    visibility: hidden;
  }
  .icon {
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    color: var(--gs-text-muted);
  }
  .label {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .desc {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-sm);
    flex-shrink: 0;
    margin-left: var(--gs-space-2);
  }
</style>
