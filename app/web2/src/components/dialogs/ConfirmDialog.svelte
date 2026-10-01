<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';
  import Button from '@/components/ui/Button.svelte';

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

  let confirmEl = $state<HTMLElement | null>(null);

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
    <Button size="lg" class="btn" onclick={onClose}>{cancelText}</Button>
    <Button
      size="lg"
      variant={danger ? 'danger' : 'primary'}
      class={danger ? 'btn danger' : 'btn primary'}
      bind:ref={confirmEl}
      onclick={onConfirm}
    >
      {confirmText}
    </Button>
  {/snippet}
</Modal>

<style>
  .confirm-body {
    min-width: 280px;
    max-width: 420px;
    font-size: var(--gs-font-size-base);
    color: var(--gs-text);
    line-height: 1.45;
  }
</style>
