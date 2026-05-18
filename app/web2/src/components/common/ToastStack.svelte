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
    right: 3px;
    bottom: 25px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    z-index: 2545;
    pointer-events: none;
    max-width: 450px;
  }
</style>
