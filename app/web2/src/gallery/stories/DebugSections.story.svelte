<script lang="ts">
  import SectionsPane from '@/components/panel-views/debug/SectionsPane.svelte';
  import { debug } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { machine } from '@/state/machine.svelte';
  import { m68kFrame } from '../fixtures';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  const v = variant;
  machine.status = 'paused';
  machine.model = 'Macintosh IIcx';
  machine.fpu = true;
  machine.mmuEnabled = true;
  machine.mmuKind = '68030_pmmu';
  if (v === 'aux') machine.auxCpus = [{ name: 'dsp', arch: 'dsp3210', freq: 66000000 }];
  debugFrame.current = m68kFrame(true);
  // A changed register (flashes against the previous values).
  debug.registersPrev = { d0: 1, d1: 0x1001, pc: 0x0040028e };
  debug.sections.registers = v === 'registers';
  debug.sections.fpu = v === 'fpu';
  debug.sections.memory = v === 'memory';
  debug.sections.mmu = v === 'mmu';
  debug.sections.breakpoints = v === 'breakpoints';
  debug.sections.watchpoints = v === 'breakpoints';
  debug.sections.callstack = v === 'breakpoints';
  if (v === 'aux') debug.auxOpen = { dsp: true };
</script>

<SectionsPane />
