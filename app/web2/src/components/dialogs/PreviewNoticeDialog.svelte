<script lang="ts">
  import Modal from '../common/Modal.svelte';

  // Bump the version suffix to re-prompt users after a significant
  // update (e.g. moving from preview to GA).
  const DISMISS_KEY = 'gs-preview-notice-dismissed-v1';
  const LEGACY_URL = 'https://pappadf.github.io/gs-pages/';

  let open = $state(false);

  $effect(() => {
    try {
      if (localStorage.getItem(DISMISS_KEY) !== '1') open = true;
    } catch {
      open = true;
    }
  });

  function onContinue() {
    try {
      localStorage.setItem(DISMISS_KEY, '1');
    } catch {
      // localStorage unavailable — the dialog will simply show again
      // on the next load. Not a correctness issue.
    }
    open = false;
  }
</script>

<Modal {open} title="Granny Smith — preview build" dismissible={false}>
  <p>
    This is a new UI for Granny Smith that's still under active development. You may run into rough
    edges or missing features.
  </p>
  <p>
    Looking for a stable version? Older releases are available at
    <a href={LEGACY_URL} target="_blank" rel="noopener noreferrer">{LEGACY_URL}</a>.
  </p>
  {#snippet actions()}
    <button type="button" class="btn-primary" onclick={onContinue}>Continue</button>
  {/snippet}
</Modal>

<style>
  p {
    margin: 0 0 10px;
  }
  p:last-of-type {
    margin-bottom: 0;
  }
  a {
    color: var(--gs-link, var(--gs-primary-bg));
    text-decoration: underline;
    word-break: break-all;
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
    background: var(--gs-primary-bg);
    color: var(--gs-primary-fg);
    border: none;
  }
  .btn-primary:hover {
    background: var(--gs-primary-hover);
  }
  .btn-primary:active {
    background: var(--gs-primary-active);
  }
</style>
