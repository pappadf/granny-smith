<script lang="ts">
  import type { Snippet } from 'svelte';

  interface Props {
    open: boolean;
    title?: string;
    onClose?: () => void;
    /** Whether clicking the backdrop or pressing Esc dismisses the modal. */
    dismissible?: boolean;
    children?: Snippet;
    actions?: Snippet;
  }
  let { open, title, onClose, dismissible = true, children, actions }: Props = $props();

  function handleBackdropClick(e: MouseEvent) {
    if (e.target === e.currentTarget && dismissible) onClose?.();
  }

  $effect(() => {
    if (!open || !dismissible) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onClose?.();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  });
</script>

{#if open}
  <div class="modal-backdrop" role="presentation" onclick={handleBackdropClick}>
    <div class="modal-card" role="dialog" aria-modal="true" aria-label={title}>
      {#if title}
        <h2 class="modal-title">{title}</h2>
      {/if}
      <div class="modal-body">
        {@render children?.()}
      </div>
      {#if actions}
        <div class="modal-actions">
          {@render actions()}
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.45);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 2600;
  }
  .modal-card {
    background: var(--gs-bg-alt);
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 6px;
    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
    min-width: 320px;
    max-width: 520px;
    padding: 20px 22px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .modal-title {
    margin: 0;
    font-size: 16px;
    font-weight: 500;
    color: var(--gs-fg-bright);
  }
  .modal-body {
    font-size: 13px;
    line-height: 1.5;
  }
  .modal-actions {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
    margin-top: 4px;
  }
</style>
