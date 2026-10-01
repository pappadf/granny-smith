<script lang="ts">
  import { onMount } from 'svelte';
  import { logs, refreshCatLevels, setCatLevel } from '@/state/logs.svelte';

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

  $effect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onClose();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  });

  function onBackdrop(ev: MouseEvent) {
    if (ev.target === ev.currentTarget) onClose();
  }

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

{#if open}
  <div class="cat-backdrop" role="presentation" onclick={onBackdrop}>
    <div class="cat-card" role="dialog" aria-label="Log category levels">
      <div class="cat-header">
        <span class="cat-title">Log Levels</span>
        <button type="button" class="close-btn" onclick={onClose} aria-label="Close">×</button>
      </div>
      {#if sortedCats.length === 0}
        <p class="cat-empty">
          No categories registered yet. Boot a machine, or type <code>log</code> in the terminal to populate
          this list.
        </p>
      {:else}
        <ul class="cat-list">
          {#each sortedCats as cat (cat)}
            <li class="cat-row">
              <span class="cat-name">{cat}</span>
              <input
                type="number"
                min="0"
                max="9"
                value={logs.catLevels[cat]}
                onchange={(e) => onLevelChange(cat, e)}
                aria-label={`Level for ${cat}`}
              />
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  </div>
{/if}

<style>
  .cat-backdrop {
    position: fixed;
    inset: 0;
    background: var(--gs-backdrop);
    z-index: var(--gs-z-popover);
    display: flex;
    align-items: flex-start;
    justify-content: flex-end;
    padding: var(--gs-popover-offset-top) var(--gs-space-4) var(--gs-space-4);
  }
  .cat-card {
    background: var(--gs-surface-raised);
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-lg);
    box-shadow: var(--gs-shadow-modal);
    min-width: 280px;
    max-width: 360px;
    max-height: 60vh;
    overflow: auto;
    padding: var(--gs-space-3) var(--gs-space-3-5);
  }
  .cat-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: var(--gs-space-2);
  }
  .cat-title {
    font-size: var(--gs-font-size-base);
    font-weight: var(--gs-font-weight-medium);
    color: var(--gs-text-strong);
  }
  .close-btn {
    background: none;
    border: none;
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-2xl);
    line-height: 1;
    cursor: pointer;
    padding: 0 var(--gs-space-1);
  }
  .close-btn:hover {
    color: var(--gs-text-strong);
  }
  .cat-empty {
    font-size: var(--gs-font-size-sm);
    color: var(--gs-text-muted);
    margin: var(--gs-space-2) 0 0;
    line-height: var(--gs-line-height-relaxed);
  }
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
  .cat-row input[type='number'] {
    width: 56px;
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: var(--gs-border-width) solid var(--gs-input-border);
    border-radius: var(--gs-radius-xs);
    height: 24px;
    padding: 0 var(--gs-space-1-5);
    font-size: var(--gs-font-size-sm);
    outline: none;
  }
  .cat-row input[type='number']:focus {
    border-color: var(--gs-focus-ring);
  }
</style>
