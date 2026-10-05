<script lang="ts">
  import ImagesView from '@/components/panel-views/images/ImagesView.svelte';
  import ImageRow from '@/components/panel-views/images/ImageRow.svelte';

  // The view over the fixture OPFS, plus rows in each state.
</script>

<div class="rows">
  <ImageRow name="System 7.5.3.dsk" icon="floppy" desc="1.4 MB" />
  <ImageRow name="hd1.img" icon="hd" desc="40 MB" badge="Mounted" />
  <ImageRow name="Tools.dsk" icon="floppy" desc="800 KB" selected badge="Inserted · Drive 2" />
</div>
<div class="view"><ImagesView /></div>

<style>
  .rows {
    display: flex;
    flex-direction: column;
    padding: var(--gs-space-1) 0;
    border-bottom: var(--gs-border-width) solid var(--gs-border);
  }
  .view {
    flex: 1 1 auto;
    min-height: 0;
  }
</style>
