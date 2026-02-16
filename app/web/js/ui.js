// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Manages all toolbar chrome: zoom, terminal collapse, play/pause, download-state,
// scheduler switcher, audio unlock, settings modal, and status/toast notifications.
import { CONFIG } from './config.js';
import {
  isRunning, setRunning, isModuleReady, getModule, onRunStateChange
} from './emulator.js';
import { handleInterrupt, fitTerminal, showPrompt, setActive, isActive } from './terminal.js';
import { fileExists, hdSlotPath, persistSync } from './fs.js';

// --- Status / Toast ---

const _termStatusEl = document.getElementById('term-status');

// Background message shown when no transient toast is active.
let _backgroundMsg = '';

// Set a persistent background message (shown when no toast is active).
export function setBackgroundMessage(msg) {
  _backgroundMsg = msg || '';
  // Show immediately if no transient toast is displaying
  if (_termStatusEl && !_termStatusEl.dataset.toast) {
    _termStatusEl.textContent = _backgroundMsg;
  }
}

// Clear the persistent background message.
export function clearBackgroundMessage() {
  setBackgroundMessage('');
}

// Set the terminal status line text (right side of header).
export function setStatus(msg, ttlMs) {
  if (!_termStatusEl) return;
  if (msg) {
    _termStatusEl.textContent = msg;
    _termStatusEl.dataset.toast = '1';
  } else {
    delete _termStatusEl.dataset.toast;
    _termStatusEl.textContent = _backgroundMsg;
  }
  if (ttlMs) setTimeout(() => {
    if (_termStatusEl.textContent === msg) {
      delete _termStatusEl.dataset.toast;
      _termStatusEl.textContent = _backgroundMsg;
    }
  }, ttlMs);
}
window.setStatus = setStatus;

// Short-lived status message.
export function toast(msg) {
  console.log('[ui]', msg);
  setStatus(msg, CONFIG.TOAST_DURATION_MS);
}

// --- ROM overlay ---

const romOverlay = document.getElementById('rom-required-overlay');
export function showRomOverlay() { romOverlay.classList.add('visible'); }
export function hideRomOverlay() { romOverlay.classList.remove('visible'); }

// Enable the Run button once a valid ROM (or checkpoint) is loaded.
export function enableRunButton() {
  const btn = document.getElementById('btn-run');
  if (btn) btn.disabled = false;
}
// Expose on window for E2E test compatibility
window.hideRomOverlay = hideRomOverlay;

// --- Run-state helpers ---

function currentRunState() { return !!isRunning(); }

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function waitForRunState(target, timeout = 1500) {
  const start = performance.now();
  return new Promise(resolve => {
    const tick = () => {
      if (isRunning() === target) return resolve();
      if (performance.now() - start > timeout) return resolve();
      requestAnimationFrame(tick);
    };
    tick();
  });
}

async function sendCtrlC() { return handleInterrupt(); }

// Insert a floppy, pausing/resuming emulation if needed.
export async function insertFloppyWithResume(path) {
  const wasRunning = currentRunState();
  if (wasRunning) {
    await sendCtrlC();
    await waitForRunState(false, 1200);
    await sleep(40);
  }
  await window.runCommand(`insert-fd ${path} 0 1`);
  if (wasRunning) {
    await sleep(30);
    await window.runCommand('run');
    setRunning(true);
  }
}

