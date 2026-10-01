<script lang="ts">
  import type { Component } from 'svelte';
  import { STORIES, findStory, type StoryProps } from './registry';
  import {
    appearance,
    applyAppearance,
    applyUrlSkin,
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

  // A page URL for a story variant in a scheme.
  function href(name: string, v: string, t: string): string {
    return `?gallery&story=${encodeURIComponent(name)}&variant=${encodeURIComponent(v)}&theme=${t}`;
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
  <main class="gallery-index">
    <h1>Granny Smith UI gallery</h1>
    <p>Every story and variant, in both schemes. Development builds only.</p>
    <table>
      <thead>
        <tr><th>Story</th><th>Variant</th><th>Dark</th><th>Light</th></tr>
      </thead>
      <tbody>
        {#each STORIES as s (s.name)}
          {#each s.variants as v (v)}
            <tr>
              <td>{s.name}</td>
              <td>{v}</td>
              <td><a href={href(s.name, v, 'dark')}>dark</a></td>
              <td><a href={href(s.name, v, 'light')}>light</a></td>
            </tr>
          {/each}
        {/each}
      </tbody>
    </table>
  </main>
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
  .gallery-index {
    padding: var(--gs-space-4) var(--gs-space-6);
    height: 100%;
    overflow: auto;
  }
  .gallery-index table {
    border-collapse: collapse;
  }
  .gallery-index td,
  .gallery-index th {
    padding: var(--gs-space-0-5) var(--gs-space-3) var(--gs-space-0-5) 0;
    text-align: left;
  }
  .gallery-index a {
    color: var(--gs-text-link);
  }
</style>
