<script lang="ts">
  import RegistersSection from './RegistersSection.svelte';
  import MemorySection from './MemorySection.svelte';
  import MmuSection from './MmuSection.svelte';
  import BreakpointsSection from './BreakpointsSection.svelte';
  import WatchpointsSection from './WatchpointsSection.svelte';
  import CallStackSection from './CallStackSection.svelte';
</script>

<div class="sections-pane">
  <RegistersSection />
  <MemorySection />
  <MmuSection />
  <BreakpointsSection />
  <WatchpointsSection />
  <CallStackSection />
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
