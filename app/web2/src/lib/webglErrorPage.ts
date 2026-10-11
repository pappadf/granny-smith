import type { GraphicsDiagnosis, GraphicsFailure } from './gpuDiagnostics';

// Renders a full-page block error when the browser cannot render the
// emulator (lib/gpuDiagnostics.ts says which way), in place of the Svelte
// app. Plain DOM on purpose — keeps the fallback path independent of the
// framework runtime so it survives even if something upstream is wrong.

// What the page says about one failure: a headline, what happened, and the
// steps to try, most likely fix first.
type FailureText = { headline: string; blurb: string; steps: string[] };

// The fix for a stuck GPU process; first in every list where it can help.
const RESTART =
  'Quit the browser completely and open it again (on a Mac, use Quit or ⌘Q — closing the window is not enough). A stuck graphics process is the most common cause, and only a restart clears it.';
const HW_ACCEL =
  'In the browser settings, turn on “Use hardware acceleration when available”, then restart the browser.';
const GPU_REPORT =
  'Open <code>chrome://gpu</code> (or <code>about:support</code> in Firefox) and check that WebGL 2 is listed as <em>Hardware accelerated</em>; update the graphics drivers if they are shown as outdated or blocklisted.';
const UPDATE =
  'Update the browser to its current version (Safari 17 or later), or use a current Chrome, Edge or Firefox.';

// The broken-graphics text shared by every "context is there but dead" case.
const STUCK: FailureText = {
  headline: 'The browser’s graphics stopped working',
  blurb:
    'The browser gave this page a WebGL 2 context that does not work: it was lost as soon as it was created, or it could not draw a test frame. This happens when the browser’s graphics (GPU) process is stuck, and it stays that way for every page until the browser is restarted.',
  steps: [RESTART, GPU_REPORT],
};

const TEXT: Record<GraphicsFailure, FailureText> = {
  'no-canvas': {
    headline: 'WebGL is unavailable',
    blurb:
      'This page could not create a canvas to draw on, so the emulator cannot show its screen.',
    steps: [RESTART, UPDATE],
  },
  'no-webgl': {
    headline: 'WebGL is unavailable',
    blurb:
      'This browser doesn’t offer WebGL at all right now. The emulator needs WebGL 2 to draw its screen.',
    steps: [RESTART, HW_ACCEL, GPU_REPORT],
  },
  'no-webgl2': {
    headline: 'WebGL 2 is required',
    blurb:
      'This browser offers WebGL 1 but not WebGL 2, which the emulator needs to draw its screen. The browser is probably too old, or hardware acceleration is off.',
    steps: [UPDATE, HW_ACCEL, RESTART],
  },
  'context-lost': STUCK,
  'gpu-broken': STUCK,
  'no-offscreencanvas': {
    headline: 'This browser is too old for the emulator',
    blurb:
      'The emulator draws its screen from a background worker, which needs OffscreenCanvas. This browser does not support it.',
    steps: [UPDATE],
  },
  'worker-no-webgl2': {
    headline: 'WebGL 2 is unavailable in background workers',
    blurb:
      'The emulator draws its screen from a background worker (WebGL 2 on an OffscreenCanvas). This browser offers WebGL 2 to the page but refused it to a worker. If the emulator worked in this browser before, its graphics process is stuck; otherwise the browser is too old.',
    steps: [RESTART, UPDATE],
  },
  'worker-context-lost': {
    ...STUCK,
    blurb:
      'The browser gave the emulator’s background worker a WebGL 2 context that was lost as soon as it was created. This happens when the browser’s graphics (GPU) process is stuck, and it stays that way until the browser is restarted.',
  },
  'worker-gpu-broken': {
    ...STUCK,
    blurb:
      'The browser gave the emulator’s background worker a WebGL 2 context that could not draw a test frame. This happens when the browser’s graphics (GPU) process is stuck, and it stays that way until the browser is restarted.',
  },
  'renderer-no-context': {
    headline: 'The emulator could not get its graphics context',
    blurb:
      'The emulator asked for a WebGL 2 context for its screen and the browser refused. The startup check passed, so the browser’s graphics most likely failed in the meantime.',
    steps: [RESTART, GPU_REPORT],
  },
  'renderer-context-dead': {
    ...STUCK,
    blurb:
      'The browser gave the emulator a WebGL 2 context that was lost as soon as it was created. This happens when the browser’s graphics (GPU) process is stuck, and it stays that way until the browser is restarted.',
  },
  'renderer-context-lost': {
    headline: 'The browser took the emulator’s graphics away',
    blurb:
      'The browser lost the emulator’s WebGL 2 context, so its screen can no longer be drawn. This happens when the browser’s graphics (GPU) process crashes or is stuck.',
    steps: [
      'Reload the page. If it fails again straight away, quit the browser completely and open it again (on a Mac, use Quit or ⌘Q).',
      GPU_REPORT,
    ],
  },
  'renderer-shader-failed': {
    ...STUCK,
    blurb:
      'The browser’s WebGL would not build the emulator’s display shaders, which are plain WebGL 2 code that every working browser accepts. Its graphics (GPU) process is most likely stuck.',
  },
};

