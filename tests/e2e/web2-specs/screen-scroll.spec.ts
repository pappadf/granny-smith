// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a zoomed screen larger than the display scrolls to every edge.
//
// The display centred its oversized screen with flex alignment, which puts
// half the overflow at negative offsets that scrolling never reaches: the
// top rows (the menu bar) and the left columns were out of reach (#279).
// At the scroll origin the screen's top-left corner must be inside the
// scroll box, and at the far end its bottom-right corner must be.

import { test, expect } from "../helpers/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";

const PLUS_ROM = path.resolve(
  __dirname,
  "../../data/roms/plus-v3-4d1f8172.rom",
);

test("a zoomed screen scrolls to its top-left and bottom-right edges", async ({
  page,
}) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);
  // Drop the ROM on the display: it boots a default machine, live, as a
  // user's drop does (only the drag gesture is synthetic -- see
  // display-drop.spec.ts).
  const rom = fs.readFileSync(PLUS_ROM).toString("base64");
  await page.evaluate((data) => {
    const el = document.querySelector(".gs-display-content, .screen-view");
    if (!el) throw new Error("display area not found");
    const r = el.getBoundingClientRect();
    const bin = atob(data);
    const buf = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    const dt = new DataTransfer();
    dt.items.add(new File([buf], "plus-v3-4d1f8172.rom"));
    for (const type of ["dragenter", "dragover", "drop"])
      el.dispatchEvent(
        new DragEvent(type, {
          bubbles: true,
          cancelable: true,
          dataTransfer: dt,
          clientX: r.x + r.width / 2,
          clientY: r.y + r.height / 2,
        }),
      );
  }, rom);
  await expect(
    page
      .locator(".toast .msg")
      .filter({ hasText: "Booted plus from the loaded ROM" }),
  ).toBeVisible({ timeout: 60_000 });

  // 400% of 512x342 is 2048x1368: larger than the display on both axes.
  const zoom = page.getByRole("textbox", { name: "Zoom level" });
  await zoom.fill("400%");
  await zoom.press("Enter");

  const view = page.locator(".screen-view");
  const frame = page.locator(".screen-wrap");
  await expect
    .poll(async () => (await frame.boundingBox())?.width ?? 0, {
      timeout: 10_000,
    })
    .toBeGreaterThan((await view.boundingBox())!.width);

  // The scroll origin shows the top-left corner.
  await view.evaluate((el) => el.scrollTo(0, 0));
  const v0 = (await view.boundingBox())!;
  const f0 = (await frame.boundingBox())!;
  expect(f0.x).toBeGreaterThanOrEqual(v0.x - 1);
  expect(f0.y).toBeGreaterThanOrEqual(v0.y - 1);

  // The far end of the scroll range shows the bottom-right corner.
  await view.evaluate((el) => el.scrollTo(el.scrollWidth, el.scrollHeight));
  const f1 = (await frame.boundingBox())!;
  expect(f1.x + f1.width).toBeLessThanOrEqual(v0.x + v0.width + 1);
  expect(f1.y + f1.height).toBeLessThanOrEqual(v0.y + v0.height + 1);

  // Back at 100% the screen fits and sits centred, as before.
  await zoom.fill("100%");
  await zoom.press("Enter");
  await expect
    .poll(async () => {
      const v = (await view.boundingBox())!;
      const f = (await frame.boundingBox())!;
      return Math.abs(f.x - v.x - (v.x + v.width - (f.x + f.width)));
    })
    .toBeLessThanOrEqual(2);
});
