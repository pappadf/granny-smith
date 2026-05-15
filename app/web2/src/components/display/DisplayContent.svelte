<script lang="ts">
  import { machine } from '@/state/machine.svelte';
  import WelcomeView from './WelcomeView.svelte';
  import DropOverlay from './DropOverlay.svelte';

  // Phase 1 only renders the Welcome view. ScreenView arrives in Phase 3
  // when bus/emulator.ts wires the real Emscripten canvas.
  const showWelcome = $derived(machine.status === 'no-machine');
</script>

<div class="gs-display-content">
  {#if showWelcome}
    <WelcomeView />
  {/if}
  <DropOverlay />
</div>

<style>
  .gs-display-content {
    flex: 1 1 auto;
    min-height: 0;
    position: relative;
    overflow: hidden;
    background: var(--gs-bg);
  }
</style>
