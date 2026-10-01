<script lang="ts">
  import DisassemblyPane from '@/components/panel-views/debug/DisassemblyPane.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { machine } from '@/state/machine.svelte';
  import { m68kFrame } from '../fixtures';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  const mmu = variant === 'mmu';
  machine.status = 'paused';
  machine.model = 'Macintosh IIcx';
  machine.mmuEnabled = mmu;
  machine.mmuKind = mmu ? '68030_pmmu' : 'none';
  debugFrame.current = m68kFrame(mmu);
</script>

<DisassemblyPane />
