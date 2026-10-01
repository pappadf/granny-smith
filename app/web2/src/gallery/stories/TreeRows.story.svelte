<script lang="ts">
  import TreeRow from '@/components/common/TreeRow.svelte';

  const noop = () => undefined;
</script>

<div class="tree" role="tree">
  <TreeRow
    label="opfs"
    icon="folder"
    depth={0}
    hasChildren
    open
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="images"
    icon="folder"
    depth={1}
    hasChildren
    open
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="hd"
    icon="folder"
    depth={2}
    hasChildren
    open={false}
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="System 7.5.3.dsk"
    icon="floppy"
    desc="1.4 MB"
    depth={2}
    hasChildren={false}
    open={false}
    selected
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="Dragged.dsk"
    icon="floppy"
    desc="800 KB"
    depth={2}
    hasChildren={false}
    open={false}
    dragSource
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="upload"
    icon="folder"
    depth={1}
    hasChildren
    open={false}
    dropTarget
    onClick={noop}
    onTwistieClick={noop}
  />
  <TreeRow
    label="a very long file name that is cut off at the edge of the panel.img"
    icon="file"
    depth={3}
    hasChildren={false}
    open={false}
    onClick={noop}
    onTwistieClick={noop}
  />
</div>

<style>
  .tree {
    display: flex;
    flex-direction: column;
    padding: var(--gs-space-1) 0;
  }
</style>
