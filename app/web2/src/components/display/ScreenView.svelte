<script lang="ts">
  import { onMount } from 'svelte';
  import { machine } from '@/state/machine.svelte';
  import { paintMockScreen } from '@/lib/canvas';

  let canvas: HTMLCanvasElement | undefined = $state(undefined);

  onMount(() => {
    if (canvas) paintMockScreen(canvas);
  });

  // Mirror the prototype's zoom: width/height in CSS pixels scale linearly
  // from the canvas's logical 512x342, but the intrinsic resolution stays
  // fixed (image-rendering: pixelated keeps each pixel sharp).
  const cssWidth = $derived(Math.round(machine.screen.width * (machine.zoom / 100)));
  const cssHeight = $derived(Math.round(machine.screen.height * (machine.zoom / 100)));
</script>

<div class="screen-view">
  <div class="screen-wrap">
    <canvas
      bind:this={canvas}
      width={machine.screen.width}
      height={machine.screen.height}
      style="width: {cssWidth}px; height: {cssHeight}px"
    ></canvas>
  </div>
</div>

<style>
  .screen-view {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: auto;
  }
  .screen-wrap {
    position: relative;
    background: var(--gs-screen-bg);
    box-shadow: 0 0 0 1px rgba(0, 0, 0, 0.8);
  }
  canvas {
    display: block;
    image-rendering: pixelated;
    image-rendering: crisp-edges;
  }
</style>
