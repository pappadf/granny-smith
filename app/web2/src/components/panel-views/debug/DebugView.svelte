<script lang="ts">
  import PaneSplit from '@/components/common/PaneSplit.svelte';
  import DisassemblyPane from './DisassemblyPane.svelte';
  import SectionsPane from './SectionsPane.svelte';
  import { layout } from '@/state/layout.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug } from '@/state/debug.svelte';
  import { refreshDebugFrame, clearDebugFrame } from '@/state/debugFrame.svelte';

  // One debug.frame per pause and per step, shared by every pane
  // (state/debugFrame.svelte.ts).  Nothing is fetched while running: the
  // frame would be stale before it arrived, and the panes say "pause".
  $effect(() => {
    void debug.refreshGen;
    if (machine.status === 'paused') void refreshDebugFrame();
    else if (machine.status !== 'running') clearDebugFrame();
  });

  // Split orientation:
  //   panel-bottom → horizontal split, Sections LEFT, Disassembly RIGHT
  //   panel-left/right → vertical split, Disassembly TOP, Sections BOTTOM
  const orientation = $derived<'horizontal' | 'vertical'>(
    layout.panelPos === 'bottom' ? 'horizontal' : 'vertical',
  );
  // In horizontal mode pane A = Sections (left). In vertical mode pane
  // A = Disassembly (top). Both keep Disassembly adjacent to the
  // PanelHeader's Debug toolbar.
  const sectionsOnA = $derived(orientation === 'horizontal');
  const defaultSizePct = $derived(orientation === 'horizontal' ? 45 : 50);
  const minA = $derived(orientation === 'horizontal' ? 180 : 160);
  const minB = $derived(orientation === 'horizontal' ? 200 : 160);
</script>

<div class="debug-view">
  <PaneSplit {orientation} {defaultSizePct} {minA} {minB}>
    {#snippet paneA()}
      {#if sectionsOnA}
        <SectionsPane />
      {:else}
        <DisassemblyPane />
      {/if}
    {/snippet}
    {#snippet paneB()}
      {#if sectionsOnA}
        <DisassemblyPane />
      {:else}
        <SectionsPane />
      {/if}
    {/snippet}
  </PaneSplit>
</div>

<style>
  .debug-view {
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
  }
</style>
