<script lang="ts">
  import ImageCategorySection from './ImageCategorySection.svelte';
  import { images, toggleCategory, detectFdDriveCount } from '@/state/images.svelte';
  import type { ImageCategory } from '@/bus/types';

  // Spec §4.3.3 fixes this order.
  const CATEGORIES: ImageCategory[] = ['rom', 'vrom', 'fd', 'hd', 'cd'];

  // Re-probe the floppy drive count when the panel opens (the active machine may
  // have changed) so a floppy badge can name its drive. Guest-initiated ejects
  // are handled live by Module.onFloppyChange (bus/emulator.ts) — no polling.
  $effect(() => {
    void detectFdDriveCount(true);
  });
</script>

<div class="images-view">
  {#each CATEGORIES as cat (cat)}
    <ImageCategorySection
      {cat}
      open={!images.collapsed[cat]}
      onToggle={() => toggleCategory(cat)}
    />
  {/each}
</div>

<style>
  .images-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    padding: 4px 0;
    display: flex;
    flex-direction: column;
  }
</style>
