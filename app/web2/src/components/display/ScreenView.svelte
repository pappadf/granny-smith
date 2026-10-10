<script lang="ts">
  import { onMount } from 'svelte';
  import { machine, fitZoom } from '@/state/machine.svelte';
  import { bootstrap } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';
  import { startVoodooGpu, gpuOverlay } from '@/gpu/voodoo2Gpu.svelte';

  let canvas: HTMLCanvasElement | undefined = $state(undefined);
  // The scroll area the screen sits in, and its size, for fitting the zoom.
  let view: HTMLDivElement | undefined = $state(undefined);
  let viewW = $state(0);
  let viewH = $state(0);
  // The Voodoo2 WebGPU takeover's overlay: transferred to the GPU worker
  // once at mount and shown exactly while the card drives the monitor in
  // GPU mode, so the pass-through switch is literally which canvas is on
  // top.  It takes no pointer events — input stays on #screen, where
  // Emscripten's proxied handlers live.
  let canvas3d: HTMLCanvasElement | undefined = $state(undefined);

  // CSS-driven scaling. The canvas's intrinsic resolution (width/height
  // attributes) stays at the emulator's framebuffer dimensions; the CSS
  // width/height scale by zoom percentage. image-rendering: pixelated keeps
  // pixels sharp.
  //
  // Non-square pixels: the core reports a pixel aspect ratio (parW:parH = the
  // display pixel's width:height). Horizontal scale stays at the zoom factor;
  // the vertical scale is multiplied by parH/parW so each source pixel occupies
  // its true shape. Square pixels (1:1) leave height unchanged; the Lisa 2 (2:3)
  // renders each pixel 1.5x taller — an exact 2x3 host block at the 200% default.
  const cssWidth = $derived(Math.round(machine.screen.width * (machine.zoom / 100)));
  const cssHeight = $derived(
    Math.round(
      machine.screen.height * (machine.zoom / 100) * (machine.screen.parH / machine.screen.parW),
    ),
  );

  // Fit the zoom to the display area until the user picks one (state/
  // machine.svelte.ts fitZoom): on every new screen size and every resize of
  // the area.  The frame's bezel (padding) is room the picture cannot use.
  let wrap: HTMLDivElement | undefined = $state(undefined);
  // The area's size, followed with a ResizeObserver where there is one
  // (not under the unit tests' jsdom).
  $effect(() => {
    if (!view) return;
    const el = view;
    const measure = () => {
      viewW = el.clientWidth;
      viewH = el.clientHeight;
    };
    measure();
    if (typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  });
  $effect(() => {
    const w = machine.screen.width;
    const h = machine.screen.height;
    const aspect = machine.screen.parH / machine.screen.parW;
    // Only a machine's screen is fitted: with none, the display is the
    // Welcome view's and the zoom stays as it is.
    const live = machine.status === 'running' || machine.status === 'paused';
    if (!view || !wrap || !live) return;
    const cs = getComputedStyle(wrap);
    const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
    const padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    fitZoom(w, h, aspect, viewW - padX, viewH - padY);
  });

  // Input handling lives entirely on the worker side via Emscripten's
  // built-in proxied callbacks (emscripten_set_mousemove_callback("#screen",
  // …) etc., registered after transferControlToOffscreen). We deliberately
  // do NOT attach JS-side handlers that talk to the core — every such
  // handler would issue a gsEval round-trip per event and saturate the
  // bridge queue, starving the worker's render tick. See app/web-legacy
  // for the same pattern.
  //
  // The one exception is the grab itself.  Pointer lock needs the click's
  // user gesture, and Safari honours it only on the main thread, while the
  // click is being handled (or in a message the worker posts while handling
  // the one that carried it -- which Emscripten's batched proxying does not
  // guarantee).  A request made from the worker's mousedown callback was
  // refused there, so the click asks here; the core follows the lock through
  // its pointerlockchange callback.  A paused or stopped machine reads no
  // deltas, so it is not grabbed.
  function grab(): void {
    if (!canvas || machine.status !== 'running' || document.pointerLockElement === canvas) return;
    try {
      // A promise where the browser has one; a refusal also fires
      // pointerlockerror, and the click simply does not grab.
      const p = canvas.requestPointerLock() as unknown as Promise<void> | undefined;
      p?.catch?.(() => undefined);
    } catch {
      // Not supported: nothing to grab with.
    }
  }

  onMount(() => {
    if (!canvas) return;
    // The GPU worker starts before the module so its answer (a device or
    // not) is in the bridge before any machine can boot.
    if (canvas3d) void startVoodooGpu(canvas3d);
    // Boot the Module on first canvas mount. Subsequent mounts (component
    // re-render via DisplayContent routing) are no-ops thanks to the
    // moduleReady guard.
    void bootstrap(canvas).catch((err) => {
      console.error('emulator bootstrap failed', err);
      showNotification(
        `Emulator failed to start: ${err instanceof Error ? err.message : err}`,
        'error',
      );
    });
  });
</script>

<div class="screen-view" bind:this={view}>
  <div class="screen-wrap" bind:this={wrap}>
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
    <!-- role="application": a focusable surface that passes every key and
         pointer event to the emulated machine, so assistive tech should not
         intercept them; it is named for what it is. -->
    <!-- svelte-ignore a11y_no_interactive_element_to_noninteractive_role -->
    <canvas
      id="screen"
      bind:this={canvas}
      tabindex="0"
      role="application"
      aria-label="Emulated machine screen"
      onmousedown={grab}
      width="512"
      height="342"
      style="width: {cssWidth}px; height: {cssHeight}px"
    ></canvas>
    <canvas
      id="screen3d"
      class="overlay"
      aria-hidden="true"
      bind:this={canvas3d}
      width="640"
      height="480"
      hidden={!gpuOverlay.visible}
      style="width: {cssWidth}px; height: {cssHeight}px"
    ></canvas>
  </div>
</div>

<style>
  /* Inset by the skin's --gs-display-inset, so a rounded display never cuts
     into a screen too large to fit (it scrolls within straight edges).
     The frame is centred by its own auto margins, not by the flex
     container's alignment: centring an oversized child puts half its
     overflow at negative offsets, above and left of the scroll origin,
     where scrolling never reaches (#279).  Auto margins centre while it
     fits and collapse to 0 once it does not. */
  .screen-view {
    position: absolute;
    inset: var(--gs-display-inset);
    display: flex;
    overflow: auto;
  }
  /* The frame around the picture: a skin may give it a bezel (padding),
     rounded corners and its own shadow.  The canvases themselves are never
     styled (lint L-9). */
  .screen-wrap {
    position: relative;
    margin: auto;
    flex: none;
    background: var(--gs-screen-frame-bg);
    box-shadow: var(--gs-screen-frame-shadow);
    padding: var(--gs-screen-frame-padding);
    border-radius: var(--gs-screen-frame-radius);
  }
  canvas {
    display: block;
    image-rendering: pixelated;
    image-rendering: crisp-edges;
    /* Prevent OS touch-pan + page bounce on touch devices. */
    touch-action: none;
  }
  /* Keyboard focus on the emulated screen shows on its frame, not as a
     ring drawn over the picture. */
  canvas:focus-visible {
    outline: none;
  }
  .screen-wrap:has(canvas:focus-visible) {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-width);
  }
  canvas.overlay {
    position: absolute;
    left: var(--gs-screen-frame-padding);
    top: var(--gs-screen-frame-padding);
    pointer-events: none;
  }
  canvas.overlay[hidden] {
    display: none;
  }
</style>
