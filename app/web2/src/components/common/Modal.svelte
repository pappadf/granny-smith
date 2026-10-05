<script lang="ts">
  import { tick, type Snippet } from 'svelte';
  import IconButton from '@/components/ui/IconButton.svelte';

  interface Props {
    open: boolean;
    title?: string;
    onClose?: () => void;
    /** Whether clicking the backdrop or pressing Esc dismisses the modal. */
    dismissible?: boolean;
    /** `dialog` (a centred card up to 520px), `wide` (a card sized to the
     *  window, a document viewer) or `popover` (a smaller card anchored
     *  under the top-right of the window, e.g. the log levels). */
    variant?: 'dialog' | 'wide' | 'popover';
    /** A close button in the title row. */
    closeButton?: boolean;
    /** The accessible name when it should differ from the title. */
    label?: string;
    children?: Snippet;
    actions?: Snippet;
  }
  let {
    open,
    title,
    onClose,
    dismissible = true,
    variant = 'dialog',
    closeButton = false,
    label,
    children,
    actions,
  }: Props = $props();

  let card = $state<HTMLElement | null>(null);

  const FOCUSABLE =
    'a[href], button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), [tabindex]:not([tabindex="-1"])';

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

  // Focus moves into the card when it opens (unless a field inside took
  // it already) and back to where it was when it closes.
  $effect(() => {
    if (!open) return;
    const before = document.activeElement as HTMLElement | null;
    void tick().then(() => {
      if (card && !card.contains(document.activeElement)) card.focus();
    });
    return () => {
      // Only when nothing else took focus meanwhile.
      const at = document.activeElement;
      const free = !at || at === document.body || !!card?.contains(at);
      if (free && before && before.isConnected && before !== document.body) before.focus();
    };
  });

  // Tab and Shift+Tab cycle inside the card.
  function trapTab(e: KeyboardEvent) {
    if (e.key !== 'Tab' || !card) return;
    const items = Array.from(card.querySelectorAll<HTMLElement>(FOCUSABLE)).filter(
      (el) => !el.hidden,
    );
    if (!items.length) {
      e.preventDefault();
      return;
    }
    const first = items[0];
    const last = items[items.length - 1];
    const at = document.activeElement;
    if (e.shiftKey && (at === first || at === card)) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && at === last) {
      e.preventDefault();
      first.focus();
    }
  }
</script>

{#if open}
  <!-- Legacy hooks: modal-backdrop, modal-card, wide, modal-title,
       modal-body, modal-actions. -->
  <div
    class="gs-modal-backdrop modal-backdrop"
    data-variant={variant}
    role="presentation"
    onclick={handleBackdropClick}
  >
    <div
      bind:this={card}
      class="gs-modal modal-card"
      class:wide={variant === 'wide'}
      data-variant={variant}
      role="dialog"
      aria-modal="true"
      aria-label={label ?? title}
      tabindex="-1"
      onkeydown={trapTab}
    >
      {#if title || closeButton}
        <div class="gs-modal__header">
          {#if title}
            <h2 class="gs-modal__title modal-title">{title}</h2>
          {/if}
          {#if closeButton}
            <IconButton
              class="gs-modal__close close-btn"
              icon="close"
              label="Close"
              tone="panel"
              onclick={() => onClose?.()}
            />
          {/if}
        </div>
      {/if}
      <div class="gs-modal__body modal-body">
        {@render children?.()}
      </div>
      {#if actions}
        <div class="gs-modal__actions modal-actions">
          {@render actions()}
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .gs-modal-backdrop {
    position: fixed;
    inset: 0;
    background: var(--gs-backdrop);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: var(--gs-z-modal);
  }
  .gs-modal-backdrop[data-variant='popover'] {
    z-index: var(--gs-z-popover);
    align-items: flex-start;
    justify-content: flex-end;
    padding: var(--gs-popover-offset-top) var(--gs-space-4) var(--gs-space-4);
  }
  .gs-modal {
    background: var(--gs-modal-bg);
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-modal-radius);
    box-shadow: var(--gs-shadow-modal);
    min-width: var(--gs-modal-min-width);
    max-width: var(--gs-modal-max-width);
    padding: var(--gs-modal-padding);
    display: flex;
    flex-direction: column;
    gap: var(--gs-modal-gap);
  }
  .gs-modal:focus-visible {
    outline: none;
  }
  .gs-modal[data-variant='wide'] {
    width: min(92vw, 1000px);
    max-width: none;
    height: min(90vh, 1100px);
    padding: var(--gs-space-3-5) var(--gs-space-4);
    gap: var(--gs-space-2-5);
  }
  .gs-modal[data-variant='wide'] .gs-modal__body {
    flex: 1 1 auto;
    min-height: 0;
    display: flex;
  }
  .gs-modal[data-variant='popover'] {
    min-width: var(--gs-popover-min-width);
    max-width: var(--gs-popover-max-width);
    max-height: 60vh;
    overflow: auto;
    padding: var(--gs-popover-padding);
    gap: var(--gs-space-2);
  }
  .gs-modal__header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--gs-space-2);
  }
  .gs-modal__title {
    margin: 0;
    font-size: var(--gs-modal-title-size);
    font-weight: var(--gs-modal-title-weight);
    color: var(--gs-text-strong);
  }
  .gs-modal[data-variant='popover'] .gs-modal__title {
    font-size: var(--gs-popover-title-size);
  }
  .gs-modal__body {
    font-size: var(--gs-font-size-base);
    line-height: var(--gs-line-height-relaxed);
  }
  .gs-modal__actions {
    display: flex;
    gap: var(--gs-space-2);
    justify-content: flex-end;
    margin-top: var(--gs-space-1);
  }
</style>
