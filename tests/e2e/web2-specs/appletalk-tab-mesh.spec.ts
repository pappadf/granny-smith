// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the AppleTalk tab mesh.
//
// Tabs of one browser profile whose machines have AppleTalk on link their
// routers to each other over a BroadcastChannel, with no user action
// (src/platform/wasm/em_atalk_tabs.c).  Three pages in one context each boot
// a Plus and nothing else:
//
//   - every page gets two open links, one to each of the others;
//   - each picked its own network number, so all three differ;
//   - every router learns the other two networks at distance 1;
//   - a page that closes is noticed: its link is lost on the others and its
//     network leaves their routing tables.
//
// A hidden tab keeps its machine running (the main loop switches from
// requestAnimationFrame to a timer); headless Chromium never hides a page,
// so the last test fires the visibilitychange the browser would and checks
// the machine runs on through it.  A tab the browser freezes is dropped by
// the others as suspended, and links again when it runs.

import { test, expect, type Page } from "../helpers/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";
import { gsCallInPage, gsEvalInPage } from "../helpers/web2-eval";

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms", "plus-v3-4d1f8172.rom");
const ROM_OPFS = "/opfs/images/rom/4d1f8172";

async function bootPlus(page: Page): Promise<void> {
  await gotoWeb2(page);
  await stageOpfsFile(page, ROM_OPFS, ROM);
  const r = await gsEvalInPage(page, "machine.boot", { model: "plus", rom: ROM_OPFS });
  expect(r, "machine.boot plus").not.toEqual(expect.objectContaining({ error: expect.anything() }));
  await expect.poll(async () => gsEvalInPage(page, "machine.id"), { timeout: 30_000 }).toBe("plus");
}

// The states of the page's router links, in slot order.
async function linkStates(page: Page): Promise<string[]> {
  const out: string[] = [];
  for (let i = 0; i < 16; i++) {
    const s = await gsEvalInPage(page, `appletalk.router.links[${i}].state`);
    if (typeof s === "string") out.push(s);
  }
  return out;
}

async function openLinks(page: Page): Promise<number> {
  return (await linkStates(page)).filter((s) => s === "open").length;
}

// The distance to network `net` in the page's routing table, or -1.
async function distanceTo(page: Page, net: number): Promise<number> {
  const st = await gsEvalInPage(page, `appletalk.router.routes["${net}"].state`);
  if (st !== "good" && st !== "suspect") return -1;
  return Number(await gsEvalInPage(page, `appletalk.router.routes["${net}"].distance`));
}

test("tabs with AppleTalk on link themselves and learn each other's networks", async ({ page, context }) => {
  test.setTimeout(4 * 60 * 1000);
  const pages = [page, await context.newPage(), await context.newPage()];
  for (const p of pages) await bootPlus(p);

  // Two links each, open, with no user action
  for (const p of pages) await expect.poll(() => openLinks(p), { timeout: 60_000 }).toBe(2);

  // Three different network numbers
  const nets = await Promise.all(pages.map(async (p) => Number(await gsEvalInPage(p, "appletalk.router.net"))));
  expect(new Set(nets).size).toBe(3);

  // Every router reaches the other two networks, one hop away
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      if (i !== j) await expect.poll(() => distanceTo(pages[i], nets[j]), { timeout: 60_000 }).toBe(1);

  // A tab that goes away is noticed by the others
  await pages[2].close();
  for (const p of pages.slice(0, 2)) await expect.poll(() => openLinks(p), { timeout: 60_000 }).toBe(1);
});

test("a hidden tab's machine keeps running at its pace", async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await bootPlus(page);
  await gsCallInPage(page, "scheduler.run", []);
  const rate = async () => {
    const t0 = Date.now();
    const i0 = Number(await gsEvalInPage(page, "scheduler.instr_count"));
    await page.waitForTimeout(4000);
    const i1 = Number(await gsEvalInPage(page, "scheduler.instr_count"));
    return (i1 - i0) / ((Date.now() - t0) / 1000);
  };
  await page.waitForTimeout(2000);
  const shown = await rate();
  // What the browser does when the tab is hidden: document.hidden and
  // visibilitychange (the emulator's worker hears it through Emscripten).
  // The loop then paces itself (postTask + Atomics.wait), not on RAF or a
  // timer, which a real hidden tab pauses or throttles.
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", { configurable: true, get: () => true });
    Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "hidden" });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  await page.waitForTimeout(1000);
  const hidden = await rate();
  expect(hidden, `hidden ${hidden.toFixed(0)}/s against shown ${shown.toFixed(0)}/s`).toBeGreaterThan(shown * 0.5);

  // Shown again, the page's input still reaches the emulator thread: hiding
  // must not end the thread's runtime (pausing Emscripten's main loop did,
  // and the next mouse or key event aborted: "emscripten_proxy_async failed")
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", { configurable: true, get: () => false });
    Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "visible" });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  await page.waitForTimeout(500);
  const box = await page.locator("#screen").boundingBox();
  expect(box).not.toBeNull();
  for (let i = 0; i < 20; i++) await page.mouse.move(box!.x + 20 + i * 5, box!.y + 20 + i * 3);
  await page.mouse.click(box!.x + 100, box!.y + 100);
  await page.keyboard.press("a");
  await page.waitForTimeout(1000);
  await expect(page.getByText(/Aborted\(/)).toHaveCount(0);
  expect(await rate(), "the machine runs on after showing").toBeGreaterThan(shown * 0.5);
});

// The browser freezes a long-hidden tab (Page Lifecycle "freeze"): nothing
// in it runs, its worker included.  The page says so on the mesh as it
// freezes, so the other tabs drop its link at once, saying why, instead of
// after seven seconds of silence; when it runs again its hellos link it
// again.  Headless Chromium fires no freeze event (CDP's
// Page.setWebLifecycleState neither fires it nor stops the worker), so the
// test fires the event the browser would; the worker, not really frozen,
// keeps saying hello, which is the resume.
test("a tab the browser freezes is dropped as suspended, and links again", async ({ page, context }) => {
  test.setTimeout(4 * 60 * 1000);
  const other = await context.newPage();
  for (const p of [page, other]) await bootPlus(p);
  await gsCallInPage(page, "scheduler.run", []); // the page shows a running Mac's Network tab
  for (const p of [page, other]) await expect.poll(() => openLinks(p), { timeout: 60_000 }).toBe(1);
  // The page learns its network number by polling (every 2 s): let it
  await other.waitForTimeout(3_000);

  await other.evaluate(() => document.dispatchEvent(new Event("freeze")));
  const reasons = async () => {
    const out: string[] = [];
    for (let i = 0; i < 16; i++) {
      const r = await gsEvalInPage(page, `appletalk.router.links[${i}].reason`);
      if (typeof r === "string" && r) out.push(r);
    }
    return out;
  };
  await expect
    .poll(reasons, { timeout: 5_000 })
    .toContain("the other tab was suspended by the browser");
  // Back: one open link again, and the suspension is history
  await expect.poll(() => openLinks(page), { timeout: 30_000 }).toBe(1);
  await page.getByRole("tab", { name: "Network" }).click();
  await expect(page.getByTestId("network-tab-peer")).toHaveCount(1, { timeout: 10_000 });
  await expect(page.getByTestId("network-tab-suspended")).toHaveCount(0);
});
