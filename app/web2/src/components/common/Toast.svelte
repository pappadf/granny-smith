<script lang="ts">
  import { dismissToast, pauseTimer, resumeTimer, type Toast } from '@/state/toasts.svelte';
  import Icon from './Icon.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import type { IconName } from '@/lib/icons';

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

  const SEV_ICON: Record<Toast['severity'], IconName> = {
    info: 'info',
    warning: 'warning',
    error: 'error',
  };
</script>

<!-- Legacy hooks: toast, show, sev-icon, msg, close-btn. -->
<div
  class="gs-toast toast {toast.severity}"
  class:show={visible}
  data-intent={toast.severity === 'error' ? 'danger' : toast.severity}
  role={toast.severity === 'error' ? 'alert' : 'status'}
  onmouseenter={() => pauseTimer(toast.id)}
  onmouseleave={() => resumeTimer(toast.id)}
>
  <span class="gs-toast__icon sev-icon {toast.severity}" data-icon={SEV_ICON[toast.severity]}
    ><Icon name={SEV_ICON[toast.severity]} /></span
  >
  <span class="gs-toast__msg msg">{toast.msg}</span>
  <IconButton
    class="gs-toast__close close-btn"
    icon="close"
    label="Dismiss notification"
    tone="panel"
    rest="faded"
    onclick={() => dismissToast(toast.id)}
  />
</div>

<style>
  .gs-toast {
    background: var(--gs-toast-bg);
    color: var(--gs-toast-fg);
    border-radius: var(--gs-toast-radius);
    box-shadow: var(--gs-shadow-toast);
    padding: var(--gs-toast-padding);
    display: flex;
    align-items: center;
    gap: var(--gs-space-2-5);
    opacity: 0;
    transform: translate3d(0, 100%, 0);
    transition:
      transform var(--gs-duration-slower) var(--gs-ease-out),
      opacity var(--gs-duration-slower) var(--gs-ease-out);
    pointer-events: auto;
    font-size: var(--gs-font-size-base);
    line-height: var(--gs-size-row);
    min-width: var(--gs-toast-min-width);
  }
  .gs-toast.show {
    opacity: 1;
    transform: none;
  }
  .gs-toast__msg {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .gs-toast__icon {
    display: inline-flex;
    flex-shrink: 0;
  }
  .gs-toast[data-intent='info'] .gs-toast__icon {
    color: var(--gs-info-solid);
  }
  .gs-toast[data-intent='warning'] .gs-toast__icon {
    color: var(--gs-warning-solid);
  }
  .gs-toast[data-intent='danger'] .gs-toast__icon {
    color: var(--gs-danger-solid);
  }
</style>
