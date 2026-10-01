<script lang="ts">
  import { machine } from '@/state/machine.svelte';
  import WelcomeView from './WelcomeView.svelte';
  import UrlBootView from './UrlBootView.svelte';
  import { urlBoot, dismissUrlBoot } from '@/state/urlBoot.svelte';
  import ScreenView from './ScreenView.svelte';
  import DropOverlay from './DropOverlay.svelte';

  // After Shut Down, machine.status === 'stopped' and the user is back on
  // Welcome (so they can pick a new config). The StatusBar stays visible —
  // handled in StatusBar.svelte, not here.
  //
  // Layering note: ScreenView is always mounted so bus.emulator.bootstrap()
  // can hand Emscripten a stable canvas reference at page load. Welcome
  // sits on top until a machine is running.
  const idle = $derived(machine.status === 'no-machine' || machine.status === 'stopped');
  // A page opened to boot from its URL shows the download progress in
  // Welcome's place until the machine runs; after that (a later Shut Down)
  // it is the ordinary Welcome again.
  const showUrlBoot = $derived(idle && urlBoot.showProgress);
  const showWelcome = $derived(idle && !urlBoot.showProgress);
  $effect(() => {
    if (!idle && urlBoot.showProgress) dismissUrlBoot();
  });
</script>

<div class="gs-display-content">
  <ScreenView />
  {#if showWelcome}
    <div class="welcome-layer">
      <WelcomeView />
    </div>
  {:else if showUrlBoot}
    <div class="welcome-layer url-boot-layer">
      <UrlBootView />
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
    background: var(--gs-surface-app);
  }
  .welcome-layer {
    position: absolute;
    inset: 0;
    background: var(--gs-surface-app);
    z-index: 10;
  }
</style>
