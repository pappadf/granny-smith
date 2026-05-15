<script lang="ts">
  import { layout } from '@/state/layout.svelte';
  import DisplayToolbar from '../display/DisplayToolbar.svelte';
  import DisplayContent from '../display/DisplayContent.svelte';
  import Panel from '../panel/Panel.svelte';
  import WorkbenchSash from './WorkbenchSash.svelte';

  const panelClass = $derived(`panel-${layout.panelPos}`);
  const panelSizeVar = $derived(`${layout.panelSize[layout.panelPos]}px`);
</script>

<div
  class="gs-workbench {panelClass}"
  class:panel-collapsed={layout.panelCollapsed}
  style="--gs-panel-size: {panelSizeVar}"
>
  <section class="gs-display">
    <DisplayToolbar />
    <DisplayContent />
  </section>
  <WorkbenchSash />
  <Panel />
</div>

<style>
  .gs-workbench {
    flex: 1;
    min-height: 0;
    display: flex;
    position: relative;
    --gs-panel-size: 280px;
  }
  .gs-workbench.panel-bottom {
    flex-direction: column;
  }
  .gs-workbench.panel-left,
  .gs-workbench.panel-right {
    flex-direction: row;
  }
  /* Panel ordering — see prototype styles.css:216-226. */
  .gs-workbench.panel-left > :global(.gs-panel) {
    order: 0;
  }
  .gs-workbench.panel-left > :global(.workbench-sash) {
    order: 1;
  }
  .gs-workbench.panel-left > .gs-display {
    order: 2;
  }
  .gs-workbench.panel-right > .gs-display {
    order: 0;
  }
  .gs-workbench.panel-right > :global(.workbench-sash) {
    order: 1;
  }
  .gs-workbench.panel-right > :global(.gs-panel) {
    order: 2;
  }
  .gs-workbench.panel-bottom > .gs-display {
    order: 0;
  }
  .gs-workbench.panel-bottom > :global(.workbench-sash) {
    order: 1;
  }
  .gs-workbench.panel-bottom > :global(.gs-panel) {
    order: 2;
  }
  .gs-display {
    flex: 1 1 auto;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    background: var(--gs-bg);
    position: relative;
  }
  /* Panel sizing rules apply to a child component with the .gs-panel class. */
  .gs-workbench.panel-bottom > :global(.gs-panel) {
    height: var(--gs-panel-size);
    width: 100%;
    border-top: 1px solid var(--gs-border);
  }
  .gs-workbench.panel-left > :global(.gs-panel) {
    width: var(--gs-panel-size);
    height: 100%;
    border-right: 1px solid var(--gs-border);
  }
  .gs-workbench.panel-right > :global(.gs-panel) {
    width: var(--gs-panel-size);
    height: 100%;
    border-left: 1px solid var(--gs-border);
  }
  .gs-workbench.panel-collapsed.panel-bottom > :global(.gs-panel) {
    height: 35px;
  }
  .gs-workbench.panel-collapsed.panel-left > :global(.gs-panel),
  .gs-workbench.panel-collapsed.panel-right > :global(.gs-panel) {
    width: 0;
    border: 0;
  }
</style>
