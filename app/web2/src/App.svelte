<script lang="ts">
  import Workbench from './components/workbench/Workbench.svelte';
  import StatusBar from './components/status-bar/StatusBar.svelte';
  import ToastStack from './components/common/ToastStack.svelte';
  import { theme, applyThemeToHtml, systemTheme } from '@/state/theme.svelte';
  import { startPersistEffects } from '@/state/persist.svelte';

  // Keep <html data-theme> in sync with the theme state. The initial set is
  // done synchronously in main.ts before mount (avoids flash); this effect
  // tracks subsequent changes (toggle button, system-pref change).
  $effect(() => applyThemeToHtml(theme.mode));

  // Listen for OS-level dark/light changes when the user has theme='system'.
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

  // Mirror persisted state slices to localStorage.
  startPersistEffects();
</script>

<Workbench />
<StatusBar />
<ToastStack />

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
