<script lang="ts">
  import ContextMenu, { type ContextMenuItem } from '@/components/common/ContextMenu.svelte';

  const noop = () => undefined;
  const items: ContextMenuItem[] = [
    { label: 'Open', action: noop },
    { label: 'Rename…', action: noop },
    { label: 'Download', action: noop },
    { sep: true },
    { label: 'Delete', action: noop, danger: true },
  ];
</script>

<ContextMenu {items} x={24} y={24} onClose={noop} />
