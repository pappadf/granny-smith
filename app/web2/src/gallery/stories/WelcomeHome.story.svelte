<script lang="ts">
  import WelcomeView from '@/components/display/WelcomeView.svelte';
  import { layout } from '@/state/layout.svelte';

  layout.welcomeSlide = 'home';
</script>

<div class="layer"><WelcomeView /></div>

<style>
  .layer {
    position: absolute;
    inset: 0;
    background: var(--gs-surface-app);
  }
</style>
