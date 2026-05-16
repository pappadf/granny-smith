<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';

  interface Props {
    open: boolean;
    title?: string;
    initial: string;
    onSubmit: (newName: string) => void;
    onClose: () => void;
  }
  let { open, title = 'Rename', initial, onSubmit, onClose }: Props = $props();

  // Captures the initial value once; the $effect below re-syncs whenever
  // the dialog re-opens with a new target.
  // svelte-ignore state_referenced_locally
  let value = $state(initial);
  let inputEl = $state<HTMLInputElement | null>(null);

  $effect(() => {
    if (open) {
      value = initial;
      // Focus + select after mount.
      requestAnimationFrame(() => {
        inputEl?.focus();
        inputEl?.select();
      });
    }
  });

  function commit() {
    const v = value.trim();
    if (!v) {
      onClose();
      return;
    }
    onSubmit(v);
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      commit();
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      onClose();
    }
  }
</script>

<Modal {open} {title} {onClose}>
  <div class="rename-body">
    <label for="rename-input" class="rename-label">New name</label>
    <input
      id="rename-input"
      class="rename-input"
      type="text"
      bind:value
      bind:this={inputEl}
      onkeydown={onKey}
    />
  </div>
  {#snippet actions()}
    <button type="button" class="btn" onclick={onClose}>Cancel</button>
    <button type="button" class="btn primary" onclick={commit}>Rename</button>
  {/snippet}
</Modal>

<style>
  .rename-body {
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-width: 280px;
  }
  .rename-label {
    font-size: 12px;
    color: var(--gs-fg-muted);
  }
  .rename-input {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 28px;
    padding: 0 8px;
    font-size: 13px;
    outline: none;
  }
  .rename-input:focus {
    border-color: var(--gs-focus);
  }
  .btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    padding: 4px 12px;
    font-size: 13px;
    cursor: pointer;
  }
  .btn:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .btn.primary {
    background: var(--gs-primary-bg);
    color: var(--gs-primary-fg);
    border-color: transparent;
  }
  .btn.primary:hover {
    background: var(--gs-primary-hover);
  }
</style>
