<script lang="ts">
  import Modal from '../common/Modal.svelte';
  import Button from '../ui/Button.svelte';
  import { checkpointPrompt, resolveCheckpointPrompt } from '@/state/checkpointPrompt.svelte';

  function onResume() {
    resolveCheckpointPrompt(true);
  }
  function onDiscard() {
    resolveCheckpointPrompt(false);
  }
</script>

<Modal open={checkpointPrompt.shown} title="Continue from saved checkpoint?" dismissible={false}>
  <p>
    A saved checkpoint of your previous emulator session was found. Resume from where you left off,
    or start fresh with a cold boot.
  </p>
  {#snippet actions()}
    <Button size="lg" class="btn-secondary" onclick={onDiscard}>Start fresh</Button>
    <Button size="lg" variant="primary" class="btn-primary" onclick={onResume}>Resume</Button>
  {/snippet}
</Modal>

<style>
  p {
    margin: 0;
  }
</style>
