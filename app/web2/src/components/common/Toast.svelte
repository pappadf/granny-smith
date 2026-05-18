<script lang="ts">
  import { dismissToast, pauseTimer, resumeTimer, type Toast } from '@/state/toasts.svelte';
  import Icon from './Icon.svelte';

  interface Props {
    toast: Toast;
  }
  let { toast }: Props = $props();

  // Trigger the slide-in animation on next tick.
  let visible = $state(false);
  $effect(() => {
    const id = requestAnimationFrame(() => (visible = true));
    return () => cancelAnimationFrame(id);
  });

  const sevGlyph = $derived(
    toast.severity === 'info' ? 'i' : toast.severity === 'warning' ? '!' : 'x',
  );
</script>

<div
  class="toast {toast.severity}"
  class:show={visible}
  role={toast.severity === 'error' ? 'alert' : 'status'}
  onmouseenter={() => pauseTimer(toast.id)}
  onmouseleave={() => resumeTimer(toast.id)}
>
  <span class="sev-icon {toast.severity}" aria-hidden="true">{sevGlyph}</span>
  <span class="msg">{toast.msg}</span>
  <button
    class="close-btn"
    aria-label="Dismiss notification"
    onclick={() => dismissToast(toast.id)}
  >
    <Icon name="close" />
  </button>
</div>

<style>
  .toast {
    background: var(--gs-toast-bg);
    color: var(--gs-toast-fg);
    border-radius: 4px;
    box-shadow: var(--gs-toast-shadow);
    padding: 10px 12px;
    display: flex;
    align-items: center;
    gap: 10px;
    opacity: 0;
    transform: translate3d(0, 100%, 0);
    transition:
      transform 300ms ease-out,
      opacity 300ms ease-out;
    pointer-events: auto;
    font-size: 13px;
    line-height: 22px;
    min-width: 260px;
  }
  .toast.show {
    opacity: 1;
    transform: none;
  }
  .msg {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .close-btn {
    background: transparent;
    border: none;
    color: var(--gs-fg);
    cursor: pointer;
    padding: 2px;
    display: none;
  }
  .toast:hover .close-btn {
    display: block;
  }
  .sev-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 16px;
    height: 16px;
    border-radius: 50%;
    font-size: 11px;
    font-weight: 700;
    flex-shrink: 0;
    color: #fff;
  }
  .sev-icon.info {
    background: var(--gs-toast-info);
    color: #fff;
  }
  .sev-icon.warning {
    background: var(--gs-toast-warning);
    color: #000;
  }
  .sev-icon.error {
    background: var(--gs-toast-error);
    color: #fff;
  }
</style>
