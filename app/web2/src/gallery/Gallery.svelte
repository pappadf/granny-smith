<script lang="ts">
  import type { Component } from 'svelte';
  import { STORIES, findStory, type StoryProps } from './registry';
  import GalleryIndex from './GalleryIndex.svelte';
  import {
    appearance,
    applyAppearance,
    applyUrlSkin,
    skinReady,
    type SchemeMode,
  } from '@/state/appearance.svelte';

  // ?gallery[&story=<name>&variant=<v>][&theme=dark|light][&skin=<id>]
  const params = new URLSearchParams(window.location.search);
  const story = findStory(params.get('story'));
  const variant = params.get('variant') ?? story?.variants[0] ?? 'default';
  const themeParam = params.get('theme');
  const scheme: SchemeMode = themeParam === 'light' || themeParam === 'dark' ? themeParam : 'dark';
  appearance.schemeMode = scheme;
  applyUrlSkin(params);
  applyAppearance();
  if (params.get('motion') === 'reduced')
    document.documentElement.classList.add('gs-gallery-reduced-motion');

  // The list the screenshot spec walks.
  (window as unknown as { __gsGallery?: unknown }).__gsGallery = {
    stories: STORIES.map((s) => ({
      name: s.name,
      variants: s.variants,
      width: s.width,
      height: s.height,
      hover: s.hover ?? {},
      focus: s.focus ?? {},
    })),
  };

  let StoryComponent = $state<Component<StoryProps> | null>(null);
  let loadError = $state<string | null>(null);

  // Load the story, then mark the page ready once it has rendered and settled.
  if (story) {
    story
      .load()
      .then(async (m) => {
        StoryComponent = m.default as Component<StoryProps>;
        await skinReady();
        await document.fonts.ready;
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
        // Let mount-time async work (failed bus calls, transitions) finish.
        await new Promise((r) => setTimeout(r, 300));
        document.body.dataset.galleryReady = '1';
      })
      .catch((e: unknown) => {
        loadError = e instanceof Error ? e.message : String(e);
        document.body.dataset.galleryReady = '1';
      });
  }
</script>

{#if story}
  <div
    class="gallery-stage"
    data-gallery-stage
    data-story={story.name}
    data-variant={variant}
    style="width: {story.width}px; height: {story.height}px;"
  >
    {#if loadError}
      <pre class="gallery-error">{loadError}</pre>
    {:else if StoryComponent}
      <svelte:boundary>
        <StoryComponent {variant} />
        {#snippet failed(error)}
          <pre class="gallery-error">{String(error)}</pre>
        {/snippet}
      </svelte:boundary>
    {/if}
  </div>
{:else}
  <GalleryIndex />
{/if}

<style>
  .gallery-stage {
    position: relative;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }
  .gallery-error {
    color: var(--gs-danger-fg);
    white-space: pre-wrap;
  }
  /* Reduced motion for this page and the stories it links to: every
     duration collapses, as the user preference does. */
  :global(html.gs-gallery-reduced-motion) {
    --gs-duration-instant: 0.01ms;
    --gs-duration-fast: 0.01ms;
    --gs-duration-quick: 0.01ms;
    --gs-duration-base: 0.01ms;
    --gs-duration-slow: 0.01ms;
    --gs-duration-slower: 0.01ms;
    --gs-duration-spin: 0.01ms;
    --gs-duration-pulse: 0.01ms;
    --gs-duration-indeterminate: 0.01ms;
  }
</style>
