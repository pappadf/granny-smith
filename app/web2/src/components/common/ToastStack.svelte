<script lang="ts">
  import { toasts } from '@/state/toasts.svelte';
  import Toast from './Toast.svelte';
</script>

<div class="toast-container" aria-live="polite" aria-atomic="false">
  {#each toasts.active as t (t.id)}
    <Toast toast={t} />
  {/each}
</div>

<style>
  .toast-container {
    position: fixed;
    right: var(--gs-toast-offset-right);
    bottom: var(--gs-toast-offset-bottom);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
    z-index: var(--gs-z-toast);
    pointer-events: none;
    max-width: 450px;
  }
</style>
