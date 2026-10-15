<script lang="ts" module>
  import { gsEval, gsErrorText } from '@/bus/emulator';
  import { opfs } from '@/bus/opfs';
  import { getOrCreateMachine, nowStamp } from '@/lib/machineId';
  import { CHECKPOINT_DIR, SAVED_CHECKPOINT_DIR } from '@/lib/opfsPaths';
  import { startActivity, endActivity } from '@/state/activity.svelte';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { checkpointsView } from './checkpointsView.svelte';

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
      // The machine's own checkpoint first, so its autosave (what a reload
      // resumes) is this moment too; checkpoint.snapshot returns once the
      // file is complete.
      if ((await gsEval('checkpoint.snapshot', [label])) !== true) {
        showNotification('Checkpoint creation failed', 'error');
        return;
      }
      await gsEval('files.mkdir', [SAVED_CHECKPOINT_DIR]);
      await gsEval('files.mkdir', [dir]);
      const ok = await gsEval('checkpoint.save', [`${dir}/state.checkpoint`]);
      if (ok !== true) {
        await gsEval('files.rm', [dir]);
        showNotification(`Checkpoint creation failed: ${gsErrorText(ok)}`, 'error');
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
