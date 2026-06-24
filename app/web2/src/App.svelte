<script lang="ts">
  import Workbench from './components/workbench/Workbench.svelte';
  import StatusBar from './components/status-bar/StatusBar.svelte';
  import ToastStack from './components/common/ToastStack.svelte';
  import CheckpointResumePrompt from './components/dialogs/CheckpointResumePrompt.svelte';
  import PreviewNoticeDialog from './components/dialogs/PreviewNoticeDialog.svelte';
  import { theme, applyThemeToHtml, systemTheme } from '@/state/theme.svelte';
  import { layout } from '@/state/layout.svelte';
  import { startPersistEffects } from '@/state/persist.svelte';

  // Keep <html data-theme> in sync with the theme state. Initial set is done
  // synchronously in main.ts before mount; this effect tracks subsequent
  // changes (toggle button, system-pref change).
  $effect(() => applyThemeToHtml(theme.mode));

  $effect(() => {
    if (typeof window === 'undefined' || !window.matchMedia) return;
    const mq = window.matchMedia('(prefers-color-scheme: light)');
    const handler = () => {
      if (theme.mode === 'system') {
        document.documentElement.dataset.theme = systemTheme();
      }
    };
    mq.addEventListener('change', handler);
    return () => mq.removeEventListener('change', handler);
  });

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
</script>

<Workbench />
{#if !layout.fullscreen}
  <StatusBar />
{/if}
<ToastStack />
<CheckpointResumePrompt />
<PreviewNoticeDialog />

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
