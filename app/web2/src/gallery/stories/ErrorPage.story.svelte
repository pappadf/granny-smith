<script lang="ts">
  import { renderWebGLErrorPage, renderStartupErrorPage } from '@/lib/webglErrorPage';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();
  let host = $state<HTMLDivElement | null>(null);

  $effect(() => {
    if (!host) return;
    if (variant === 'webgl')
      renderWebGLErrorPage(host, {
        ok: false,
        failure: 'context-lost',
        detail: 'getParameter(VIEWPORT) returned null (renderer: Apple GPU)',
      });
    else renderStartupErrorPage(host, 'the emulator worker did not start within 30 s');
  });
</script>

<div class="host" bind:this={host}></div>

<style>
  .host {
    position: absolute;
    inset: 0;
  }
</style>
