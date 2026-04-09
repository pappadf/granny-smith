// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Owns the xterm.js terminal, input state machine, prompt, history, and key handler.
import { CONFIG } from './config.js';

// Private state
let xterm = null;
let fitAddon = null;
const DEFAULT_PROMPT = '';
const inputState = {
  prompt: DEFAULT_PROMPT,
  buffer: '',
  cursor: 0,
  history: [],
  historyIndex: 0,
  active: false
};

// Callbacks (wired during init)
let _onSubmit = null;    // async (line) => { ... }
let _onInterrupt = null; // async () => { ... }
let _isReady = () => false;
let _isRunning = () => false;
let _getPrompt = () => null; // returns prompt string or null
let _tabComplete = null; // (line, cursorPos) => string[] | null

// Clear the current line on the terminal
const clearLine = () => { xterm.write('\r'); xterm.write('\x1b[2K'); };

// Refresh the prompt string from the runtime (if available)
function refreshPrompt() {
  const value = _getPrompt();
  inputState.prompt = (value && value.length) ? value : '';
}

// Re-draw the current input line (prompt + buffer + cursor position)
function renderInput() {
  if (!inputState.active) return;
  clearLine();
  xterm.write(inputState.prompt + inputState.buffer);
  const moveBack = inputState.buffer.length - inputState.cursor;
  if (moveBack > 0) xterm.write(`\x1b[${moveBack}D`);
}

// Show the prompt (optionally resetting the buffer)
export function showPrompt(resetBuffer = true) {
  if (_isRunning()) {
    inputState.active = false;
    return;
  }
  if (resetBuffer) {
    inputState.buffer = '';
    inputState.cursor = 0;
  }
  inputState.historyIndex = inputState.history.length;
  refreshPrompt();
  inputState.active = true;
  clearLine();
  xterm.write(inputState.prompt);
}

// Maintain a bounded command history (FIFO when at capacity).
function pushHistory(line) {
  if (!line || !line.trim()) return;
  inputState.history.push(line);
  if (inputState.history.length > CONFIG.MAX_HISTORY_ENTRIES) inputState.history.shift();
  inputState.historyIndex = inputState.history.length;
}

// Navigate through history
function recallHistory(delta) {
  if (!inputState.history.length) return;
  inputState.historyIndex = Math.min(
    inputState.history.length,
    Math.max(0, inputState.historyIndex + delta)
  );
  if (inputState.historyIndex === inputState.history.length) {
    inputState.buffer = '';
  } else {
    inputState.buffer = inputState.history[inputState.historyIndex];
  }
  inputState.cursor = inputState.buffer.length;
  renderInput();
}

// Write a message to the terminal, preserving the input line if active.
export function writeLine(message) {
  const text = (message ?? '').toString();
  if (!text) return;
  if (inputState.active) clearLine();
  xterm.writeln(text);
  if (inputState.active) renderInput();
}

// Submit the current input line for execution
async function submitLine() {
  if (!_isReady() || !inputState.active) return;
  const line = inputState.buffer;
  const trimmed = line.trim();
  inputState.active = false;
  xterm.write('\r\n');
  if (!trimmed) {
    showPrompt(true);
    return;
  }
  pushHistory(line);
  if (trimmed === 'clear') {
    xterm.write('\x1b[2J\x1b[H');
    showPrompt(true);
    return;
  }
  try {
    await _onSubmit(line);
  } catch (err) {
    writeLine(`Command failed: ${err?.message || err}`);
  }
  if (!_isRunning()) {
    showPrompt(true);
  }
}

// Handle Tab completion
function handleTab() {
  if (!_tabComplete || !inputState.active) return;

  const completions = _tabComplete(inputState.buffer, inputState.cursor);
  if (!completions || completions.length === 0) return;

  // Find the word being completed (last token up to cursor)
  const beforeCursor = inputState.buffer.slice(0, inputState.cursor);
  const lastSpace = beforeCursor.lastIndexOf(' ');
  const partial = beforeCursor.slice(lastSpace + 1);
  const prefix = beforeCursor.slice(0, lastSpace + 1);

  if (completions.length === 1) {
    // Single match: complete inline
    const completed = completions[0];
    inputState.buffer = prefix + completed + inputState.buffer.slice(inputState.cursor);
    inputState.cursor = prefix.length + completed.length;
    // Add trailing space
    if (inputState.cursor === inputState.buffer.length) {
      inputState.buffer += ' ';
      inputState.cursor++;
    }
    renderInput();
  } else {
    // Multiple matches: find common prefix and show options
    let common = completions[0];
    for (let i = 1; i < completions.length; i++) {
      let j = 0;
      while (j < common.length && j < completions[i].length &&
             common[j].toLowerCase() === completions[i][j].toLowerCase()) j++;
      common = common.slice(0, j);
    }

    if (common.length > partial.length) {
      // Extend to common prefix
      inputState.buffer = prefix + common + inputState.buffer.slice(inputState.cursor);
      inputState.cursor = prefix.length + common.length;
      renderInput();
    } else {
      // Show all completions
      xterm.write('\r\n');
      const cols = Math.floor(xterm.cols / 20) || 1;
      for (let i = 0; i < completions.length; i++) {
        const padded = completions[i].padEnd(19);
        xterm.write(padded + ((i + 1) % cols === 0 ? '\r\n' : ' '));
      }
      if (completions.length % cols !== 0) xterm.write('\r\n');
      renderInput();
    }
  }
}

