<script lang="ts">
  import { layout } from '@/state/layout.svelte';
  import PaneSplit from '@/components/common/PaneSplit.svelte';
  import TerminalPane from './TerminalPane.svelte';
  import CommandBrowser from './CommandBrowser.svelte';

  // Spec §4.3.0: horizontal split when the panel docks at the bottom,
  // vertical when it docks left or right. Terminal goes left/top.
  const orientation = $derived<'horizontal' | 'vertical'>(
    layout.panelPos === 'bottom' ? 'horizontal' : 'vertical',
  );
  // Spec mins: 240/220 horizontal, 160/140 vertical.
  const minA = $derived(orientation === 'horizontal' ? 240 : 160);
  const minB = $derived(orientation === 'horizontal' ? 220 : 140);
</script>

<div class="terminal-view">
  <PaneSplit {orientation} defaultSizePct={60} {minA} {minB}>
    {#snippet paneA()}
      <TerminalPane />
    {/snippet}
    {#snippet paneB()}
      <div class="browser-host"><CommandBrowser /></div>
    {/snippet}
  </PaneSplit>
</div>

<style>
  .terminal-view {
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
  }
  .browser-host {
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
    background: var(--gs-bg);
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }
</style>
