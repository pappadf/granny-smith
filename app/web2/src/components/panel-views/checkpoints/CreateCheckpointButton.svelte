<script lang="ts" module>
  import { gsEval } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { checkpointsView } from './checkpointsView.svelte';

  // Save a checkpoint of the running machine (also the panel header's
  // overflow menu's "Create Checkpoint").
  export async function createCheckpoint(): Promise<void> {
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

<script lang="ts">
  import Button from '@/components/ui/Button.svelte';
</script>

<Button
  class="action-btn"
  onclick={createCheckpoint}
  title="Save a checkpoint of the current machine"
>
  Create Checkpoint
</Button>
