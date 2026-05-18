// Headless diagnostic for app/web2. Spawns the python dev server, drives
// Chromium via Playwright, and waits for explicit page conditions —
// never sleeps for a hopeful "settle". Captures console + pageerror +
// terminal contents, then exits with a JSON report.
//
// Designed so an agent (or human) can run `make ui2-diag` and read back
// what's happening without an interactive browser.
//
// Usage:
//   node scripts/ui2-diag.mjs                            # software GL (swiftshader)
//   GL=native node scripts/ui2-diag.mjs                  # let Chromium pick
//   GL=disable node scripts/ui2-diag.mjs                 # explicitly disable WebGL
//   COMMANDS='cpu.pc;rom list' node scripts/ui2-diag.mjs # type these after ready
//   PORT=18181 node scripts/ui2-diag.mjs                 # custom port
//   HEADLESS=0 node scripts/ui2-diag.mjs                 # show the browser window
//   TIMEOUT_MS=15000 node scripts/ui2-diag.mjs           # hard cap on per-step waits
//
// Wait points (each fails fast with a "where did we hang" report):
//   1. python dev server reachable on PORT
//   2. page DOM-content loaded
//   3. window.__gsReady === true (set by main.ts once bridge is live)
//   4. xterm prompt visible (.xterm-rows contains the prompt string)
//   5. each typed command's response has rendered (xterm row count grew)

import { chromium } from '/workspaces/granny-smith/tests/e2e/node_modules/playwright/index.mjs';
import { spawn } from 'node:child_process';
import { request } from 'node:http';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(__dirname, '..');
const SERVE_ROOT = resolve(REPO_ROOT, 'app/web2/dist');
const PORT = parseInt(process.env.PORT ?? '18181', 10);
const GL_MODE = process.env.GL ?? 'swiftshader'; // 'swiftshader' | 'native' | 'disable'
const HEADLESS = process.env.HEADLESS !== '0';
const COMMANDS = (process.env.COMMANDS ?? '').split(';').filter(Boolean);
const TIMEOUT_MS = parseInt(process.env.TIMEOUT_MS ?? '15000', 10);
const BASE = `http://localhost:${PORT}`;

function waitForServer(port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolveOk, rejectErr) => {
    const probe = () => {
      const req = request({ host: 'localhost', port, path: '/', method: 'HEAD' }, (res) => {
        res.resume();
        resolveOk();
      });
      req.on('error', () => {
        if (Date.now() > deadline) rejectErr(new Error(`server did not start on :${port}`));
        else setTimeout(probe, 100);
      });
      req.end();
    };
    probe();
  });
}

