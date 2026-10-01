// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the skin.  A persisted skin is on <html> before any script of the
// app runs (index.html's pre-paint script), so the first frame is never the
// default; a skin switch at runtime restyles everything, the console's
// CodeMirror input included, without a reload; an unknown ?skin= falls back
// to the default skin.

import { test, expect, type Page } from "@playwright/test";

// The persisted skin, set before the page's own scripts.
async function prefs(page: Page, skin: string | null): Promise<void> {
  await page.addInitScript((k) => {
    if (k) localStorage.setItem("gs-skin", k);
    localStorage.setItem("gs-preview-notice-dismissed-v1", "1");
  }, skin);
}

// Load the app and wait for the core.
async function open(page: Page, url = "/index.html"): Promise<void> {
  await page.goto(url);
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
}

test("a persisted light skin is applied before the app runs", async ({
  page,
}) => {
  await prefs(page, "workbench-light");
  // Keep the app's own JavaScript from running at all: what is on screen
  // then is the pre-paint script and the stylesheet alone.
  await page.route("**/assets/*.js", (route) => route.abort());
  await page.goto("/index.html", { waitUntil: "domcontentloaded" });
  const html = await page.evaluate(() => ({
    skin: document.documentElement.dataset.skin,
    scheme: getComputedStyle(document.documentElement).colorScheme,
    bg: getComputedStyle(document.body).backgroundColor,
  }));
  expect(html.skin).toBe("workbench-light");
  expect(html.scheme).toBe("light");
  expect(html.bg).toBe("rgb(255, 255, 255)");
});

test("switching the skin restyles the page and the console input at runtime", async ({
  page,
}) => {
  await prefs(page, "workbench");
  await open(page);
  const input = page.locator(".cm-editor").first();
  await expect(input).toBeVisible();
  const before = await input.evaluate((el) => getComputedStyle(el).color);
  expect(before).toBe("rgb(204, 204, 204)");

  await page.locator('.gs-toolbar button[aria-label="Appearance"]').click();
  await page
    .locator(".gs-menu .gs-menu__item", { hasText: "Workbench Light" })
    .click();
  await expect(page.locator("html")).toHaveAttribute(
    "data-skin",
    "workbench-light",
  );
  await expect
    .poll(() => input.evaluate((el) => getComputedStyle(el).color))
    .toBe("rgb(51, 51, 51)");
  const meta = await page.evaluate(() =>
    document
      .querySelector('meta[name="color-scheme"]')
      ?.getAttribute("content"),
  );
  expect(meta).toBe("light");
  // The choice persists.
  expect(await page.evaluate(() => localStorage.getItem("gs-skin"))).toBe(
    "workbench-light",
  );
});

test("an unknown ?skin= falls back to the default skin", async ({ page }) => {
  await prefs(page, null);
  await open(page, "/index.html?skin=no-such-skin");
  await expect(page.locator("html")).toHaveAttribute("data-skin", "workbench");
});
