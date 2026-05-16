import './styles/tokens.css';
import './styles/reset.css';
import { mount } from 'svelte';
import App from './App.svelte';
import { loadPersistedState } from '@/state/persist.svelte';
import { applyThemeToHtml, theme } from '@/state/theme.svelte';
import { autoPickPanelPos, layout } from '@/state/layout.svelte';
import { setOpfsBackend, BrowserOpfs } from '@/bus/opfs';
import { maybeOfferBackgroundCheckpoint } from '@/bus/checkpoint';
import { processUrlMedia, parseUrlMediaParams } from '@/bus/urlMedia';
import { isModuleReady } from '@/bus/emulator';

// Synchronous before-mount work: avoid theme flash + auto-pick layout.
loadPersistedState();
applyThemeToHtml(theme.mode);

try {
  if (!localStorage.getItem('gs-panel-pos')) {
    layout.panelPos = autoPickPanelPos(window.innerWidth, window.innerHeight);
  }
} catch {
  // localStorage unavailable — fall through with default.
}

// Swap MockOpfs for the real browser OPFS implementation. Tests stay on
// MockOpfs via tests/setup.ts.
setOpfsBackend(new BrowserOpfs());

const target = document.getElementById('app');
if (!target) throw new Error('#app mount point missing from index.html');

const app = mount(App, { target });
export default app;

// Post-mount async orchestration. ScreenView's onMount runs Module
// bootstrap; we wait for that to finish before probing for a checkpoint
// or processing URL media.

const urlParams = new URLSearchParams(window.location.search);
const mediaParams = parseUrlMediaParams(urlParams);

void (async () => {
  // Poll until the Module is ready (ScreenView.onMount fires bootstrap()).
  // This is a once-at-startup wait; the Promise.race below caps it so a
  // missing WASM build (npm test / CI without `make`) doesn't deadlock.
  await waitFor(() => isModuleReady(), 30_000).catch(() => undefined);
  if (!isModuleReady()) return;

  const resumed = await maybeOfferBackgroundCheckpoint();
  if (resumed) return;

  if (urlParams.has('rom') || mediaParams.floppies.length || mediaParams.hardDisks.length) {
    await processUrlMedia(urlParams);
  }
  // Otherwise the Welcome view stays up and waits for the user.
})();

function waitFor(pred: () => boolean, timeoutMs: number): Promise<void> {
  return new Promise((resolve, reject) => {
    const start = performance.now();
    const tick = () => {
      if (pred()) resolve();
      else if (performance.now() - start > timeoutMs) reject(new Error('timeout'));
      else setTimeout(tick, 50);
    };
    tick();
  });
}
