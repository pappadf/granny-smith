<script lang="ts">
  import WelcomeHomeSlide from './WelcomeHomeSlide.svelte';

  // Phase 1 only ships the Home slide. The Configuration slide arrives in
  // Phase 2; both will coexist behind a slide-stack with 250 ms transitions.
</script>

<div class="welcome-view">
  <div class="welcome-slides">
    <WelcomeHomeSlide />
  </div>
</div>

<style>
  .welcome-view {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: stretch;
    justify-content: center;
    overflow: hidden;
    color: var(--gs-fg);
    font-size: 13px;
    line-height: 16px;
  }
  .welcome-slides {
    width: 100%;
    height: 100%;
    position: relative;
  }
</style>
