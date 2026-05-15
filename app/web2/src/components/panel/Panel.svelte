<script lang="ts">
  import { layout } from '@/state/layout.svelte';
  import PanelHeader from './PanelHeader.svelte';
  import TerminalView from '../panel-views/terminal/TerminalView.svelte';
  import MachineView from '../panel-views/machine/MachineView.svelte';
  import FilesystemView from '../panel-views/filesystem/FilesystemView.svelte';
  import ImagesView from '../panel-views/images/ImagesView.svelte';
  import CheckpointsView from '../panel-views/checkpoints/CheckpointsView.svelte';
  import DebugView from '../panel-views/debug/DebugView.svelte';
  import LogsView from '../panel-views/logs/LogsView.svelte';
</script>

<aside class="gs-panel">
  <PanelHeader />
  <div class="gs-panel-content">
    {#if layout.activeTab === 'terminal'}<TerminalView />
    {:else if layout.activeTab === 'machine'}<MachineView />
    {:else if layout.activeTab === 'filesystem'}<FilesystemView />
    {:else if layout.activeTab === 'images'}<ImagesView />
    {:else if layout.activeTab === 'checkpoints'}<CheckpointsView />
    {:else if layout.activeTab === 'debug'}<DebugView />
    {:else if layout.activeTab === 'logs'}<LogsView />
    {/if}
  </div>
</aside>

<style>
  .gs-panel {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    background: var(--gs-bg);
    overflow: hidden;
    min-width: 0;
    min-height: 0;
  }
  .gs-panel-content {
    flex: 1 1 auto;
    min-height: 0;
    min-width: 0;
    position: relative;
    overflow: hidden;
  }
</style>
