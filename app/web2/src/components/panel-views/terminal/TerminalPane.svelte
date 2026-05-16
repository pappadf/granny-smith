<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import {
    gsEvalLine,
    getRuntimePrompt,
    shellInterrupt,
    tabComplete,
    isModuleReady,
  } from '@/bus/emulator';
  import { setTerminalSink } from '@/bus/logSink';
  import { layout } from '@/state/layout.svelte';
  import { registerTerminalInsert } from './terminalBridge';

  // xterm types kept opaque — the module is dynamic-imported so the
  // dependency is code-split and only loads when the Terminal tab first
  // opens. Component tests mock the import via tests/setup.ts.
  interface XtermLike {
    open(el: HTMLElement): void;
    write(s: string): void;
    writeln(s: string): void;
    loadAddon(a: unknown): void;
    onKey(handler: (e: { key: string; domEvent: KeyboardEvent }) => void): void;
    focus(): void;
    scrollToBottom(): void;
    cols: number;
  }
  interface FitAddonLike {
    fit(): void;
  }

  let containerEl = $state<HTMLDivElement | null>(null);
  let xterm: XtermLike | null = null;
  let fitAddon: FitAddonLike | null = null;
  let destroyed = false;

  // Input-line state machine — mirrors app/web/js/terminal.js. The
  // shell prompt is fed back through gsEvalLine's return value.
  const MAX_HISTORY = 256;
  const inputState = {
    prompt: '',
    buffer: '',
    cursor: 0,
    history: [] as string[],
    historyIndex: 0,
    active: false,
  };

  function clearLine() {
    if (!xterm) return;
    xterm.write('\r');
    xterm.write('\x1b[2K');
  }

  function refreshPrompt() {
    const p = getRuntimePrompt();
    inputState.prompt = p ?? '';
  }

  function renderInput() {
    if (!xterm || !inputState.active) return;
    clearLine();
    xterm.write(inputState.prompt + inputState.buffer);
    const moveBack = inputState.buffer.length - inputState.cursor;
    if (moveBack > 0) xterm.write(`\x1b[${moveBack}D`);
  }

  function showPrompt(reset = true) {
    if (!xterm) return;
    if (reset) {
      inputState.buffer = '';
      inputState.cursor = 0;
    }
    inputState.historyIndex = inputState.history.length;
    refreshPrompt();
    inputState.active = true;
    clearLine();
    xterm.write(inputState.prompt);
  }

  function writeLine(message: string) {
    if (!xterm) return;
    const text = (message ?? '').toString();
    if (!text) return;
    if (inputState.active) clearLine();
    xterm.writeln(text);
    if (inputState.active) renderInput();
  }

  function pushHistory(line: string) {
    const trimmed = line?.trim();
    if (!trimmed) return;
    inputState.history.push(line);
    if (inputState.history.length > MAX_HISTORY) inputState.history.shift();
    inputState.historyIndex = inputState.history.length;
  }

  function recallHistory(delta: number) {
    if (!inputState.history.length) return;
    inputState.historyIndex = Math.min(
      inputState.history.length,
      Math.max(0, inputState.historyIndex + delta),
    );
    if (inputState.historyIndex === inputState.history.length) {
      inputState.buffer = '';
    } else {
      inputState.buffer = inputState.history[inputState.historyIndex];
    }
    inputState.cursor = inputState.buffer.length;
    renderInput();
  }

  async function submitLine() {
    if (!xterm || !isModuleReady() || !inputState.active) return;
    const line = inputState.buffer;
    const trimmed = line.trim();
    inputState.active = false;
    xterm.write('\r\n');
    if (!trimmed) {
      showPrompt(true);
      return;
    }
    pushHistory(line);
    // `clear` is handled locally — same affordance as app/web's
    // terminal.js. The shell has its own `clear` (a no-op convenience);
    // doing it here keeps the round-trip out of the hot path.
    if (trimmed === 'clear') {
      xterm.write('\x1b[2J\x1b[H');
      showPrompt(true);
      return;
    }
    try {
      await gsEvalLine(line);
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      writeLine(`Command failed: ${msg}`);
    }
    showPrompt(true);
  }

  async function doTabComplete() {
    if (!xterm || !inputState.active) return;
    const result = await tabComplete(inputState.buffer, inputState.cursor);
    if (!result || !result.candidates.length) return;
    const { candidates, span } = result;

    if (candidates.length === 1) {
      const completed = candidates[0];
      const before = inputState.buffer.slice(0, span.start);
      const after = inputState.buffer.slice(span.end);
      inputState.buffer = before + completed + after;
      inputState.cursor = before.length + completed.length;
      // Trailing space when caret lands at the very end.
      if (inputState.cursor === inputState.buffer.length) {
        inputState.buffer += ' ';
        inputState.cursor++;
      }
      renderInput();
      return;
    }

    // Multiple matches — extend to the common prefix; if it didn't
    // grow, print the whole list.
    let common = candidates[0];
    for (let i = 1; i < candidates.length; i++) {
      let j = 0;
      while (
        j < common.length &&
        j < candidates[i].length &&
        common[j].toLowerCase() === candidates[i][j].toLowerCase()
      ) {
        j++;
      }
      common = common.slice(0, j);
    }
    const partial = inputState.buffer.slice(span.start, span.end);
    if (common.length > partial.length) {
      const before = inputState.buffer.slice(0, span.start);
      const after = inputState.buffer.slice(span.end);
      inputState.buffer = before + common + after;
      inputState.cursor = before.length + common.length;
      renderInput();
      return;
    }

    xterm.write('\r\n');
    const cols = Math.floor(xterm.cols / 20) || 1;
    for (let i = 0; i < candidates.length; i++) {
      const padded = candidates[i].padEnd(19);
      xterm.write(padded + ((i + 1) % cols === 0 ? '\r\n' : ' '));
    }
    if (candidates.length % cols !== 0) xterm.write('\r\n');
    renderInput();
  }

  async function handleInterrupt() {
    if (!xterm) return;
    if (inputState.active) {
      clearLine();
      xterm.write('^C\r\n');
      inputState.buffer = '';
      inputState.cursor = 0;
      inputState.active = false;
    }
    try {
      await shellInterrupt();
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      writeLine(`Interrupt failed: ${msg}`);
    }
    showPrompt(true);
  }

  // Programmatic insert from the Command Browser. Replaces the current
  // input with `text + ' '` and focuses the terminal.
  function insertText(text: string) {
    if (!xterm) return;
    if (!inputState.active) showPrompt(true);
    inputState.buffer = text + ' ';
    inputState.cursor = inputState.buffer.length;
    renderInput();
    xterm.focus();
  }

  // Refit whenever the panel resizes, collapses, or re-orients.
  $effect(() => {
    // Track the layout dependencies that should trigger a refit.
    void layout.panelPos;
    void layout.panelSize.bottom;
    void layout.panelSize.left;
    void layout.panelSize.right;
    void layout.panelCollapsed;
    if (fitAddon) requestAnimationFrame(() => fitAddon?.fit());
  });

  onMount(() => {
    let cleanup: (() => void) | null = null;
    void (async () => {
      const [xtermMod, fitMod, _css] = await Promise.all([
        import('@xterm/xterm'),
        import('@xterm/addon-fit'),
        import('@xterm/xterm/css/xterm.css'),
      ]);
      void _css;
      if (destroyed || !containerEl) return;
      const TerminalCtor = (xtermMod as { Terminal: new (opts: unknown) => XtermLike }).Terminal;
      const FitAddonCtor = (fitMod as { FitAddon: new () => FitAddonLike }).FitAddon;

      xterm = new TerminalCtor({
        fontSize: 13,
        cursorBlink: true,
        convertEol: true,
        // Match the log-buffer cap for memory symmetry. xterm's default
        // is 1000 lines; 5000 fits one user-session of debug output.
        scrollback: 5000,
        theme: { background: '#0d0f11' },
      });
      fitAddon = new FitAddonCtor();
      xterm.loadAddon(fitAddon);
      xterm.open(containerEl);
      fitAddon.fit();

      // Pipe Module.print into the terminal.
      setTerminalSink((line: string) => writeLine(line));

      // Allow CommandBrowser rows to push into the prompt.
      registerTerminalInsert((s: string) => insertText(s));

      // Key handler — mirrors app/web/js/terminal.js.
      xterm.onKey(async ({ key, domEvent }) => {
        const ev = domEvent;
        if (!isModuleReady()) return;

        if ((ev.ctrlKey || ev.metaKey) && (ev.key === 'c' || ev.key === 'C')) {
          ev.preventDefault();
          await handleInterrupt();
          return;
        }
        if (!inputState.active) return;
        if (ev.key === 'Tab') {
          ev.preventDefault();
          await doTabComplete();
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
          if (xterm) xterm.write('\x1b[2J\x1b[H');
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

      showPrompt(true);
      const onResize = () => fitAddon?.fit();
      window.addEventListener('resize', onResize);
      cleanup = () => window.removeEventListener('resize', onResize);
    })();
    return () => {
      cleanup?.();
    };
  });

  onDestroy(() => {
    destroyed = true;
    setTerminalSink(null);
    registerTerminalInsert(null);
    xterm = null;
    fitAddon = null;
  });
</script>

<div class="terminal-host" bind:this={containerEl}></div>

<style>
  .terminal-host {
    width: 100%;
    height: 100%;
    background: #0d0f11;
    overflow: hidden;
    padding: 4px 6px;
    box-sizing: border-box;
  }
  .terminal-host :global(.xterm-viewport) {
    background: #0d0f11 !important;
  }
</style>
