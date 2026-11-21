<script lang="ts" module>
  import { saveLabelledCheckpoint } from '@/bus/checkpoint';
  import { opfs } from '@/bus/opfs';
  import { getOrCreateMachine, nowStamp } from '@/lib/machineId';
  import { CHECKPOINT_DIR, SAVED_CHECKPOINT_DIR } from '@/lib/opfsPaths';
  import { startActivity, endActivity } from '@/state/activity.svelte';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { checkpointsView } from './checkpointsViewState.svelte';
  import { formatLocalStamp } from '@/lib/checkpointMeta';

  // Save a checkpoint of the running machine (also the panel header's
  // overflow menu's "Create Checkpoint"): a self-contained copy of the whole
  // machine, disks included, in a directory of its own under
  // SAVED_CHECKPOINT_DIR, listed in the panel next to the machines' own
  // background checkpoints.  It used to call checkpoint.snapshot, which only
  // refreshed the machine's single background checkpoint: the toast promised
  // a named checkpoint that never appeared, and there was no way to keep
  // two points in time.
  export async function createCheckpoint(): Promise<void> {
    if (machine.status !== 'running' && machine.status !== 'paused') {
      showNotification('Start a machine before creating a checkpoint', 'warning');
      return;
    }
    const label = autoLabel();
    const me = getOrCreateMachine();
    const dir = `${SAVED_CHECKPOINT_DIR}/${me.id}-${nowStamp()}`;
    startActivity(label, 'Saving');
    try {
      // The machine's own checkpoint (its autosave), then the copy in dir.
      const saved = await saveLabelledCheckpoint(label, SAVED_CHECKPOINT_DIR, dir);
      if (!saved.ok) {
        showNotification(
          saved.reason === undefined
            ? 'Checkpoint creation failed'
            : `Checkpoint creation failed: ${saved.reason}`,
          'error',
        );
        return;
      }
      // The machine's own manifest (model, RAM, build, images), labelled.
      const own =
        (await opfs.readJson<Record<string, unknown>>(
          `${CHECKPOINT_DIR}/${me.id}-${me.created}/manifest.json`,
        )) ?? {};
      await opfs.writeJson(`${dir}/manifest.json`, { ...own, label, saved: true });
      showNotification(`Checkpoint '${label}' created`, 'info');
      await checkpointsView.refresh?.();
    } catch {
      showNotification('Checkpoint creation failed', 'error');
    } finally {
      endActivity();
    }
  }

  // To the second: checkpoints made within one minute used to share a name.
  function autoLabel(): string {
    return `Checkpoint ${formatLocalStamp(new Date(), true)}`;
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
