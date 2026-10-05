<script lang="ts">
  import { processDataTransfer } from '@/bus/upload';
  import { nextDragState, isOutsideViewport } from '@/lib/dragState';
  import type { DragState } from '@/lib/dragState';

  // The four-state machine of lib/dragState.ts: Idle → Active →
  // Display | FsTree → back to Idle on drop / leave / viewport exit.
  let dragState = $state<DragState>('idle');
  let depth = 0;

  // The Display overlay only shows in the 'display' substate; the
  // FsTree branch lets the FilesystemView's own drop styling take over.
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

<!-- Always mounted, so the fade in and out runs; the motion tokens make it
     instant under reduced motion. -->
<div class="drop-overlay" data-visible={visible || undefined} aria-hidden={!visible}>
  <div class="drop-label">Drop to open</div>
</div>

<style>
  .drop-overlay {
    position: absolute;
    inset: 0;
    pointer-events: none;
    background: var(--gs-drop-bg);
    border: 2px dashed var(--gs-drop-border);
    z-index: var(--gs-z-drop);
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    transition: opacity var(--gs-duration-quick) var(--gs-ease-out);
  }
  .drop-overlay[data-visible] {
    opacity: 1;
  }
  .drop-label {
    color: var(--gs-drop-label-fg);
    font-weight: var(--gs-font-weight-semibold);
    font-size: var(--gs-font-size-md);
    background: var(--gs-drop-label-bg);
    padding: var(--gs-space-2) var(--gs-space-4);
    border-radius: var(--gs-radius-md);
  }
</style>
