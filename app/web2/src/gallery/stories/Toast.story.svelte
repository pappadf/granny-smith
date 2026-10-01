<script lang="ts">
  import Toast from '@/components/common/Toast.svelte';
  import type { Toast as ToastData } from '@/state/toasts.svelte';

  // Rendered directly (not through the toast queue), so no timer dismisses
  // them before the screenshot.
  const list: ToastData[] = [
    { id: 9001, msg: 'State saved (Macintosh IIcx 2026-09-30)', severity: 'info', createdAt: 0 },
    { id: 9002, msg: 'Full screen blocked by the browser', severity: 'warning', createdAt: 0 },
    { id: 9003, msg: 'Save State failed (write): disk full', severity: 'error', createdAt: 0 },
  ];
</script>

<div class="stack">
  {#each list as t (t.id)}
    <Toast toast={t} />
  {/each}
</div>

<style>
  .stack {
    position: absolute;
    right: 8px;
    bottom: 8px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    max-width: 450px;
  }
</style>
