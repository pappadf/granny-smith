<script lang="ts">
  import ContextMenu, { type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  const noop = () => undefined;
  const items: ContextMenuItem[] = [
    { label: 'Open', action: noop },
    { label: 'Rename…', action: noop },
    { label: 'Download', action: noop },
    { sep: true },
    { label: 'Delete', action: noop, danger: true },
  ];
  // A checkable menu (the microphone's device list), with an icon, a
  // shortcut and a disabled entry.
  const checkable: ContextMenuItem[] = [
    { label: 'Disconnect microphone', action: noop, icon: 'mic-off', shortcut: 'Ctrl+M' },
    { sep: true },
    { label: 'System default', action: noop, checked: true },
    { label: 'USB Audio', action: noop, checked: false },
    { label: 'Unavailable input', action: noop, checked: false, disabled: true },
  ];
</script>

<ContextMenu items={variant === 'checked' ? checkable : items} x={24} y={24} onClose={noop} />
