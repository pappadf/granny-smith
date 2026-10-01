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
    z-index: var(--gs-z-modal);
  }
  .modal-card {
    background: var(--gs-surface-raised);
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-lg);
    box-shadow: var(--gs-shadow-modal);
    min-width: 320px;
    max-width: 520px;
    padding: var(--gs-space-5) var(--gs-space-6);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-3-5);
  }
  .modal-card.wide {
    width: min(92vw, 1000px);
    max-width: none;
    height: min(90vh, 1100px);
    padding: var(--gs-space-3-5) var(--gs-space-4);
    gap: var(--gs-space-2-5);
  }
  .modal-card.wide .modal-body {
    flex: 1 1 auto;
    min-height: 0;
    display: flex;
  }
  .modal-title {
    margin: 0;
    font-size: var(--gs-font-size-xl);
    font-weight: var(--gs-font-weight-medium);
    color: var(--gs-text-strong);
  }
  .modal-body {
    font-size: var(--gs-font-size-base);
    line-height: var(--gs-line-height-relaxed);
  }
  .modal-actions {
    display: flex;
    gap: var(--gs-space-2);
    justify-content: flex-end;
    margin-top: var(--gs-space-1);
  }
</style>
