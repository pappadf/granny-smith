<script lang="ts">
  import PaneSplit from '@/components/common/PaneSplit.svelte';
  import WorkbenchSash from '@/components/workbench/WorkbenchSash.svelte';
</script>

<div class="split">
  <PaneSplit orientation="horizontal" defaultSizePct={50} minA={50} minB={50}>
    {#snippet paneA()}<p class="pane">Pane A</p>{/snippet}
    {#snippet paneB()}<p class="pane">Pane B</p>{/snippet}
  </PaneSplit>
</div>
<div class="sash"><WorkbenchSash /></div>

<style>
  .split {
    height: 200px;
    border-bottom: var(--gs-border-width) solid var(--gs-border);
  }
  .sash {
    position: relative;
    flex: 1 1 auto;
    display: flex;
    flex-direction: column;
  }
  .pane {
    margin: var(--gs-space-2);
  }
</style>
