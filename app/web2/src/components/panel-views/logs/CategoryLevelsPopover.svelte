<script lang="ts">
  import { onMount } from 'svelte';
  import { logs, refreshCatLevels, setCatLevel } from '@/state/logs.svelte';
  import NumberInput from '@/components/ui/NumberInput.svelte';
  import Modal from '@/components/common/Modal.svelte';
  import Hint from '@/components/ui/Hint.svelte';

  interface Props {
    open: boolean;
    onClose: () => void;
  }
  let { open, onClose }: Props = $props();

  // Pull the current cat→level map on open so the popover reflects the
  // emulator's authoritative state (the user may have typed `log <cat>
  // <n>` in the terminal between opens).
  $effect(() => {
    if (open) void refreshCatLevels();
  });

  async function onLevelChange(cat: string, ev: Event) {
    const t = ev.target as HTMLInputElement;
    const v = parseInt(t.value, 10);
    if (!Number.isFinite(v) || v < 0) {
      t.value = String(logs.catLevels[cat] ?? 0);
      return;
    }
    // A refused level snaps the field back to what the core holds.
    if (!(await setCatLevel(cat, v))) t.value = String(logs.catLevels[cat] ?? 0);
  }

  onMount(() => {
    if (open) void refreshCatLevels();
  });

  const sortedCats = $derived(Object.keys(logs.catLevels).sort());
</script>

<Modal
  {open}
  variant="popover"
  title="Log Levels"
  label="Log category levels"
  closeButton
  {onClose}
>
  {#if sortedCats.length === 0}
    <Hint as="p" class="cat-empty">
      No categories registered yet. Boot a machine, or type <code>log</code> in the terminal to populate
      this list.
    </Hint>
  {:else}
    <ul class="cat-list">
      {#each sortedCats as cat (cat)}
        <li class="cat-row">
          <span class="cat-name">{cat}</span>
          <NumberInput
            min="0"
            max="9"
            style="width: 56px"
            value={logs.catLevels[cat]}
            onchange={(e) => onLevelChange(cat, e)}
            aria-label={`Level for ${cat}`}
          />
        </li>
      {/each}
    </ul>
  {/if}
</Modal>

<style>
  .cat-list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  .cat-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--gs-space-2);
    padding: var(--gs-space-1) 0;
    font-size: var(--gs-font-size-sm);
  }
  .cat-name {
    font-family: var(--gs-font-mono);
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  :global(.cat-empty) {
    font-size: var(--gs-font-size-sm);
    margin: 0;
    line-height: var(--gs-line-height-relaxed);
  }
</style>
