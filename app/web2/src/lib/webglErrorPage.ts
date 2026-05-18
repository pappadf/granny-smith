import type { WebGLCheckResult } from './webglCheck';

// Renders a full-page block error when WebGL 2 is unavailable, in place
// of mounting the Svelte app. Plain DOM on purpose — keeps the fallback
// path independent of the framework runtime so it survives even if
// something upstream is wrong.

export function renderWebGLErrorPage(target: HTMLElement, result: WebGLCheckResult): void {
  if (result.ok) return;

  const headline =
    result.reason === 'no-webgl' ? 'WebGL is unavailable' : 'Hardware acceleration required';

  const blurb =
    result.reason === 'no-webgl'
      ? "This browser doesn't expose WebGL at all. The emulator needs WebGL 2 to render the Display."
      : 'This page needs WebGL 2 to run the emulator. Your browser reports it as unavailable.';

  target.innerHTML = '';
  const root = document.createElement('div');
  root.className = 'gs-webgl-error';
  root.setAttribute('role', 'alert');
  root.innerHTML = `
    <div class="gs-webgl-error__card">
      <h1>${escapeHtml(headline)}</h1>
      <p>${escapeHtml(blurb)}</p>
      <p>Try one of the following:</p>
      <ul>
        <li>Quit and restart your browser (a stuck GPU process is the most common cause).</li>
        <li>Open <code>chrome://gpu</code> (or <code>about:support</code> in Firefox) and check that hardware acceleration is enabled and WebGL 2 is listed as <em>Hardware accelerated</em>.</li>
        <li>In browser settings, enable &ldquo;Use hardware acceleration when available&rdquo;.</li>
        <li>Update graphics drivers if the GPU report shows them as outdated or blocklisted.</li>
      </ul>
      ${result.detail ? `<p class="gs-webgl-error__detail">Details: <code>${escapeHtml(result.detail)}</code></p>` : ''}
      <button type="button" class="gs-webgl-error__retry">Retry</button>
    </div>
  `;
  target.appendChild(root);

  const style = document.createElement('style');
  style.textContent = CSS;
  target.appendChild(style);

  const btn = root.querySelector<HTMLButtonElement>('.gs-webgl-error__retry');
  btn?.addEventListener('click', () => location.reload());
}

function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

const CSS = `
.gs-webgl-error {
  position: fixed;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #1e1e1e;
  color: #cccccc;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  padding: 24px;
  box-sizing: border-box;
  z-index: 9999;
}
.gs-webgl-error__card {
  max-width: 560px;
  background: #252526;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 6px;
  padding: 28px 32px;
  line-height: 1.5;
}
.gs-webgl-error__card h1 {
  margin: 0 0 12px;
  font-size: 20px;
  font-weight: 600;
  color: #e7e7e7;
}
.gs-webgl-error__card p {
  margin: 8px 0;
}
.gs-webgl-error__card ul {
  margin: 8px 0 16px;
  padding-left: 22px;
}
.gs-webgl-error__card li {
  margin: 4px 0;
}
.gs-webgl-error__card code {
  background: #3c3c3c;
  color: #e7e7e7;
  padding: 1px 5px;
  border-radius: 3px;
  font-size: 12px;
}
.gs-webgl-error__detail {
  margin-top: 16px;
  font-size: 12px;
  color: rgba(231, 231, 231, 0.6);
}
.gs-webgl-error__retry {
  margin-top: 16px;
  background: #0e639c;
  color: #ffffff;
  border: none;
  padding: 8px 18px;
  border-radius: 4px;
  font-size: 13px;
  cursor: pointer;
}
.gs-webgl-error__retry:hover {
  background: #1177bb;
}
.gs-webgl-error__retry:focus-visible {
  outline: 2px solid #007fd4;
  outline-offset: 2px;
}
`;
