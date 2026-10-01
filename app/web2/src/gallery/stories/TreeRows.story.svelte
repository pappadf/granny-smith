<script lang="ts">
  import TreeItem from '@/components/ui/TreeItem.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();
  const noop = () => undefined;
</script>

<div class="tree" role="tree" tabindex="0" data-variant={variant}>
  <TreeItem class="tree-row" label="opfs" icon="folder" depth={0} hasChildren open onclick={noop} />
  <TreeItem class="tree-row" label="images" icon="folder" depth={1} hasChildren open />
  <TreeItem class="tree-row" label="hd" icon="folder" depth={2} hasChildren loading />
  <TreeItem
    class="tree-row"
    label="System 7.5.3.dsk"
    icon="floppy"
    description="1.4 MB"
    depth={2}
    selected
  />
  <TreeItem
    class="tree-row"
    label="Dragged.dsk"
    icon="floppy"
    description="800 KB"
    depth={2}
    dragSource
  />
  <TreeItem class="tree-row" label="empty" icon="folder" depth={1} hasChildren open />
  <TreeItem depth={2} variant="placeholder" label="(empty)" />
  <TreeItem class="tree-row" label="upload" icon="folder" depth={1} hasChildren dropTarget />
  <TreeItem
    class="tree-row"
    label="a very long file name that is cut off at the edge of the panel.img"
    icon="file"
    depth={3}
  />
</div>

<style>
  .tree {
    display: flex;
    flex-direction: column;
    padding: var(--gs-space-1) 0;
  }
  .tree:focus-visible {
    outline: none;
  }
</style>
