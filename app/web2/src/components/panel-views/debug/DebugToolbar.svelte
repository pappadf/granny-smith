<script lang="ts">
  import Toolbar from '@/components/ui/Toolbar.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import { machine } from '@/state/machine.svelte';
  import { continueExec, pauseExec, stepInto, stopMachine, restart } from '@/bus/debug';

  const isRunning = $derived(machine.status === 'running');
  const isPaused = $derived(machine.status === 'paused');
  const stepDisabled = $derived(!isPaused);

  async function onContinueOrPause() {
    if (isRunning) await pauseExec();
    else await continueExec();
  }
</script>

<Toolbar class="debug-toolbar" variant="inline" label="Debug actions">
  {#if isRunning}
    <IconButton
      class="tb-btn"
      tone="panel"
      icon="pause"
      iconSize={14}
      label="Pause"
      onclick={onContinueOrPause}
    />
  {:else}
    <IconButton
      class="tb-btn"
      tone="panel"
      icon="play"
      iconSize={14}
      label="Continue"
      onclick={onContinueOrPause}
    />
  {/if}
  <IconButton
    class="tb-btn"
    tone="panel"
    icon="step-into"
    iconSize={14}
    label="Step Into"
    disabled={stepDisabled}
    onclick={() => stepInto(1)}
  />
  <IconButton
    class="tb-btn"
    tone="panel"
    icon="stop"
    iconSize={14}
    label="Stop"
    onclick={() => stopMachine()}
  />
  <IconButton
    class="tb-btn"
    tone="panel"
    icon="restart"
    iconSize={14}
    label="Restart"
    onclick={() => restart()}
  />
</Toolbar>
