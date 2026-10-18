<script lang="ts">
  import Workbench from './components/workbench/Workbench.svelte';
  import StatusBar from './components/status-bar/StatusBar.svelte';
  import ToastStack from './components/common/ToastStack.svelte';
  import CheckpointResumePrompt from './components/dialogs/CheckpointResumePrompt.svelte';
  import PreviewNoticeDialog from './components/dialogs/PreviewNoticeDialog.svelte';
  import PrintViewerDialog from './components/dialogs/PrintViewerDialog.svelte';
  import DialogHost from './components/dialogs/DialogHost.svelte';
  import StorageWarning from './components/common/StorageWarning.svelte';
  import { applyAppearance } from '@/state/appearance.svelte';
  import { layout, chromeHidden } from '@/state/layout.svelte';
  import { startPersistEffects } from '@/state/persist.svelte';
  import { startCapsLockSync } from '@/lib/capslock';
  import { startScreenKeyGuard } from '@/lib/screenKeys';

  // Keep <html data-skin> in sync with the appearance state.  The first
  // write happens in main.ts before mount; this effect follows every skin
  // switch.
  $effect(() => applyAppearance());

  // Keep layout.fullscreen in sync with the browser's native fullscreen state.
  // This lives here (not in DisplayToolbar) so the listener survives the
  // toolbar being unmounted while fullscreen — otherwise pressing Esc would
  // exit fullscreen but never flip the flag back, leaving the chrome hidden.
  $effect(() => {
    const handler = () => {
      layout.fullscreen = !!document.fullscreenElement;
    };
    document.addEventListener('fullscreenchange', handler);
    return () => document.removeEventListener('fullscreenchange', handler);
  });

  startPersistEffects();
  startCapsLockSync();
  // Keys the grabbed screen gets do nothing in the browser, and a live
  // machine asks before the page is reloaded or closed (#352).
  startScreenKeyGuard();
</script>

<!-- Above everything, also with the chrome hidden: /opfs is on memory. -->
<StorageWarning />
<Workbench />
{#if !chromeHidden()}
  <StatusBar />
{/if}
<ToastStack />
<CheckpointResumePrompt />
<PreviewNoticeDialog />
<PrintViewerDialog />
<DialogHost />

<style>
  :global(html),
  :global(body),
  :global(#app) {
    height: 100%;
  }
  :global(#app) {
    display: flex;
    flex-direction: column;
  }
</style>
