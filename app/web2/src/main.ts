import './styles/tokens.css';
import './styles/reset.css';
import { mount } from 'svelte';
import App from './App.svelte';
import { loadPersistedState } from '@/state/persist.svelte';
import { applyThemeToHtml, theme } from '@/state/theme.svelte';
import { autoPickPanelPos, layout } from '@/state/layout.svelte';

// Load persisted state and apply <html data-theme> synchronously, before
// mount, to avoid a flash of unstyled (or wrong-theme) chrome on first paint.
loadPersistedState();
applyThemeToHtml(theme.mode);

// If the user hasn't explicitly chosen a panel position, auto-pick one based
// on the viewport's aspect ratio (spec §10).
try {
  if (!localStorage.getItem('gs-panel-pos')) {
    layout.panelPos = autoPickPanelPos(window.innerWidth, window.innerHeight);
  }
} catch {
  // localStorage unavailable — fall through with default panelPos.
}

const target = document.getElementById('app');
if (!target) throw new Error('#app mount point missing from index.html');

export default mount(App, { target });
