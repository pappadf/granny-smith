<script lang="ts">
  import { onMount } from 'svelte';
  import { machine } from '@/state/machine.svelte';
  import { bootstrap } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';

  let canvas: HTMLCanvasElement | undefined = $state(undefined);

  // CSS-driven scaling. The canvas's intrinsic resolution (width/height
  // attributes) stays at the emulator's framebuffer dimensions; the CSS
  // width/height scale by zoom percentage. image-rendering: pixelated keeps
  // pixels sharp.
  const cssWidth = $derived(Math.round(machine.screen.width * (machine.zoom / 100)));
  const cssHeight = $derived(Math.round(machine.screen.height * (machine.zoom / 100)));

  // Input handling lives entirely on the worker side via Emscripten's
  // built-in proxied callbacks (emscripten_set_mousemove_callback("#screen",
  // …) etc., registered after transferControlToOffscreen). We deliberately
  // do NOT attach JS-side mouse/keyboard handlers here — every such
  // handler would issue a gsEval round-trip per event and saturate the
  // bridge queue, starving the worker's render tick. See app/web-legacy
  // for the same pattern.

  onMount(() => {
    if (!canvas) return;
    // Boot the Module on first canvas mount. Subsequent mounts (component
    // re-render via DisplayContent routing) are no-ops thanks to the
    // moduleReady guard.
    void bootstrap(canvas).catch((err) => {
      console.error('emulator bootstrap failed', err);
      showNotification('Emulator failed to start (see console)', 'error');
    });
  });
</script>

<div class="screen-view">
  <div class="screen-wrap">
    <!--
      Intrinsic width/height attributes are static (Plus default). The
      worker owns canvas resolution via emscripten_set_canvas_element_size
      ("#screen", w, h) after transferControlToOffscreen runs; once that
      transfer has happened the canvas's `width`/`height` properties can
      no longer be assigned from the main thread (InvalidStateError).
      Letting Svelte rebind them reactively from `machine.screen.*`
      throws when the worker reports a resize, which aborts the rest of
      that reactive batch — including the `style="…"` update below — so
      CSS scaling stops following the actual screen size. The CSS
      (style.width / style.height) is what drives layout and is safe
      to update reactively.
    -->
    <canvas
      id="screen"
      bind:this={canvas}
      tabindex="0"
      width="512"
      height="342"
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
    outline: none;
    /* Prevent OS touch-pan + page bounce on touch devices. */
    touch-action: none;
  }
</style>