// Initialize all UI chrome. Called once from main.js.
export function initUI({ canvas, panel, toggle, termBody, canvasWrapper, screenToolbar }) {
  // --- Terminal collapse ---
  function setCollapsed(collapsed) {
    panel.dataset.collapsed = collapsed ? 'true' : 'false';
    toggle.setAttribute('aria-expanded', String(!collapsed));
    if (!collapsed) setTimeout(fitTerminal, CONFIG.FIT_TERMINAL_DELAY_MS);
  }
  toggle.addEventListener('click', () => setCollapsed(panel.dataset.collapsed !== 'true'));
  toggle.addEventListener('keydown', e => { if (['Enter', ' '].includes(e.key)) { e.preventDefault(); toggle.click(); } });

  // --- Zoom ---
  const baseWidth = canvas.width || 512;
  const baseHeight = canvas.height || 342;
  const zoomLevelInput = document.getElementById('zoom-level');
  let zoomPct = CONFIG.DEFAULT_ZOOM_PCT;
  if (zoomPct < CONFIG.MIN_ZOOM_PCT || zoomPct > CONFIG.MAX_ZOOM_PCT) zoomPct = CONFIG.DEFAULT_ZOOM_PCT;

  function applyZoom(pct) {
    zoomPct = Math.max(CONFIG.MIN_ZOOM_PCT, Math.min(CONFIG.MAX_ZOOM_PCT, Math.round(pct)));
    const scale = zoomPct / 100;
    const w = Math.round(baseWidth * scale);
    const h = Math.round(baseHeight * scale);
    canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    canvasWrapper.style.width = w + 'px';
    canvasWrapper.style.height = h + 'px';
    screenToolbar.style.width = w + 'px';
    panel.style.width = w + 'px';
    zoomLevelInput.value = `${zoomPct}%`;
    try { localStorage.setItem('zoomPct', String(zoomPct)); } catch (_) {}
    setTimeout(() => { if (panel.dataset.collapsed === 'false') fitTerminal(); }, 40);
  }
  document.getElementById('zoom-in').addEventListener('click', () => applyZoom(zoomPct + 10));
  document.getElementById('zoom-out').addEventListener('click', () => applyZoom(zoomPct - 10));
  zoomLevelInput.addEventListener('change', () => {
    const v = parseInt(zoomLevelInput.value.replace('%', ''), 10);
    if (!isNaN(v)) applyZoom(v); else zoomLevelInput.value = `${zoomPct}%`;
  });
  applyZoom(zoomPct);

  // Fit terminal when layout changes
  const ro = new ResizeObserver(fitTerminal);
  ro.observe(termBody);
  setTimeout(fitTerminal, 120);
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && panel.dataset.collapsed === 'false') fitTerminal();
  });
  window.addEventListener('keydown', e => {
    if (['INPUT', 'TEXTAREA'].includes(e.target.tagName)) return;
    if (e.key === '`') setCollapsed(panel.dataset.collapsed !== 'true');
  });

  // --- Play / Pause button ---
  const runBtn = document.getElementById('btn-run');
  const downloadBtn = document.getElementById('btn-download');
  const ICON_PLAY = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5v14l11-7z"/></svg>';
  const ICON_PAUSE = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 5h4v14H6zm8 0h4v14h-4z"/></svg>';

  function updateRunButton(running) {
    runBtn.innerHTML = running ? ICON_PAUSE : ICON_PLAY;
    runBtn.title = running ? 'Pause (send Ctrl-C)' : 'Run (R)';
    runBtn.setAttribute('aria-pressed', running ? 'true' : 'false');
  }

  // Register run-state callback to update button + terminal prompt
  onRunStateChange((running) => {
    updateRunButton(running);
    if (running) {
      setActive(false);
      // Clear the "click play" background message when running
      clearBackgroundMessage();
    } else if (isModuleReady() && !isActive()) {
      showPrompt(true);
      // Show hint to click play when paused (only if ROM is loaded)
      if (!romOverlay.classList.contains('visible')) {
        setBackgroundMessage('Click ▶ to start emulation');
      }
    }
  });

  // --- Pointer lock background message ---
  document.addEventListener('pointerlockchange', () => {
    if (document.pointerLockElement) {
      setBackgroundMessage('Press Esc twice to release mouse pointer');
    } else {
      // Restore appropriate background message when pointer is released
      if (!isRunning()) {
        if (!romOverlay.classList.contains('visible')) {
          setBackgroundMessage('Click ▶ to start emulation');
        }
      } else {
        clearBackgroundMessage();
      }
    }
  });

  runBtn.addEventListener('click', async () => {
    // Proactively unlock/resume WebAudio
    try {
      const ctx = getModule()?.mPlusAudio?.ctx;
      if (ctx && ctx.state === 'suspended' && typeof ctx.resume === 'function') {
        ctx.resume();
      }
    } catch (_) {}
    if (isRunning()) {
      await sendCtrlC();
      setRunning(false);
      toast('interrupt (Ctrl-C)');
    } else {
      await window.runCommand('run');
      setRunning(true);
      toast('run');
    }
  });

  // --- Download state snapshot ---
  const pad2 = (n) => String(n).padStart(2, '0');
  function makeTempStatePath() {
    const now = new Date();
    const stamp = `${now.getUTCFullYear()}${pad2(now.getUTCMonth() + 1)}${pad2(now.getUTCDate())}-${pad2(now.getUTCHours())}${pad2(now.getUTCMinutes())}${pad2(now.getUTCSeconds())}`;
    return `/tmp/saved-state-${stamp}.bin`;
  }

  async function downloadStateSnapshot() {
    if (!isModuleReady()) {
      toast('Emulator not ready');
      return;
    }
    if (!downloadBtn) return;
    downloadBtn.disabled = true;
    const tempPath = makeTempStatePath();
    const tempName = tempPath.split('/').pop();
    try {
      // save-state runs synchronously via the command mutex;
      // no need to pause — the checkpoint captures the scheduler's
      // running flag so the saved state preserves run/pause state.
      await window.runCommand(`save-state ${tempPath}`);
      await window.runCommand(`download ${tempPath}`);
      toast(`State saved (${tempName})`);
    } catch (err) {
      console.error('State download failed', err);
      toast('State download failed');
    } finally {
      downloadBtn.disabled = false;
    }
  }

  // Initialize icon state (expect not running at load)
  setRunning(false);
  updateRunButton(false);
  if (downloadBtn) downloadBtn.addEventListener('click', () => { downloadStateSnapshot(); });

  // --- Scheduler mode switcher ---
  const scheduleSwitcher = document.getElementById('schedule-switcher');
  const SCHEDULE_MODE_MAP = { hw: 'strict', real: 'live', max: 'fast' };

  function updateScheduleSwitcherUI(mode) {
    const btns = scheduleSwitcher.querySelectorAll('.switcher-btn');
    btns.forEach(btn => {
      if (btn.dataset.mode === mode) {
        btn.setAttribute('aria-current', 'page');
      } else {
        btn.removeAttribute('aria-current');
      }
    });
  }

  async function setScheduleMode(mode) {
    await window.runCommand(`schedule ${mode}`);
    updateScheduleSwitcherUI(mode);
  }

  scheduleSwitcher.addEventListener('click', async (e) => {
    const btn = e.target.closest('.switcher-btn');
    if (!btn) return;
    const mode = btn.dataset.mode;
    if (!mode) return;
    if (btn.getAttribute('aria-current') === 'page') return;
    await setScheduleMode(mode);
    toast(`Scheduler: ${SCHEDULE_MODE_MAP[mode]}`);
  });

  // --- Audio unlock ---
  (function setupAudioUnlock() {

    let unlocked = false;
    const tryUnlock = () => {
      if (unlocked) return;
      try {
        const ctx = getModule()?.mPlusAudio?.ctx;
        if (ctx && typeof ctx.resume === 'function') {
          if (ctx.state === 'suspended') {
            ctx.resume().then(() => { unlocked = true; }, () => {});
          } else {
            unlocked = true;
          }
        }
      } catch (_) {}
      if (unlocked) {
        window.removeEventListener('pointerdown', tryUnlock, true);
        window.removeEventListener('keydown', tryUnlock, true);
        window.removeEventListener('click', tryUnlock, true);
        window.removeEventListener('touchstart', tryUnlock, true);
      }
    };
    window.addEventListener('pointerdown', tryUnlock, true);
    window.addEventListener('keydown', tryUnlock, true);
    window.addEventListener('click', tryUnlock, true);
    window.addEventListener('touchstart', tryUnlock, true);
  })();

  // --- Settings modal ---
  const settingsModal = document.getElementById('settings-modal');
  const checkpointToggle = document.getElementById('checkpoint-toggle');

  document.getElementById('btn-settings').addEventListener('click', async () => {
    settingsModal.setAttribute('aria-hidden', 'false');
    try {
      const output = await window.runCommand('checkpoint');
      if (output && output.includes('Current state:')) {
        const isOn = output.includes('Current state: on');
        checkpointToggle.checked = isOn;
      }
    } catch (e) {
      console.warn('Failed to query checkpoint state:', e);
    }
  });

  document.getElementById('settings-close').addEventListener('click', () => {
    settingsModal.setAttribute('aria-hidden', 'true');
  });

  checkpointToggle.addEventListener('change', async (e) => {
    const enabled = e.target.checked;
    const command = `checkpoint ${enabled ? 'on' : 'off'}`;
    try {
      await window.runCommand(command);
      console.log(`Background checkpointing ${enabled ? 'enabled' : 'disabled'}`);
    } catch (err) {
      console.error('Failed to toggle checkpoint:', err);
      e.target.checked = !enabled;
    }
  });

  // --- File picker (directory upload) ---
  const filePicker = document.getElementById('file-picker');
  filePicker.addEventListener('change', async (ev) => {
    const files = Array.from(ev.target.files || []);
    if (!files.length) return;
    for (const f of files) {
      const rel = f.webkitRelativePath || f.name;
      const p = '/persist/' + rel;
      const { writeBinary: wb } = await import('./fs.js');
      wb(p, new Uint8Array(await f.arrayBuffer()));
    }
    await persistSync();
    toast(`Uploaded ${files.length} file(s)`);
    filePicker.value = '';
  });
}
