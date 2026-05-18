<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import { machine } from '@/state/machine.svelte';
  import { continueExec, pauseExec, stepInto, stepOver, stopMachine, restart } from '@/bus/debug';

  const isRunning = $derived(machine.status === 'running');
  const isPaused = $derived(machine.status === 'paused');
  const stepDisabled = $derived(!isPaused);

  async function onContinueOrPause() {
    if (isRunning) await pauseExec();
    else await continueExec();
  }
</script>

<div class="debug-toolbar" role="toolbar" aria-label="Debug actions">
  {#if isRunning}
    <button
      type="button"
      class="tb-btn"
      title="Pause"
      aria-label="Pause"
      onclick={onContinueOrPause}
    >
      <Icon name="pause" size={14} />
    </button>
  {:else}
    <button
      type="button"
      class="tb-btn"
      title="Continue"
      aria-label="Continue"
      onclick={onContinueOrPause}
    >
      <Icon name="play" size={14} />
    </button>
  {/if}
  <button
    type="button"
    class="tb-btn"
    title="Step Into"
    aria-label="Step Into"
    disabled={stepDisabled}
    onclick={() => stepInto(1)}
  >
    <Icon name="step-into" size={14} />
  </button>
  <button
    type="button"
    class="tb-btn"
    title="Step Over"
    aria-label="Step Over"
    disabled={stepDisabled}
    onclick={() => stepOver()}
  >
    <Icon name="step-over" size={14} />
  </button>
  <button type="button" class="tb-btn" title="Stop" aria-label="Stop" onclick={() => stopMachine()}>
    <Icon name="stop" size={14} />
  </button>
  <button
    type="button"
    class="tb-btn"
    title="Restart"
    aria-label="Restart"
    onclick={() => restart()}
  >
    <Icon name="restart" size={14} />
  </button>
</div>

<style>
  .debug-toolbar {
    display: inline-flex;
    align-items: center;
    gap: 2px;
  }
  .tb-btn {
    width: 22px;
    height: 22px;
    background: transparent;
    color: var(--gs-fg);
    border: none;
    border-radius: 2px;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0;
  }
  .tb-btn:hover:not(:disabled) {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .tb-btn:disabled {
    color: var(--gs-fg-muted);
    cursor: default;
    opacity: 0.5;
  }
</style>
