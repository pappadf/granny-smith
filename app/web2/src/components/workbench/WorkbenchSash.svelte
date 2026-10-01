<script lang="ts">
  import Sash from '@/components/ui/Sash.svelte';
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

<Sash
  class="workbench-sash"
  {active}
  orientation={ariaOrientation}
  label="Resize panel"
  onmousedown={onMouseDown}
  ondblclick={onDoubleClick}
/>

<style>
  :global(.gs-workbench.panel-collapsed) > :global(.workbench-sash) {
    display: none;
  }
</style>
