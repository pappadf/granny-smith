<script lang="ts">
  import Modal from '../common/Modal.svelte';
  import Button from '../ui/Button.svelte';
  import { urlBoot } from '@/state/urlBoot.svelte';

  // Bump the version suffix to re-prompt users after a significant
  // update (e.g. moving from preview to GA).
  const DISMISS_KEY = 'gs-preview-notice-dismissed-v1';
  const LEGACY_URL = 'https://pappadf.github.io/gs-pages/';

  let open = $state(false);

  $effect(() => {
    // A page opened to boot a machine from its URL asks nothing: the
    // notice waits for a visit to the start screen.
    if (urlBoot.requested) return;
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
    <Button size="lg" variant="primary" class="btn-primary" onclick={onContinue}>Continue</Button>
  {/snippet}
</Modal>

<style>
  p {
    margin: 0 0 var(--gs-space-2-5);
  }
  p:last-of-type {
    margin-bottom: 0;
  }
  a {
    color: var(--gs-text-link);
    text-decoration: underline;
    word-break: break-all;
  }
</style>