export function renderWebGLErrorPage(target: HTMLElement, result: GraphicsDiagnosis): void {
  if (result.ok) return;
  const text = TEXT[result.failure];

  target.innerHTML = '';
  const root = document.createElement('div');
  root.className = 'gs-webgl-error';
  root.setAttribute('role', 'alert');
  root.dataset.failure = result.failure;
  // The steps are trusted literals above (some carry markup); everything
  // the browser reported is escaped.
  root.innerHTML = `
    <div class="gs-webgl-error__card">
      <h1>${escapeHtml(text.headline)}</h1>
      <p>${escapeHtml(text.blurb)}</p>
      <p>Try the following:</p>
      <ol>
        ${text.steps.map((s) => `<li>${s}</li>`).join('')}
      </ol>
      <p class="gs-webgl-error__detail">Details (<code>${escapeHtml(result.failure)}</code>): ${escapeHtml(result.detail)}</p>
      <button type="button" class="gs-webgl-error__retry">Reload</button>
    </div>
  `;
  target.appendChild(root);

  const style = document.createElement('style');
  style.textContent = CSS;
  target.appendChild(style);

  const btn = root.querySelector<HTMLButtonElement>('.gs-webgl-error__retry');
  btn?.addEventListener('click', () => location.reload());
}

// Renders a full-page block error when the emulator cannot start at all (a
// bridge version mismatch, a module that fails to load, a worker that never
// comes up).  Same card and plain-DOM approach as the WebGL page, so it works
// even when the failure is in the app's own startup.
export function renderStartupErrorPage(
  target: HTMLElement,
  reason: string,
  headline = 'The emulator could not start',
  advice = 'Reloading usually fixes this, particularly right after an update (the page and the emulator it loads must come from the same build).',
): void {
  target.innerHTML = '';
  const root = document.createElement('div');
  root.className = 'gs-webgl-error';
  root.setAttribute('role', 'alert');
  root.innerHTML = `
    <div class="gs-webgl-error__card">
      <h1>${escapeHtml(headline)}</h1>
      <p>${escapeHtml(advice)}</p>
      <p class="gs-webgl-error__detail">Details: <code>${escapeHtml(reason)}</code></p>
      <button type="button" class="gs-webgl-error__retry">Reload</button>
    </div>
  `;
  target.appendChild(root);
  const style = document.createElement('style');
  style.textContent = CSS;
  target.appendChild(style);
  root
    .querySelector<HTMLButtonElement>('.gs-webgl-error__retry')
    ?.addEventListener('click', () => location.reload());
}

function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// Every value is a token, so the page follows the active skin (the token
// stylesheet is loaded by the time main.ts shows it); the fallbacks are
// Workbench's dark values, for a page shown before any stylesheet applies.
// This is the one place token fallbacks are allowed (tests/lint/tokens.test.ts).
const CSS = `
.gs-webgl-error {
  position: fixed;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--gs-surface-app, #1e1e1e);
  color: var(--gs-text, #cccccc);
  font-family: var(--gs-font-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif);
  padding: 24px;
  box-sizing: border-box;
  z-index: 9999;
}
.gs-webgl-error__card {
  max-width: 560px;
  background: var(--gs-card-bg, #252526);
  border: 1px solid var(--gs-border-card, rgba(255, 255, 255, 0.1));
  border-radius: 6px;
  padding: 28px 32px;
  line-height: 1.5;
}
.gs-webgl-error__card h1 {
  margin: 0 0 12px;
  font-size: 20px;
  font-weight: 600;
  color: var(--gs-text-strong, #e7e7e7);
}
.gs-webgl-error__card p {
  margin: 8px 0;
}
.gs-webgl-error__card ol {
  margin: 8px 0 16px;
  padding-left: 22px;
}
.gs-webgl-error__card li {
  margin: 4px 0;
}
.gs-webgl-error__card code {
  background: var(--gs-input-bg, #3c3c3c);
  color: var(--gs-input-fg, #e7e7e7);
  padding: 1px 5px;
  border-radius: 3px;
  font-size: 12px;
}
.gs-webgl-error__detail {
  margin-top: 16px;
  font-size: 12px;
  color: var(--gs-text-muted, rgba(231, 231, 231, 0.6));
}
.gs-webgl-error__retry {
  margin-top: 16px;
  background: var(--gs-accent, #0e639c);
  color: var(--gs-text-on-accent, #ffffff);
  border: none;
  padding: 8px 18px;
  border-radius: 4px;
  font-size: 13px;
  cursor: pointer;
}
.gs-webgl-error__retry:hover {
  background: var(--gs-accent-hover, #1177bb);
}
.gs-webgl-error__retry:focus-visible {
  outline: 2px solid var(--gs-focus-ring, #007fd4);
  outline-offset: 2px;
}
`;
