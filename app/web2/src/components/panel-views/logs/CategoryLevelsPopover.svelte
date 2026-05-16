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
    await setCatLevel(cat, v);
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
    background: rgba(0, 0, 0, 0.35);
    z-index: 2700;
    display: flex;
    align-items: flex-start;
    justify-content: flex-end;
    padding: 80px 16px 16px;
  }
  .cat-card {
    background: var(--gs-bg-alt);
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 6px;
    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
    min-width: 280px;
    max-width: 360px;
    max-height: 60vh;
    overflow: auto;
    padding: 12px 14px;
  }
  .cat-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 8px;
  }
  .cat-title {
    font-size: 13px;
    font-weight: 500;
    color: var(--gs-fg-bright);
  }
  .close-btn {
    background: none;
    border: none;
    color: var(--gs-fg-muted);
    font-size: 18px;
    line-height: 1;
    cursor: pointer;
    padding: 0 4px;
  }
  .close-btn:hover {
    color: var(--gs-fg-bright);
  }
  .cat-empty {
    font-size: 12px;
    color: var(--gs-fg-muted);
    margin: 8px 0 0;
    line-height: 1.5;
  }
  .cat-list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .cat-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 4px 0;
    font-size: 12px;
  }
  .cat-name {
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
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
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 24px;
    padding: 0 6px;
    font-size: 12px;
    outline: none;
  }
  .cat-row input[type='number']:focus {
    border-color: var(--gs-focus);
  }
</style>
