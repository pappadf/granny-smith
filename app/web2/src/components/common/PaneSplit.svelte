<script lang="ts">
  import type { Snippet } from 'svelte';

  // Generic two-pane split with a draggable sash. Used by the Terminal
  // view (Phase 4) and the Debug view (Phase 6). Orientation comes from
  // the consumer — typically derived from layout.panelPos so the split
  // axis flips when the panel docks to the side.
  //
  //   horizontal: pane A on the left, pane B on the right, vertical sash
  //   vertical:   pane A on top,  pane B at the bottom, horizontal sash

  interface Props {
    orientation: 'horizontal' | 'vertical';
    defaultSizePct?: number; // initial size of pane A, 0–100
    minA?: number; // px
    minB?: number; // px
    paneA: Snippet;
    paneB: Snippet;
  }

  let { orientation, defaultSizePct = 60, minA = 160, minB = 140, paneA, paneB }: Props = $props();

  // Track size as a percentage so the split survives container resizes
  // without re-locking to an absolute pixel count. Captures defaultSizePct
  // once on mount — subsequent prop changes shouldn't reset a size the
  // user has dragged manually.
  // svelte-ignore state_referenced_locally
  let sizePctA = $state(defaultSizePct);
  let containerEl = $state<HTMLDivElement | null>(null);
  let dragging = $state(false);

  function totalPx(): number {
    if (!containerEl) return 0;
    const r = containerEl.getBoundingClientRect();
    return orientation === 'horizontal' ? r.width : r.height;
  }

  function clamp(pct: number): number {
    const total = totalPx();
    if (total <= 0) return Math.max(0, Math.min(100, pct));
    const aPx = (pct / 100) * total;
    const minAPct = (minA / total) * 100;
    const minBPct = ((total - minB) / total) * 100;
    if (aPx < minA) return minAPct;
    if (aPx > total - minB) return minBPct;
    return pct;
  }

  function onSashDown(ev: PointerEvent) {
    if (!containerEl) return;
    ev.preventDefault();
    dragging = true;
    const sash = ev.currentTarget as HTMLElement;
    sash.setPointerCapture(ev.pointerId);
    const onMove = (e: PointerEvent) => {
      if (!containerEl) return;
      const r = containerEl.getBoundingClientRect();
      const v = orientation === 'horizontal' ? e.clientX - r.left : e.clientY - r.top;
      const total = orientation === 'horizontal' ? r.width : r.height;
      if (total <= 0) return;
      sizePctA = clamp((v / total) * 100);
    };
    const onUp = (e: PointerEvent) => {
      dragging = false;
      sash.releasePointerCapture(e.pointerId);
      sash.removeEventListener('pointermove', onMove);
      sash.removeEventListener('pointerup', onUp);
    };
    sash.addEventListener('pointermove', onMove);
    sash.addEventListener('pointerup', onUp);
  }

  function onSashDouble() {
    sizePctA = clamp(defaultSizePct);
  }

  const ariaOrientation = $derived(orientation === 'horizontal' ? 'vertical' : 'horizontal');
</script>

<div class="pane-split" class:vertical={orientation === 'vertical'} bind:this={containerEl}>
  <div class="pane pane-a" style="flex-basis: {sizePctA}%;">
    {@render paneA()}
  </div>
  <div
    class="pane-sash"
    class:active={dragging}
    role="separator"
    aria-orientation={ariaOrientation}
    aria-label="Resize pane"
    onpointerdown={onSashDown}
    ondblclick={onSashDouble}
  ></div>
  <div class="pane pane-b" style="flex-basis: {100 - sizePctA}%;">
    {@render paneB()}
  </div>
</div>

<style>
  .pane-split {
    display: flex;
    flex-direction: row;
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
  }
  .pane-split.vertical {
    flex-direction: column;
  }
  .pane {
    min-width: 0;
    min-height: 0;
    flex-grow: 0;
    flex-shrink: 0;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }
  .pane-sash {
    flex: 0 0 4px;
    background: transparent;
    position: relative;
    z-index: 5;
    user-select: none;
  }
  /* 1px visible line centered inside the 4px hit area. Same colour as
     section dividers so the split reads as part of the same hairline
     grid. The wider parent stays hover-able and draggable. */
  .pane-sash::before {
    content: '';
    position: absolute;
    background: var(--gs-border);
    pointer-events: none;
  }
  .pane-split:not(.vertical) > .pane-sash {
    cursor: col-resize;
    width: 4px;
    margin: 0 -2px;
  }
  .pane-split:not(.vertical) > .pane-sash::before {
    top: 0;
    bottom: 0;
    left: 50%;
    width: 1px;
    transform: translateX(-50%);
  }
  .pane-split.vertical > .pane-sash {
    cursor: row-resize;
    height: 4px;
    margin: -2px 0;
  }
  .pane-split.vertical > .pane-sash::before {
    left: 0;
    right: 0;
    top: 50%;
    height: 1px;
    transform: translateY(-50%);
  }
  .pane-sash:hover,
  .pane-sash.active {
    background: var(--gs-sash-hover);
  }
</style>
