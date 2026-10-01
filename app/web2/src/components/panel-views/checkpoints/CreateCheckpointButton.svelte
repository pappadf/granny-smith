<script lang="ts">
  import { gsEval } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { checkpointsView } from './checkpointsView.svelte';

  async function onClick() {
    if (machine.status !== 'running' && machine.status !== 'paused') {
      showNotification('Start a machine before creating a checkpoint', 'warning');
      return;
    }
    const label = autoLabel();
    try {
      const ok = await gsEval('checkpoint.snapshot', [label]);
      if (ok === true) {
        showNotification(`Checkpoint '${label}' created`, 'info');
        await checkpointsView.refresh?.();
      } else {
        showNotification('Checkpoint creation failed', 'error');
      }
    } catch {
      showNotification('Checkpoint creation failed', 'error');
    }
  }

  function autoLabel(): string {
    const d = new Date();
    const pad = (n: number) => String(n).padStart(2, '0');
    return `Checkpoint ${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(
      d.getHours(),
    )}:${pad(d.getMinutes())}`;
  }
</script>

<button
  type="button"
  class="action-btn"
  onclick={onClick}
  title="Save a checkpoint of the current machine"
>
  Create Checkpoint
</button>

<style>
  .action-btn {
    background: transparent;
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    height: var(--gs-size-control);
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
  }
  .action-btn:hover {
    background: var(--gs-row-hover);
  }
</style>
