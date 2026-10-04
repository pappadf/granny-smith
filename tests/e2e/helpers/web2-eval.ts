// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Read or call the object model from a spec without typing into the terminal.
// Under automation web2 installs `window.__gsEvalForTests` (bus/testHook.ts), a
// thin wrapper over the same gsEval the UI uses; it is installed once the
// bridge is ready, so call this after gotoWeb2().

import { expect, type Page } from "@playwright/test";

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
          "__gsEvalForTests missing: is the page under automation and the bridge ready?",
        );
      return hook(path, args);
    },
    { path, args },
  );
}

// gsEvalInPage for a call that must succeed: the spec fails on a refusal
// ({ error }) with the path in the message, instead of on a later read.
export async function gsCallInPage(
  page: Page,
  path: string,
  args?: unknown[] | Record<string, unknown>,
): Promise<unknown> {
  const r = await gsEvalInPage(page, path, args);
  expect(r, path).not.toEqual(
    expect.objectContaining({ error: expect.anything() }),
  );
  return r;
}

// Every file under the scratch area, as paths relative to it: each tab writes
// in its own directory there (app/web2 lib/opfsPaths.ts), so "nothing left in
// scratch" means no file at any depth, not an empty listing.
export async function scratchFiles(page: Page): Promise<string[]> {
  const out: string[] = [];
  async function walk(rel: string): Promise<void> {
    const dir = rel ? `/opfs/upload/.scratch/${rel}` : "/opfs/upload/.scratch";
    const entries = (await gsEvalInPage(page, "files.list", [dir])) as
      { name: string; kind: string }[] | { error: string };
    if (!Array.isArray(entries)) return;
    for (const e of entries) {
      if (e.name === "." || e.name === "..") continue;
      const p = rel ? `${rel}/${e.name}` : e.name;
      if (e.kind === "directory") await walk(p);
      else out.push(p);
    }
  }
  await walk("");
  return out;
}
