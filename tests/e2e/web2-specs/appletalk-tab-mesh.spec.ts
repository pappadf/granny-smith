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
// the machine runs on through it.

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

test("a hidden tab's machine keeps running", async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await bootPlus(page);
  await gsCallInPage(page, "scheduler.run", []);
  // What the browser does when the tab is hidden: document.hidden and
  // visibilitychange (the emulator's worker hears it through Emscripten)
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", { configurable: true, get: () => true });
    Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "hidden" });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  const before = Number(await gsEvalInPage(page, "scheduler.instr_count"));
  await expect
    .poll(async () => Number(await gsEvalInPage(page, "scheduler.instr_count")) - before, { timeout: 30_000 })
    .toBeGreaterThan(1_000_000);
});
