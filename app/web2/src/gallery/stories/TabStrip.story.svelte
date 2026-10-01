<script lang="ts">
  import Tabs from '@/components/ui/Tabs.svelte';

  const tabs = [
    { key: 'state', label: 'State' },
    { key: 'translate', label: 'Translate' },
  ] as const;
  const noop = () => undefined;
</script>

<Tabs tabClass="tab" {tabs} active="state" onSelect={noop}>
  {#snippet accessory()}<span class="acc">S | U</span>{/snippet}
</Tabs>

<style>
  .acc {
    font-size: var(--gs-font-size-xs);
  }
</style>
