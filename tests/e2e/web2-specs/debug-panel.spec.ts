// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Debug view against a real machine (11-WORK-ORDER unit 0.4).
//
// Until this spec there was no e2e coverage of the Debug view at all, and its
// component tests mock the bus functions under test — so register editing
// (sent a shell statement as a gsEval path) and the Breakpoints panel (read a
// path that never resolved, and "removed" by adding a second breakpoint) were
// broken on every model with every test green.  Each case here drives the real
// UI and then asks the core, through the automation-only __gsEvalForTests
// hook, whether the action actually happened.

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gsEvalInPage } from "../helpers/web2-eval";

const PLUS_ROM = path.resolve(
  __dirname,
  "../../data/roms/plus-v3-4d1f8172.rom",
);

// Boot a Plus from a URL parameter (the ROM fetch is served from disk), wait
// for it to run, then pause it so the Debug view shows state.
async function bootPlusPaused(page: Page): Promise<void> {
  const body = fs.readFileSync(PLUS_ROM);
  await page.route("**/url-plus.rom", (route) =>
    route.fulfill({
      status: 200,
      contentType: "application/octet-stream",
      body,
    }),
  );
  await page.goto("/index.html?rom=url-plus.rom&model=plus");
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
  await expect(page.locator(".gs-statusbar .sb-state .label")).toHaveText(
    "Running",
    { timeout: 60_000 },
  );
  const cont = page.getByRole("button", { name: "Continue" });
  if (await cont.isVisible().catch(() => false)) await cont.click();
  await gsEvalInPage(page, "scheduler.stop");
  await expect(page.locator(".gs-statusbar .sb-state .label")).toHaveText(
    "Paused",
    { timeout: 15_000 },
  );
  await page.locator('button.ptab[data-tab="debug"]').click();
}

// Open one collapsible Debug section by its title.
async function openSection(page: Page, title: string): Promise<void> {
  const header = page.locator("header.header", { hasText: title });
  if ((await header.getAttribute("aria-expanded")) !== "true")
    await header.click();
}

test("a register edit reaches the core", async ({ page }) => {
  test.setTimeout(120_000);
  await bootPlusPaused(page);
  await openSection(page, "Registers");

  const d0 = page.getByLabel("D0 register value");
  await expect(d0).toBeVisible({ timeout: 15_000 });
  await d0.fill("00001234");
  await d0.press("Enter");

  await expect
    .poll(() => gsEvalInPage(page, "machine.cpu.d0"), { timeout: 10_000 })
    .toBe(0x1234);
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Failed to write" }),
  ).toHaveCount(0);
});

test("breakpoints are listed, and Remove removes", async ({ page }) => {
  test.setTimeout(120_000);
  await bootPlusPaused(page);
  await openSection(page, "Breakpoints");

  // Add through the section's own row.
  await page.locator('.add-btn[title="Add breakpoint"]').click();
  const addr = page.getByLabel("Breakpoint address");
  await addr.fill("0x400100");
  await addr.press("Enter");

  const rows = page.locator(".bp-row");
  await expect(rows).toHaveCount(1, { timeout: 10_000 });
  await expect(rows.first()).toContainText("00400100");
  expect(await gsEvalInPage(page, "debug.breakpoints.count")).toBe(1);

  // Adding the same address again must not stack a second entry.
  await page.locator('.add-btn[title="Add breakpoint"]').click();
  await addr.fill("0x400100");
  await addr.press("Enter");
  await expect
    .poll(() => gsEvalInPage(page, "debug.breakpoints.count"), {
      timeout: 10_000,
    })
    .toBe(1);
  await expect(rows).toHaveCount(1);

  // Remove through the row's context menu: gone from the list and the core.
  await rows.first().click({ button: "right" });
  await page.getByRole("menuitem", { name: "Remove" }).click();
  await expect(rows).toHaveCount(0, { timeout: 10_000 });
  expect(await gsEvalInPage(page, "debug.breakpoints.count")).toBe(0);
});
