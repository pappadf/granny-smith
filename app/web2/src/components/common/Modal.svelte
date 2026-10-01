<script lang="ts">
  import type { Snippet } from 'svelte';

  interface Props {
    open: boolean;
    title?: string;
    onClose?: () => void;
    /** Whether clicking the backdrop or pressing Esc dismisses the modal. */
    dismissible?: boolean;
    /** A large card sized to the window (a document viewer) instead of a 520px one. */
    wide?: boolean;
    children?: Snippet;
    actions?: Snippet;
  }
  let {
    open,
    title,
    onClose,
    dismissible = true,
    wide = false,
    children,
    actions,
  }: Props = $props();

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
    <div class="modal-card" class:wide role="dialog" aria-modal="true" aria-label={title}>
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
    background: var(--gs-backdrop);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 2600;
  }
  .modal-card {
    background: var(--gs-surface-raised);
    color: var(--gs-text);
    border: 1px solid var(--gs-border);
    border-radius: 6px;
    box-shadow: var(--gs-shadow-modal);
    min-width: 320px;
    max-width: 520px;
    padding: 20px 22px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .modal-card.wide {
    width: min(92vw, 1000px);
    max-width: none;
    height: min(90vh, 1100px);
    padding: 14px 16px;
    gap: 10px;
  }
  .modal-card.wide .modal-body {
    flex: 1 1 auto;
    min-height: 0;
    display: flex;
  }
  .modal-title {
    margin: 0;
    font-size: 16px;
    font-weight: 500;
    color: var(--gs-text-strong);
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