// Handle Ctrl-C / interrupt
export async function handleInterrupt() {
  if (inputState.active) {
    clearLine();
    xterm.write('^C\r\n');
    inputState.buffer = '';
    inputState.cursor = 0;
    inputState.active = false;
  }
  try {
    await _onInterrupt();
  } catch (err) {
    writeLine(`Interrupt failed: ${err?.message || err}`);
  }
  showPrompt(true);
}

// Set the terminal active state (used by run-state callbacks)
export function setActive(active) {
  inputState.active = active;
}

// Check whether shell input is currently active
export function isActive() {
  return inputState.active;
}

// Fit the terminal to its container
export function fitTerminal() {
  try {
    fitAddon.fit();
    xterm.scrollToBottom();
  } catch (e) {}
}

// Initialize the terminal in the given container element.
export function initTerminal(containerEl, { onSubmit, onInterrupt, isReady, isRunning, getPrompt, tabComplete }) {
  _onSubmit = onSubmit;
  _onInterrupt = onInterrupt;
  _isReady = isReady;
  _isRunning = isRunning;
  _getPrompt = getPrompt;
  _tabComplete = tabComplete || null;

  const TerminalCtor = window.Terminal;
  const FitAddonCtor = window.FitAddon?.FitAddon || window.FitAddon;
  if (!TerminalCtor) {
    throw new Error('Terminal library failed to load (xterm.js CDN unreachable)');
  }
  if (!FitAddonCtor) {
    throw new Error('FitAddon failed to load (xterm-addon-fit CDN unreachable)');
  }

  xterm = new TerminalCtor({
    fontSize: 13,
    cursorBlink: true,
    convertEol: true,
    theme: { background: '#0d0f11' }
  });
  fitAddon = new FitAddonCtor();
  xterm.loadAddon(fitAddon);
  xterm.open(containerEl);
  fitAddon.fit();

  // Key handler
  xterm.onKey(async ({ key, domEvent }) => {
    const ev = domEvent;
    if (!_isReady()) return;

    // Ctrl-C / Cmd-C → interrupt
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === 'c' || ev.key === 'C')) {
      ev.preventDefault();
      await handleInterrupt();
      return;
    }

    if (!inputState.active) return;

    if (ev.key === 'Tab') {
      ev.preventDefault();
      handleTab();
      return;
    }
    if (ev.key === 'Enter') {
      ev.preventDefault();
      await submitLine();
      return;
    }
    if (ev.key === 'Backspace') {
      ev.preventDefault();
      if (inputState.cursor > 0) {
        inputState.buffer =
          inputState.buffer.slice(0, inputState.cursor - 1) +
          inputState.buffer.slice(inputState.cursor);
        inputState.cursor--;
        renderInput();
      }
      return;
    }
    if (ev.key === 'Delete') {
      ev.preventDefault();
      if (inputState.cursor < inputState.buffer.length) {
        inputState.buffer =
          inputState.buffer.slice(0, inputState.cursor) +
          inputState.buffer.slice(inputState.cursor + 1);
        renderInput();
      }
      return;
    }
    if (ev.key === 'ArrowLeft') {
      ev.preventDefault();
      if (inputState.cursor > 0) {
        inputState.cursor--;
        renderInput();
      }
      return;
    }
    if (ev.key === 'ArrowRight') {
      ev.preventDefault();
      if (inputState.cursor < inputState.buffer.length) {
        inputState.cursor++;
        renderInput();
      }
      return;
    }
    if (ev.key === 'ArrowUp') {
      ev.preventDefault();
      recallHistory(-1);
      return;
    }
    if (ev.key === 'ArrowDown') {
      ev.preventDefault();
      recallHistory(1);
      return;
    }
    if (ev.key === 'Home') {
      ev.preventDefault();
      inputState.cursor = 0;
      renderInput();
      return;
    }
    if (ev.key === 'End') {
      ev.preventDefault();
      inputState.cursor = inputState.buffer.length;
      renderInput();
      return;
    }
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === 'l' || ev.key === 'L')) {
      ev.preventDefault();
      xterm.write('\x1b[2J\x1b[H');
      inputState.buffer = '';
      inputState.cursor = 0;
      showPrompt(true);
      return;
    }
    if (ev.ctrlKey || ev.metaKey) return;
    if (key && key.length === 1 && key >= ' ') {
      ev.preventDefault();
      inputState.buffer =
        inputState.buffer.slice(0, inputState.cursor) +
        key +
        inputState.buffer.slice(inputState.cursor);
      inputState.cursor += key.length;
      renderInput();
    }
  });
}
