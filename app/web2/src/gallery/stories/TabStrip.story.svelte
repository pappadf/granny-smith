<script lang="ts">
  import TabStrip from '@/components/common/TabStrip.svelte';

  const tabs = [
    { key: 'state', label: 'State' },
    { key: 'translate', label: 'Translate' },
  ] as const;
  const noop = () => undefined;
</script>

<TabStrip {tabs} active="state" onSelect={noop}>
  {#snippet accessory()}<span class="acc">S | U</span>{/snippet}
</TabStrip>

<style>
  .acc {
    font-size: var(--gs-font-size-xs);
  }
</style>
