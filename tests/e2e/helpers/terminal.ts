// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Drive the Terminal panel's console the way a user does: focus its input,
// type a line, press Enter.  What is typed while a command runs is queued
// and run after it rather than lost.  One copy of what used to live in
// every spec.

import { type Page } from "@playwright/test";

// The console's input (CodeMirror's contenteditable) and its output list.
export const CONSOLE_INPUT = ".console .cm-content";
export const CONSOLE_OUTPUT = ".console-output";

// Focus the console input and WAIT until it really is the active element:
// a click resolves when it is DISPATCHED, not when focus has moved, and
// with an AV machine plus a live AudioWorklet on the main thread that gap
// is wide (CI #636 once lost the first 17 characters of a line that way).
export async function focusTerminal(page: Page): Promise<void> {
  const input = page.locator(CONSOLE_INPUT);
  await input.focus();
  await page.waitForFunction(
    () => document.activeElement?.classList.contains("cm-content") === true,
    undefined,
    { timeout: 15_000 },
  );
}

// The console's current prompt text.
export async function consolePrompt(page: Page): Promise<string> {
  return ((await page.locator(".console-prompt").textContent()) ?? "").trim();
}

// The console's input line as shown -- the prompt, then what is typed --
// or "" while a command runs (the line has not returned yet).
export async function consoleLine(page: Page): Promise<string> {
  if ((await page.locator(".console.busy").count()) > 0) return "";
  const typed = (await page.locator(CONSOLE_INPUT).innerText()).replace(/\s+$/, "");
  return `${await consolePrompt(page)} ${typed}`.trim();
}

export interface TerminalRunOptions {
  // Per-key typing delay in ms.
  delay?: number;
  // Wait this long after Enter (for specs that read the output right away).
  settleMs?: number;
}

// Type one shell line into the terminal and submit it.
export async function terminalRun(
  page: Page,
  line: string,
  opts: TerminalRunOptions = {},
): Promise<void> {
  await focusTerminal(page);
  await page.keyboard.type(line, opts.delay ? { delay: opts.delay } : undefined);
  await page.keyboard.press("Enter");
  if (opts.settleMs) await page.waitForTimeout(opts.settleMs);
}

// A fresh key per echo, shared by every probe in the run, so the echoed
// input line (still holding the unexpanded ${...}) or an earlier answer can
// never satisfy the match -- only this response can.
let echoSeq = 0;

// How long the terminal is given to answer an echo before its output is read.
const ECHO_WAIT_MS = 400;

// Echo `body` (shell text with ${...} splices) behind a fresh key, once, and
// match the answer against `value` (a regex source with capture groups).
// Returns the match, or null when the answer has not shown up yet.
export async function terminalEcho(
  page: Page,
  body: string,
  value: string,
): Promise<RegExpMatchArray | null> {
  const key = `te${++echoSeq}`;
  await terminalRun(page, `echo "${key}=${body}"`, { settleMs: 250 });
  await page.waitForTimeout(ECHO_WAIT_MS);
  const text = await page.locator(CONSOLE_OUTPUT).innerText();
  return text.match(new RegExp(`${key}=${value}`));
}

export interface TerminalEvalOptions {
  // Echoes to try before giving up.
  attempts?: number;
  // The value pattern (a regex source without groups).  The default
  // rejects a value holding a space or a colon: such a probe times out.
  value?: string;
}

// Read one expression's value back through the terminal, retrying until the
// answer shows (a line typed while the machine starts can be lost).
export async function terminalEval(
  page: Page,
  expr: string,
  opts: TerminalEvalOptions = {},
): Promise<string> {
  const value = opts.value ?? "[A-Za-z0-9_.-]+";
  for (let attempt = 0; attempt < (opts.attempts ?? 30); attempt++) {
    const m = await terminalEcho(page, `[\${${expr}}]`, `\\[(${value})\\]`);
    if (m) return m[1];
  }
  throw new Error(`terminal probe never answered: ${expr}`);
}
