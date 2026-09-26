// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Drive the Terminal panel's xterm the way a user does: focus it, type a
// line, press Enter.  Typed input goes through xterm's onData and the pane
// queues it while a command runs, so a line typed behind a busy one is run
// after it rather than lost.  One copy of what used to live in every spec.

import { type Page } from "@playwright/test";

// Focus xterm's own hidden input and WAIT until it really is the active
// element.  `.xterm` click() resolves when the click is DISPATCHED, not when
// xterm has taken focus, and with an AV machine plus a live AudioWorklet on
// the main thread that gap is wide: CI #636 lost the first 17 characters of
// a line that way.
export async function focusTerminal(page: Page): Promise<void> {
  const ta = page.locator(".xterm textarea.xterm-helper-textarea");
  if ((await ta.count()) > 0) {
    await ta.focus();
    await page.waitForFunction(
      () =>
        document.activeElement instanceof HTMLTextAreaElement &&
        document.activeElement.classList.contains("xterm-helper-textarea"),
      undefined,
      { timeout: 15_000 },
    );
    return;
  }
  await page.locator(".xterm").click();
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
