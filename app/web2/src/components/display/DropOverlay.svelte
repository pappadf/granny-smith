<script lang="ts">
  import { processDataTransfer } from '@/bus/upload';
  import { nextDragState, isReducedMotion, isOutsideViewport } from '@/lib/dragState';
  import type { DragState } from '@/lib/dragState';

  // Spec §8.5 four-state machine. Phase 7 promotes the basic
  // dragDepth-only overlay to the full state graph: Idle → Active →
  // Display | FsTree → back to Idle on drop / leave / viewport exit.
  let dragState = $state<DragState>('idle');
  let depth = 0;
  const reduced = isReducedMotion();

  // The Display overlay only shows in the 'display' substate; the
  // FsTree branch lets the FilesystemView's own drop styling take over
  // (Phase 5 wired that path).
  const visible = $derived(dragState === 'display');

  function hasFiles(dt: DataTransfer | null): boolean {
    return !!dt && Array.from(dt.types).includes('Files');
  }

  function isOverDisplay(target: EventTarget | null): boolean {
    if (!(target instanceof Element)) return false;
    return target.closest('.gs-display, .gs-display-content, .screen-view') !== null;
  }

  function isOverFsTree(target: EventTarget | null): boolean {
    if (!(target instanceof Element)) return false;
    return target.closest('.fs-view') !== null;
  }

  $effect(() => {
    const onEnter = (e: DragEvent) => {
      if (!hasFiles(e.dataTransfer)) return;
      e.preventDefault();
      depth++;
      dragState = nextDragState(dragState, { kind: 'enter', hasFiles: true });
    };
    const onOver = (e: DragEvent) => {
      if (!hasFiles(e.dataTransfer)) return;
      e.preventDefault();
      // Viewport-exit detection: a move event with out-of-bounds
      // coordinates (some browsers fire on chrome-edge departure).
      if (isOutsideViewport(e.clientX, e.clientY)) {
        depth = 0;
        dragState = nextDragState(dragState, { kind: 'viewport-exit' });
        return;
      }
      if (isOverDisplay(e.target)) {
        dragState = nextDragState(dragState, { kind: 'over-display' });
      } else if (isOverFsTree(e.target)) {
        dragState = nextDragState(dragState, { kind: 'over-fs-tree' });
      } else {
        dragState = nextDragState(dragState, { kind: 'over-other' });
      }
    };
    const onLeave = () => {
      depth = Math.max(0, depth - 1);
      if (depth === 0) {
        dragState = nextDragState(dragState, { kind: 'leave-all' });
      }
    };
    const onDrop = (e: DragEvent) => {
      depth = 0;
      const wasDisplay = dragState === 'display';
      dragState = nextDragState(dragState, { kind: 'drop' });
      if (!wasDisplay) return;
      // Only the Display drop is handled here — the Filesystem tree's
      // own drop handler runs in FilesystemView.
      e.preventDefault();
      if (e.dataTransfer) {
        void processDataTransfer(e.dataTransfer);
      }
    };
    const onDragEnd = () => {
      depth = 0;
      dragState = nextDragState(dragState, { kind: 'end' });
    };

    document.addEventListener('dragenter', onEnter);
    document.addEventListener('dragover', onOver);
    document.addEventListener('dragleave', onLeave);
    document.addEventListener('drop', onDrop);
    document.addEventListener('dragend', onDragEnd);
    return () => {
      document.removeEventListener('dragenter', onEnter);
      document.removeEventListener('dragover', onOver);
      document.removeEventListener('dragleave', onLeave);
      document.removeEventListener('drop', onDrop);
      document.removeEventListener('dragend', onDragEnd);
    };
  });
</script>

{#if visible}
  <div class="drop-overlay" class:reduced data-state={dragState}>
    <div class="drop-label">Drop to open</div>
  </div>
{/if}

<style>
  .drop-overlay {
    position: absolute;
    inset: 0;
    pointer-events: none;
    background: var(--gs-drop-bg);
    border: 2px dashed var(--gs-drop-border);
    z-index: 100;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: opacity 150ms ease-out;
  }
  .drop-overlay.reduced {
    transition: none;
  }
  .drop-label {
    color: var(--gs-drop-label-fg);
    font-weight: 600;
    font-size: 14px;
    background: var(--gs-drop-label-bg);
    padding: 8px 16px;
    border-radius: 4px;
  }
</style>
