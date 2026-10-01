<script lang="ts">
  import Modal from '../common/Modal.svelte';
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
    <button type="button" class="btn-secondary" onclick={onDiscard}>Start fresh</button>
    <button type="button" class="btn-primary" onclick={onResume}>Resume</button>
  {/snippet}
</Modal>

<style>
  p {
    margin: 0;
  }
  button {
    font-family: inherit;
    font-size: 13px;
    padding: 6px 14px;
    border-radius: 2px;
    cursor: pointer;
    height: 30px;
  }
  .btn-primary {
    background: var(--gs-accent);
    color: var(--gs-text-on-accent);
    border: none;
  }
  .btn-primary:hover {
    background: var(--gs-accent-hover);
  }
  .btn-primary:active {
    background: var(--gs-accent-active);
  }
  .btn-secondary {
    background: transparent;
    color: var(--gs-text);
    border: 1px solid var(--gs-border);
  }
  .btn-secondary:hover {
    background: var(--gs-control-hover);
    color: var(--gs-text-strong);
  }
</style>
