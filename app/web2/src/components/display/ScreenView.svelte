<script lang="ts">
  import { onMount } from 'svelte';
  import { machine } from '@/state/machine.svelte';
  import { bootstrap, gsEval, isModuleReady } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';

  let canvas: HTMLCanvasElement | undefined = $state(undefined);

  // CSS-driven scaling. The canvas's intrinsic resolution (width/height
  // attributes) stays at the emulator's framebuffer dimensions; the CSS
  // width/height scale by zoom percentage. image-rendering: pixelated keeps
  // pixels sharp.
  const cssWidth = $derived(Math.round(machine.screen.width * (machine.zoom / 100)));
  const cssHeight = $derived(Math.round(machine.screen.height * (machine.zoom / 100)));

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

  // ----- Input forwarding -----
  // Mouse: emit position + button events to input.mouse.* via gsEval.
  // Keyboard: focus the canvas, forward keydown/keyup to input.keyboard.*.
  // These shell-line paths exist on the C side post-shell-as-object-model
  // proposal; the exact method names may shift — keep gsEval calls as the
  // single seam so updates land in one place.

  function rectScale(ev: MouseEvent): { x: number; y: number } {
    if (!canvas) return { x: 0, y: 0 };
    const rect = canvas.getBoundingClientRect();
    // Map CSS pixels back to the canvas's logical resolution.
    const scaleX = machine.screen.width / rect.width;
    const scaleY = machine.screen.height / rect.height;
    return {
      x: Math.round((ev.clientX - rect.left) * scaleX),
      y: Math.round((ev.clientY - rect.top) * scaleY),
    };
  }

  function onMouseMove(ev: MouseEvent) {
    if (!isModuleReady()) return;
    const { x, y } = rectScale(ev);
    void gsEval('input.mouse.move', [x, y]);
  }

  function onMouseDown(ev: MouseEvent) {
    if (!isModuleReady()) return;
    ev.preventDefault();
    canvas?.focus();
    void gsEval('input.mouse.button', ['down']);
  }

  function onMouseUp(ev: MouseEvent) {
    if (!isModuleReady()) return;
    ev.preventDefault();
    void gsEval('input.mouse.button', ['up']);
  }

  function onKeyDown(ev: KeyboardEvent) {
    if (!isModuleReady()) return;
    ev.preventDefault();
    void gsEval('input.keyboard.down', [ev.code]);
  }

  function onKeyUp(ev: KeyboardEvent) {
    if (!isModuleReady()) return;
    ev.preventDefault();
    void gsEval('input.keyboard.up', [ev.code]);
  }
</script>

<div class="screen-view">
  <div class="screen-wrap">
    <canvas
      id="screen"
      bind:this={canvas}
      tabindex="0"
      width={machine.screen.width}
      height={machine.screen.height}
      style="width: {cssWidth}px; height: {cssHeight}px"
      onmousemove={onMouseMove}
      onmousedown={onMouseDown}
      onmouseup={onMouseUp}
      onkeydown={onKeyDown}
      onkeyup={onKeyUp}
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
