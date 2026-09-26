<script lang="ts">
  import RegistersSection from './RegistersSection.svelte';
  import FpuSection from './FpuSection.svelte';
  import MemorySection from './MemorySection.svelte';
  import MmuSection from './MmuSection.svelte';
  import BreakpointsSection from './BreakpointsSection.svelte';
  import WatchpointsSection from './WatchpointsSection.svelte';
  import CallStackSection from './CallStackSection.svelte';
  import AuxCoreSection from './AuxCoreSection.svelte';
  import { machine } from '@/state/machine.svelte';
</script>

<div class="sections-pane">
  <RegistersSection />
  <FpuSection />
  <MemorySection />
  <MmuSection />
  <BreakpointsSection />
  <WatchpointsSection />
  <CallStackSection />
  <!-- The machine's auxiliary cores (capabilities.aux_cpus), after the main CPU's sections. -->
  {#each machine.auxCpus as cpu (cpu.name)}
    <AuxCoreSection {cpu} />
  {/each}
</div>

<style>
  .sections-pane {
    width: 100%;
    height: 100%;
    overflow-y: auto;
    background: var(--gs-bg);
    display: flex;
    flex-direction: column;
  }
</style>
