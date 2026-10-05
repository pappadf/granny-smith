<script lang="ts">
  import CheckpointResumePrompt from '@/components/dialogs/CheckpointResumePrompt.svelte';
  import CreateCheckpointButton from '@/components/panel-views/checkpoints/CreateCheckpointButton.svelte';
  import DebugToolbar from '@/components/panel-views/debug/DebugToolbar.svelte';
  import { machine } from '@/state/machine.svelte';

  // The in-panel button families side by side (the dialog ones are in the
  // Dialogs and NoticeDialogs stories).
  machine.status = 'paused';
</script>

<div class="row"><CreateCheckpointButton /></div>
<div class="row"><DebugToolbar /></div>
<div class="row hidden"><CheckpointResumePrompt /></div>

<style>
  .row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2);
    height: 35px;
    padding: 0 var(--gs-space-2);
  }
  .hidden {
    display: none;
  }
</style>
