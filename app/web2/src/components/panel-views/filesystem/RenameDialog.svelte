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
  let error = $state('');

  $effect(() => {
    if (open) {
      value = initial;
      error = '';
      // Focus + select after mount.
      requestAnimationFrame(() => {
        inputEl?.focus();
        inputEl?.select();
      });
    }
  });

  // A rename target is a single path component. A '/' would silently turn
  // the rename into a move (worst case into the item's own subtree), and
  // '.'/'..' resolve to other directories entirely.
  function validate(v: string): string {
    if (v.includes('/') || v.includes('\\')) return 'Name cannot contain slashes';
    if (v === '.' || v === '..') return 'Invalid name';
    return '';
  }

  function commit() {
    const v = value.trim();
    if (!v) {
      onClose();
      return;
    }
    const why = validate(v);
    if (why) {
      error = why;
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
      oninput={() => (error = '')}
    />
    {#if error}
      <div class="rename-error" role="alert">{error}</div>
    {/if}
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
    gap: var(--gs-space-1-5);
    min-width: 280px;
  }
  .rename-label {
    font-size: var(--gs-font-size-sm);
    color: var(--gs-text-muted);
  }
  .rename-error {
    font-size: var(--gs-font-size-sm);
    color: var(--gs-danger-fg);
  }
  .rename-input {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: var(--gs-border-width) solid var(--gs-input-border);
    border-radius: var(--gs-radius-xs);
    height: 28px;
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-base);
    outline: none;
  }
  .rename-input:focus {
    border-color: var(--gs-focus-ring);
  }
  .btn {
    background: transparent;
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    padding: var(--gs-space-1) var(--gs-space-3);
    font-size: var(--gs-font-size-base);
    cursor: pointer;
  }
  .btn:hover {
    background: var(--gs-row-hover);
  }
  .btn.primary {
    background: var(--gs-accent);
    color: var(--gs-text-on-accent);
    border-color: transparent;
  }
  .btn.primary:hover {
    background: var(--gs-accent-hover);
  }
</style>