async function main() {
  // 1. Start the python dev server.
  const server = spawn(
    'python3',
    ['scripts/dev_server.py', '--root', SERVE_ROOT, '--port', String(PORT)],
    { cwd: REPO_ROOT, stdio: ['ignore', 'pipe', 'pipe'] },
  );
  const serverOut = [];
  server.stdout.on('data', (d) => serverOut.push(d.toString()));
  server.stderr.on('data', (d) => serverOut.push(d.toString()));
  const cleanup = () => {
    try {
      server.kill('SIGTERM');
    } catch {
      /* ignore */
    }
  };
  process.on('exit', cleanup);

  const report = {
    ok: false,
    failedAt: null,
    base: BASE,
    glMode: GL_MODE,
    headless: HEADLESS,
    timeoutMs: TIMEOUT_MS,
    probe: null,
    transcripts: [],
    events: [],
    serverOut: '',
  };

  const finish = async (code = 0) => {
    report.serverOut = serverOut.join('');
    console.log(JSON.stringify(report, null, 2));
    cleanup();
    process.exit(code);
  };

  try {
    await waitForServer(PORT, TIMEOUT_MS);
  } catch (err) {
    report.failedAt = 'server-start';
    report.reason = String(err);
    await finish(1);
    return;
  }

  // 2. Launch Chromium.
  const glArgs =
    GL_MODE === 'swiftshader'
      ? ['--use-gl=angle', '--use-angle=swiftshader-webgl', '--ignore-gpu-blocklist']
      : GL_MODE === 'disable'
        ? ['--disable-webgl', '--disable-webgl2']
        : [];
  const browser = await chromium.launch({ headless: HEADLESS, args: glArgs });
  const context = await browser.newContext({
    viewport: { width: 1280, height: 800 },
    serviceWorkers: 'allow',
  });
  const page = await context.newPage();

  page.on('console', (msg) => {
    report.events.push({ kind: 'console.' + msg.type(), text: msg.text() });
  });
  page.on('pageerror', (err) => {
    report.events.push({ kind: 'pageerror', text: err.message, stack: err.stack ?? '' });
  });
  page.on('requestfailed', (req) => {
    report.events.push({
      kind: 'requestfailed',
      url: req.url(),
      failure: req.failure()?.errorText ?? '',
    });
  });

  // 3. Navigate.
  try {
    await page.goto(BASE, { waitUntil: 'domcontentloaded', timeout: TIMEOUT_MS });
  } catch (err) {
    report.failedAt = 'page.goto';
    report.reason = String(err);
    await browser.close();
    await finish(1);
    return;
  }

  // 4. Wait for the bridge to come up. main.ts sets window.__gsReady = true
  //    after `whenModuleReady()` resolves — no settle guesswork.
  try {
    await page.waitForFunction(
      () => window.__gsReady === true,
      null,
      { timeout: TIMEOUT_MS },
    );
  } catch (err) {
    // The most likely failure mode in real-browser-GPU testing: WASM
    // module loaded but the worker died before resolving ready (e.g.
    // WebGL 2 init failed and the worker exits early). Capture what
    // we can.
    report.failedAt = '__gsReady';
    report.reason = String(err);
    report.probe = await collectProbe(page);
    await browser.close();
    await finish(1);
    return;
  }

  // 5. Wait for the terminal to render its first prompt. TerminalPane
  //    seeds it from `shell.prompt` after whenModuleReady resolves.
  try {
    await page.waitForFunction(
      () => {
        const rowsEl = document.querySelector('.terminal-host .xterm-rows');
        if (!rowsEl) return false;
        // Wait until any visible row ends with a prompt marker ('> '
        // or '$ ' with any number of trailing spaces).
        return /[$>] *$/m.test(rowsEl.textContent ?? '');
      },
      null,
      { timeout: TIMEOUT_MS },
    );
  } catch (err) {
    report.failedAt = 'prompt-visible';
    report.reason = String(err);
    report.probe = await collectProbe(page);
    await browser.close();
    await finish(1);
    return;
  }

  report.probe = await collectProbe(page);

  // 6. Send commands. Each wait condition: the row count grows.
  const commandsToSend = COMMANDS.length ? COMMANDS : ['help'];
  try {
    await page.click('.terminal-host');
    for (const cmd of commandsToSend) {
      const beforeRows = await page.evaluate(
        () => document.querySelectorAll('.terminal-host .xterm-rows > div').length,
      );
      await page.keyboard.type(cmd, { delay: 12 });
      await page.keyboard.press('Enter');
      try {
        await page.waitForFunction(
          (n) =>
            document.querySelectorAll('.terminal-host .xterm-rows > div').length > n,
          beforeRows,
          { timeout: TIMEOUT_MS },
        );
      } catch {
        // Don't bail — capture what we got, mark the cmd as no-response.
        report.events.push({ kind: 'cmd-timeout', text: cmd });
      }
      const text = await page.evaluate(() => {
        const rowsEl = document.querySelector('.terminal-host .xterm-rows');
        return rowsEl ? rowsEl.innerText : '';
      });
      report.transcripts.push({ cmd, xtermText: text });
    }
  } catch (err) {
    report.transcripts.push({ cmd: '[harness]', error: String(err) });
  }

  report.ok = true;
  await browser.close();
  await finish(0);
}

async function collectProbe(page) {
  return page.evaluate(() => {
    const cnv = document.querySelector('#screen');
    const xterm = document.querySelector('.terminal-host');
    const rowsEl = xterm?.querySelector('.xterm-rows');
    return {
      crossOriginIsolated: typeof crossOriginIsolated !== 'undefined' ? crossOriginIsolated : null,
      hasSAB: typeof SharedArrayBuffer !== 'undefined',
      hasOffscreenCanvas: typeof OffscreenCanvas !== 'undefined',
      gsReady: !!window.__gsReady,
      canvasFound: !!cnv,
      canvasWidth: cnv ? cnv.width : null,
      canvasHeight: cnv ? cnv.height : null,
      xtermPresent: !!xterm,
      xtermText: rowsEl ? rowsEl.innerText : '',
      serviceWorkerControlled: !!navigator.serviceWorker?.controller,
    };
  });
}

main().catch((err) => {
  console.error('harness crashed:', err);
  process.exit(1);
});
