import './styles/tokens.css';
import './styles/reset.css';
import { mount, unmount } from 'svelte';
import App from './App.svelte';
import { loadPersistedState } from '@/state/persist.svelte';
import { applyThemeToHtml, theme } from '@/state/theme.svelte';
import { autoPickPanelPos, layout } from '@/state/layout.svelte';
import { setOpfsBackend, BrowserOpfs } from '@/bus/opfs';
import { maybeOfferBackgroundCheckpoint } from '@/bus/checkpoint';
import {
  processUrlMedia,
  parseUrlMediaParams,
  hasUrlMedia,
  urlSchedulerMode,
} from '@/bus/urlMedia';
import { whenModuleReady, onEmulatorCrash, applySchedulerMode } from '@/bus/emulator';
import { setSchedulerMode } from '@/state/machine.svelte';
import { installEvalHookForAutomation } from '@/bus/testHook';
import { checkWebGL2Available } from '@/lib/webglCheck';
import { renderWebGLErrorPage, renderStartupErrorPage } from '@/lib/webglErrorPage';

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
  // ?speed= is the toolbar's pacing preference from the start: a boot pushes
  // it to the fresh core (reconcileUiWithMachine), and a resumed machine is
  // switched to it below.
  const urlMode = urlSchedulerMode(mediaParams.speed);
  if (urlMode) setSchedulerMode(urlMode);

  void (async () => {
    try {
      await whenModuleReady();
    } catch (e) {
      // The emulator cannot start: replace the half-alive UI with a blocking
      // page that says why, as the WebGL probe does before mount.
      void unmount(mounted);
      renderStartupErrorPage(target, e instanceof Error ? e.message : String(e));
      return;
    }

    // A worker that dies later (a wasm trap, an abort) cannot be recovered in
    // this page: every request now fails at once; say so and offer a reload.
    onEmulatorCrash((reason) => {
      void unmount(mounted);
      renderStartupErrorPage(target, reason, 'The emulator stopped');
    });

    // Expose a single boolean flag for the headless diagnostic harness
    // (scripts/ui2-diag.mjs) and other automation to wait on. Cheaper /
    // more explicit than scraping the terminal for the prompt.
    (window as unknown as { __gsReady?: boolean }).__gsReady = true;

    // Under automation only, let specs read core state without typing into
    // the terminal (bus/testHook.ts).
    installEvalHookForAutomation();

    const resumed = await maybeOfferBackgroundCheckpoint();
    if (resumed) {
      if (urlMode) await applySchedulerMode(urlMode);
      return;
    }

    // Any media parameter starts URL processing: ?cd= or ?vrom= alone used to
    // be ignored (N-20).
    if (hasUrlMedia(mediaParams)) await processUrlMedia(urlParams);
    // Otherwise the Welcome view stays up and waits for the user.
  })();

  return mounted;
}
