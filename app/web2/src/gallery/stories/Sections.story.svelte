<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';

  const noop = () => undefined;
</script>

<CollapsibleSection title="Registers" open onToggle={noop}>
  <p class="body">Section body.</p>
</CollapsibleSection>
<CollapsibleSection title="Breakpoints" open={false} count={3} onToggle={noop}>
  <p class="body">Hidden.</p>
</CollapsibleSection>
<CollapsibleSection title="Hard disks" open count={2} onToggle={noop}>
  {#snippet actions()}<button type="button" class="act">+</button>{/snippet}
  <p class="body">Section with actions.</p>
</CollapsibleSection>

<style>
  .body {
    margin: 0;
    padding: 4px 22px;
  }
  .act {
    background: transparent;
    border: none;
    color: inherit;
  }
</style>
