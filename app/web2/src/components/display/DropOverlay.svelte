<script lang="ts">
  import { processDataTransfer } from '@/bus/upload';

  // Basic show-on-dragenter / hide-on-leave-or-drop overlay (spec §8.5 polish
  // is Phase 7). Uses a depth counter so dragging across child elements
  // doesn't flicker.
  let visible = $state(false);
  let depth = 0;

  function hasFiles(dt: DataTransfer | null): boolean {
    return !!dt && Array.from(dt.types).includes('Files');
  }

  $effect(() => {
    const onEnter = (e: DragEvent) => {
      if (!hasFiles(e.dataTransfer)) return;
      e.preventDefault();
      depth++;
      visible = true;
    };
    const onOver = (e: DragEvent) => {
      if (hasFiles(e.dataTransfer)) e.preventDefault();
    };
    const onLeave = () => {
      depth = Math.max(0, depth - 1);
      if (depth === 0) visible = false;
    };
    const onDrop = (e: DragEvent) => {
      e.preventDefault();
      depth = 0;
      visible = false;
      if (e.dataTransfer) {
        void processDataTransfer(e.dataTransfer);
      }
    };

    document.addEventListener('dragenter', onEnter);
    document.addEventListener('dragover', onOver);
    document.addEventListener('dragleave', onLeave);
    document.addEventListener('drop', onDrop);
    return () => {
      document.removeEventListener('dragenter', onEnter);
      document.removeEventListener('dragover', onOver);
      document.removeEventListener('dragleave', onLeave);
      document.removeEventListener('drop', onDrop);
    };
  });
</script>

{#if visible}
  <div class="drop-overlay">
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
  .drop-label {
    color: var(--gs-drop-label-fg);
    font-weight: 600;
    font-size: 14px;
    background: var(--gs-drop-label-bg);
    padding: 8px 16px;
    border-radius: 4px;
  }
</style>
