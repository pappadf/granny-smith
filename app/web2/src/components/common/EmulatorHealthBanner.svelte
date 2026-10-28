<script lang="ts">
  import Callout from '@/components/ui/Callout.svelte';
  import Button from '@/components/ui/Button.svelte';
  import { emulatorHealth } from '@/state/emulatorHealth.svelte';
  import { dumpDiagnostics } from '@/bus/diagnostics';

  // The emulator stopped answering (a stall, which may end) or died (a
  // crash, which does not).  The page stays up under this banner, so the
  // console and what the diagnostics dumped there can still be read.
  let now = $state(performance.now());
  $effect(() => {
    if (!emulatorHealth.stalled) return;
    const t = setInterval(() => (now = performance.now()), 1000);
    return () => clearInterval(t);
  });
  const stalledFor = $derived(Math.max(0, Math.round((now - emulatorHealth.stallSince) / 1000)));
</script>

{#if emulatorHealth.crashed}
  <Callout intent="danger" role="alert" class="emulator-health" data-testid="emulator-crashed">
    <strong>The emulator stopped.</strong>
    <span class="detail">{emulatorHealth.crashReason}</span>
    <span class="advice"
      >The console keeps what it printed, and the diagnostics the page dumped.</span
    >
    <span class="actions">
      <Button size="sm" onclick={() => dumpDiagnostics('requested')}>Dump diagnostics</Button>
      <Button size="sm" variant="primary" onclick={() => location.reload()}>Reload</Button>
    </span>
  </Callout>
{:else if emulatorHealth.stalled}
  <Callout intent="warning" role="alert" class="emulator-health" data-testid="emulator-stalled">
    <strong>The emulator has not answered for {stalledFor} s.</strong>
    <span class="detail">{emulatorHealth.stallReason}</span>
    <span class="advice">It may still come back; the diagnostics are on the console.</span>
    <span class="actions">
      <Button size="sm" onclick={() => dumpDiagnostics('requested')}>Dump diagnostics</Button>
      <Button size="sm" variant="primary" onclick={() => location.reload()}>Reload</Button>
    </span>
  </Callout>
{/if}

<style>
  :global(.emulator-health) {
    flex: none;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--gs-space-1-5) var(--gs-space-3);
    padding: var(--gs-space-1-5) var(--gs-space-3);
    font-size: var(--gs-font-size-sm);
  }
  .detail {
    font-family: var(--gs-font-mono);
  }
  .advice {
    color: var(--gs-text-muted);
  }
  .actions {
    display: flex;
    gap: var(--gs-space-1-5);
    margin-left: auto;
  }
</style>
