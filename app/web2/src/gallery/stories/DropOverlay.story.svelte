<script lang="ts">
  import { onMount } from 'svelte';
  import DropOverlay from '@/components/display/DropOverlay.svelte';

  // The display's drop overlay, shown by a synthetic file drag over the
  // display area.
  let area = $state<HTMLDivElement | null>(null);
  onMount(() => {
    const dt = new DataTransfer();
    dt.items.add(new File(['x'], 'System 7.5.3.dsk'));
    area?.dispatchEvent(
      new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer: dt }),
    );
    area?.dispatchEvent(
      new DragEvent('dragover', {
        bubbles: true,
        cancelable: true,
        dataTransfer: dt,
        clientX: 100,
        clientY: 100,
      }),
    );
  });
</script>

<div class="gs-display" bind:this={area}>
  <DropOverlay />
</div>

<style>
  .gs-display {
    position: absolute;
    inset: 0;
    background: var(--gs-surface-app);
  }
</style>
