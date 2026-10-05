<script lang="ts">
  import Badge from '@/components/ui/Badge.svelte';
  import Icon from '@/components/common/Icon.svelte';
  import ListRow from '@/components/ui/ListRow.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props {
    name: string;
    desc?: string;
    icon: IconName;
    /** Mount badge text (e.g. "Inserted", "Inserted · Drive 2", "Mounted").
     *  Null/empty when the image isn't mounted. */
    badge?: string | null;
    selected?: boolean;
    onClick?: (ev: MouseEvent) => void;
    onDoubleClick?: (ev: MouseEvent) => void;
    onContextMenu?: (ev: MouseEvent) => void;
  }
  let {
    name,
    desc,
    icon,
    badge = null,
    selected = false,
    onClick,
    onDoubleClick,
    onContextMenu,
  }: Props = $props();
</script>

<ListRow
  class="image-row {badge ? 'mounted' : ''}"
  indent
  {selected}
  role="row"
  tabindex={-1}
  onclick={onClick}
  ondblclick={onDoubleClick}
  oncontextmenu={onContextMenu}
>
  <span class="icon"><Icon name={icon} size="base" /></span>
  <span class="name" class:mounted={!!badge}>{name}</span>
  {#if badge}
    <Badge class="badge" intent="success">{badge}</Badge>
  {/if}
  {#if desc}
    <span class="desc">{desc}</span>
  {/if}
</ListRow>

<style>
  .name.mounted {
    font-weight: var(--gs-font-weight-semibold);
  }
  .icon {
    flex-shrink: 0;
    color: var(--gs-text-muted);
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
  .desc {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-sm);
    flex-shrink: 0;
  }
</style>
