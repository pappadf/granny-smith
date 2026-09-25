// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Read or call the object model from a spec without typing into the terminal.
// Under automation web2 installs `window.__gsEvalForTests` (bus/testHook.ts), a
// thin wrapper over the same gsEval the UI uses; it is installed once the
// bridge is ready, so call this after gotoWeb2().

import { type Page } from '@playwright/test';

// gsEval(path, args) inside the page; resolves to the core's JSON answer —
// a value, null (a method that returns nothing), or { error }.
export async function gsEvalInPage(
  page: Page,
  path: string,
  args?: unknown[] | Record<string, unknown>,
): Promise<unknown> {
  return page.evaluate(
    async ({ path, args }) => {
      const hook = (
        window as {
          __gsEvalForTests?: (p: string, a?: unknown) => Promise<unknown>;
        }
      ).__gsEvalForTests;
      if (!hook)
        throw new Error(
          '__gsEvalForTests missing: is the page under automation and the bridge ready?',
        );
      return hook(path, args);
    },
    { path, args },
  );
}
