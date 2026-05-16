<script lang="ts">
  import { machine } from '@/state/machine.svelte';
  import WelcomeView from './WelcomeView.svelte';
  import ScreenView from './ScreenView.svelte';
  import DropOverlay from './DropOverlay.svelte';

  // After Shut Down, machine.status === 'stopped' and the user is back on
  // Welcome (so they can pick a new config). The StatusBar stays visible
  // (spec §11) — handled in StatusBar.svelte, not here.
  //
  // Layering note: ScreenView is always mounted so bus.emulator.bootstrap()
  // can hand Emscripten a stable canvas reference at page load. Welcome
  // sits on top until a machine is running.
  const showWelcome = $derived(machine.status === 'no-machine' || machine.status === 'stopped');
</script>

<div class="gs-display-content">
  <ScreenView />
  {#if showWelcome}
    <div class="welcome-layer">
      <WelcomeView />
    </div>
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
  .welcome-layer {
    position: absolute;
    inset: 0;
    background: var(--gs-bg);
    z-index: 10;
  }
</style>
