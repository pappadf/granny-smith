<script lang="ts">
  import {
    layout,
    setPanelSize,
    resetPanelSizes,
    getPanelMin,
    type PanelPos,
  } from '@/state/layout.svelte';

  // Minimum Display size (in px) preserved across sash drags. The panel
  // can't grow large enough to leave the display smaller than this.
  const MIN_DISPLAY_PX = 120;

  let active = $state(false);

  function onMouseDown(ev: MouseEvent) {
    if (layout.panelCollapsed) return;
    ev.preventDefault();
    active = true;

    const pos: PanelPos = layout.panelPos;
    const wb = (ev.currentTarget as HTMLElement).parentElement;
    if (!wb) return;
    const wbRect = wb.getBoundingClientRect();
    const startPx = pos === 'bottom' ? ev.clientY : ev.clientX;
    const startSize = layout.panelSize[pos];
    const minPx = getPanelMin(pos);
    const maxPx = (pos === 'bottom' ? wbRect.height : wbRect.width) - MIN_DISPLAY_PX;

    const onMove = (e: MouseEvent) => {
      const cur = pos === 'bottom' ? e.clientY : e.clientX;
      const delta = cur - startPx;
      // panel-bottom / panel-right: dragging up/left grows the panel.
      // panel-left: dragging right grows the panel.
      const size = pos === 'left' ? startSize + delta : startSize - delta;
      setPanelSize(pos, Math.max(minPx, Math.min(maxPx, size)));
    };
    const onUp = () => {
      active = false;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  }

  function onDoubleClick() {
    resetPanelSizes();
  }

  const ariaOrientation = $derived(layout.panelPos === 'bottom' ? 'horizontal' : 'vertical');
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
  class="workbench-sash"
  class:active
  role="separator"
  aria-orientation={ariaOrientation}
  aria-label="Resize panel"
  onmousedown={onMouseDown}
  ondblclick={onDoubleClick}
></div>

<style>
  .workbench-sash {
    flex: 0 0 4px;
    background: transparent;
    position: relative;
    z-index: 5;
    user-select: none;
  }
  :global(.gs-workbench.panel-bottom) > .workbench-sash {
    cursor: row-resize;
    width: 100%;
    height: 4px;
    margin: -2px 0;
  }
  :global(.gs-workbench.panel-left) > .workbench-sash,
  :global(.gs-workbench.panel-right) > .workbench-sash {
    cursor: col-resize;
    width: 4px;
    height: 100%;
    margin: 0 -2px;
  }
  .workbench-sash:hover,
  .workbench-sash.active {
    background: var(--gs-sash-hover);
  }
  :global(.gs-workbench.panel-collapsed) > .workbench-sash {
    display: none;
  }
</style>
