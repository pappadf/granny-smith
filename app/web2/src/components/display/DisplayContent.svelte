<script lang="ts">
  import { machine } from '@/state/machine.svelte';
  import WelcomeView from './WelcomeView.svelte';
  import ScreenView from './ScreenView.svelte';
  import DropOverlay from './DropOverlay.svelte';

  // After a Shut Down, machine.status === 'stopped' and the user is back on
  // the Welcome view (so they can pick a new config). The StatusBar stays
  // visible (spec §11) — that's handled in StatusBar.svelte, not here.
  const showWelcome = $derived(machine.status === 'no-machine' || machine.status === 'stopped');
</script>

<div class="gs-display-content">
  {#if showWelcome}
    <WelcomeView />
  {:else}
    <ScreenView />
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
