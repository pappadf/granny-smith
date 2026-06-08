<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';

  interface Props {
    open: boolean;
    title?: string;
    message: string;
    confirmText?: string;
    cancelText?: string;
    /** Style the confirm button as a destructive action. */
    danger?: boolean;
    onConfirm: () => void;
    onClose: () => void;
  }
  let {
    open,
    title = 'Confirm',
    message,
    confirmText = 'OK',
    cancelText = 'Cancel',
    danger = false,
    onConfirm,
    onClose,
  }: Props = $props();

  let confirmEl = $state<HTMLButtonElement | null>(null);

  $effect(() => {
    if (open) requestAnimationFrame(() => confirmEl?.focus());
  });

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      onConfirm();
    }
  }
</script>

<Modal {open} {title} {onClose}>
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="confirm-body" onkeydown={onKey}>{message}</div>
  {#snippet actions()}
    <button type="button" class="btn" onclick={onClose}>{cancelText}</button>
    <button
      type="button"
      class="btn"
      class:primary={!danger}
      class:danger
      bind:this={confirmEl}
      onclick={onConfirm}
    >
      {confirmText}
    </button>
  {/snippet}
</Modal>

<style>
  .confirm-body {
    min-width: 280px;
    max-width: 420px;
    font-size: 13px;
    color: var(--gs-fg);
    line-height: 1.45;
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
  .btn.danger {
    background: var(--gs-toast-error);
    color: #fff;
    border-color: transparent;
  }
  .btn.danger:hover {
    filter: brightness(1.08);
  }
</style>
