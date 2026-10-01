<script lang="ts">
  import WelcomeView from '@/components/display/WelcomeView.svelte';
  import { layout } from '@/state/layout.svelte';

  // Without an emulator the form waits on its ROM scan; the app-states
  // spec covers the filled-in form against a real core.
  layout.welcomeSlide = 'configuration';
</script>

<div class="layer"><WelcomeView /></div>

<style>
  .layer {
    position: absolute;
    inset: 0;
    background: var(--gs-bg);
  }
</style>
