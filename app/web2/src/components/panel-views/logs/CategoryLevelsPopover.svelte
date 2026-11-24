<script lang="ts">
  import { onMount } from 'svelte';
  import { logs, refreshCatLevels, setCatLevel, setCatStdout } from '@/state/logs.svelte';
  import Checkbox from '@/components/ui/Checkbox.svelte';
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
    // Any non-negative integer: the core has no upper limit, and some
    // categories use levels well above 9.
    const v = Number(t.value);
    if (t.value.trim() === '' || !Number.isInteger(v) || v < 0) {
      t.value = String(logs.catLevels[cat] ?? 0);
      return;
    }
    // A refused level snaps the field back to what the core holds.
    if (!(await setCatLevel(cat, v))) t.value = String(logs.catLevels[cat] ?? 0);
  }

  // A refused change snaps the box back to what the core holds.
  async function onStdoutChange(cat: string, on: boolean) {
    if (!(await setCatStdout(cat, on))) logs.catStdout = { ...logs.catStdout };
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
    <Hint as="p" class="cat-empty"
      >Level: 0 is off, higher reports more. Every line shows in the Logs panel; tick
      <strong>Terminal</strong> to print a category's lines there as well.</Hint
    >
    <ul class="cat-list">
      {#each sortedCats as cat (cat)}
        <li class="cat-row">
          <span class="cat-name">{cat}</span>
          <Checkbox
            size="sm"
            class="cat-stdout"
            title="Also print this category's lines in the Terminal (every line reaches this panel)"
            checked={logs.catStdout[cat] ?? true}
            onchange={(v: boolean) => void onStdoutChange(cat, v)}
            label="Terminal"
          />
          <NumberInput
            min="0"
            step="1"
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
