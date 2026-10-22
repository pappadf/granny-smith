<script lang="ts">
  import Callout from '@/components/ui/Callout.svelte';
  import { storageHealth } from '@/state/storageHealth.svelte';
  import { VOLATILE_STORAGE_MESSAGE } from '@/bus/storageHealth';

  // Stays up for as long as the page is open: there is nothing to dismiss,
  // since the storage does not come back without a reload.
</script>

{#if storageHealth.volatile}
  <Callout intent="danger" role="alert" class="storage-warning" data-testid="storage-warning">
    <strong>{VOLATILE_STORAGE_MESSAGE}</strong>
    Allow site data for this page, or leave private browsing, then reload.
  </Callout>
{/if}

<style>
  :global(.storage-warning) {
    flex: none;
    padding: var(--gs-space-1-5) var(--gs-space-3);
    font-size: var(--gs-font-size-sm);
  }
</style>
