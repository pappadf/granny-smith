<script lang="ts">
  import { layout } from '@/state/layout.svelte';
  import WelcomeHomeSlide from './WelcomeHomeSlide.svelte';
  import WelcomeConfigSlide from './WelcomeConfigSlide.svelte';
</script>

<div class="welcome-view">
  <div class="welcome-slides">
    <div class="welcome-slide" class:active={layout.welcomeSlide === 'home'}>
      <WelcomeHomeSlide />
    </div>
    <div class="welcome-slide" class:active={layout.welcomeSlide === 'configuration'}>
      <WelcomeConfigSlide />
    </div>
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
  /* Slide stack — spec §3.5: 250 ms ease-out opacity + 10 px translateX. */
  .welcome-slide {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: flex-start;
    justify-content: center;
    overflow: auto;
    opacity: 0;
    transform: translateX(10px);
    pointer-events: none;
    transition:
      opacity 250ms ease-out,
      transform 250ms ease-out;
  }
  .welcome-slide.active {
    opacity: 1;
    transform: none;
    pointer-events: auto;
  }
</style>
