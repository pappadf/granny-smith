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
import { whenModuleReady } from '@/bus/emulator';
import { checkWebGL2Available } from '@/lib/webglCheck';
import { renderWebGLErrorPage } from '@/lib/webglErrorPage';

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

const target = document.getElementById('app');
if (!target) throw new Error('#app mount point missing from index.html');

const app = bootApp(target);
export default app;

function bootApp(target: HTMLElement): unknown {
  // Probe WebGL 2 before mounting. The emulator worker can't recover from
  // a missing GPU context — show a full-page block so the user isn't
  // stuck in a half-broken UI (Display dead, terminal alive).
  const webgl = checkWebGL2Available();
  if (!webgl.ok) {
    renderWebGLErrorPage(target, webgl);
    return null;
  }

  // Swap MockOpfs for the real browser OPFS implementation. Tests stay on
  // MockOpfs via tests/setup.ts.
  setOpfsBackend(new BrowserOpfs());

  const mounted = mount(App, { target });

  // Post-mount async orchestration. ScreenView's onMount runs Module
  // bootstrap; we wait for the bridge's ready signal before probing for a
  // checkpoint or processing URL media. No polling — bootstrap() resolves
  // `whenModuleReady()` exactly when the bridge is live.
  const urlParams = new URLSearchParams(window.location.search);
  const mediaParams = parseUrlMediaParams(urlParams);

  void (async () => {
    await whenModuleReady();

    // Expose a single boolean flag for the headless diagnostic harness
    // (scripts/ui2-diag.mjs) and other automation to wait on. Cheaper /
    // more explicit than scraping the terminal for the prompt.
    (window as unknown as { __gsReady?: boolean }).__gsReady = true;

    const resumed = await maybeOfferBackgroundCheckpoint();
    if (resumed) return;

    if (urlParams.has('rom') || mediaParams.floppies.length || mediaParams.hardDisks.length) {
      await processUrlMedia(urlParams);
    }
    // Otherwise the Welcome view stays up and waits for the user.
  })();

  return mounted;
}
