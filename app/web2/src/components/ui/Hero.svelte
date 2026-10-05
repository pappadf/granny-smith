<script lang="ts">
  // The large title and subtitle at the top of the welcome and URL-boot
  // screens.  `titleClass` / `subtitleClass` carry the legacy hooks
  // (welcome-title / title, welcome-subtitle / subtitle).
  interface Props {
    title: string;
    subtitle?: string;
    titleClass?: string;
    subtitleClass?: string;
  }
  let { title, subtitle, titleClass = '', subtitleClass = '' }: Props = $props();
</script>

<h1 class="gs-hero__title {titleClass}">{title}</h1>
{#if subtitle}<p class="gs-hero__subtitle {subtitleClass}">{subtitle}</p>{/if}

<style>
  .gs-hero__title {
    font-size: var(--gs-hero-title-size);
    font-weight: var(--gs-hero-title-weight);
    color: var(--gs-hero-title-fg);
    margin: 0 0 var(--gs-space-2) 0;
  }
  .gs-hero__subtitle {
    color: var(--gs-hero-subtitle-fg);
    margin: 0 0 var(--gs-space-7) 0;
    font-size: var(--gs-hero-subtitle-size);
  }
</style>
