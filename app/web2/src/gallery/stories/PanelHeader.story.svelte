<script lang="ts">
  import PanelHeader from '@/components/panel/PanelHeader.svelte';
  import { layout, type PanelTab } from '@/state/layout.svelte';
  import { machine } from '@/state/machine.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  layout.activeTab = variant as PanelTab;
  machine.status = 'paused';
</script>

<div class="panel"><PanelHeader /></div>

<style>
  .panel {
    display: flex;
    flex-direction: column;
    background: var(--gs-surface-app);
  }
</style>
