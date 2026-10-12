// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: ?hd0=blank:<size> (#281).  A URL boot can attach a new blank
// hard disk instead of downloading one: the page creates it with
// files.hd_create (the core parses the size, as for the shell and the New
// Machine dialog) under /opfs/images/hd/, named for the link
// (blank_<spec>_<slot>_<hash>.dmg), and attaches it.  Reloading the same link
// attaches that disk again instead of creating another.  Only the ROM's
// transport is stubbed (page.route, as url-boot.spec.ts does); creating,
// storing and attaching run for real.

import { test, expect, type Page } from "../helpers/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gsEvalInPage } from "../helpers/web2-eval";

const PLUS_ROM = path.resolve(
  __dirname,
  "../../data/roms/plus-v3-4d1f8172.rom",
);
const BLANK = /^\/opfs\/images\/hd\/blank_20mb_hd0_[0-9a-f]{8}\.dmg$/;

// Serve the Plus ROM for the sentinel path the URL points at.
async function routeRom(page: Page): Promise<void> {
  const body = fs.readFileSync(PLUS_ROM);
  await page.route("**/url-blank-plus.rom", (route) =>
    route.fulfill({
      status: 200,
      contentType: "application/octet-stream",
      body,
    }),
  );
}

// Load the link and wait for the machine it boots.
async function bootFromUrl(page: Page, query: string): Promise<void> {
  await page.goto(`/index.html?${query}`);
  await expect(
    page
      .locator(".toast .msg")
      // "… without <disk>: <why>" when a disk could not be had.
      .filter({ hasText: /Booted plus (from URL parameters|without )/ }),
  ).toBeVisible({ timeout: 60_000 });
  await expect(page.locator(".gs-statusbar .sb-state .label")).toHaveText(
    "Running",
    {
      timeout: 15_000,
    },
  );
}

// The blank disks in the hard-disk store.
async function blankDisks(page: Page): Promise<string[]> {
  const entries = (await gsEvalInPage(page, "files.list", [
    "/opfs/images/hd",
  ])) as { name: string; kind: string }[] | { error: string };
  if (!Array.isArray(entries)) return [];
  return entries
    .filter((e) => e.kind !== "directory" && e.name.startsWith("blank_"))
    .map((e) => `/opfs/images/hd/${e.name}`);
}

test("?hd0=blank:20mb attaches a new blank disk, and a reload reuses it", async ({
  page,
}) => {
  test.setTimeout(180_000);
  await routeRom(page);
  const query = "rom=url-blank-plus.rom&model=plus&hd0=blank:20mb";

  await bootFromUrl(page, query);
  // The Plus's hd0 is SCSI ID 0; its image is the blank disk, stored in OPFS
  // (image.path is the machine's delta; image.filename the image itself).
  expect(await gsEvalInPage(page, "machine.scsi.device[0].image.present")).toBe(
    true,
  );
  const attached = await gsEvalInPage(
    page,
    "machine.scsi.device[0].image.filename",
  );
  expect(attached).toMatch(BLANK);
  expect(await blankDisks(page)).toEqual([attached]);
  // 20mb snaps to the smallest model at or above 20,000,000 bytes, an
  // HD20SC of 21307392 bytes (the core's catalog), read back from the UDIF.
  expect(
    Number(await gsEvalInPage(page, "machine.scsi.device[0].image.raw_size")),
  ).toBe(21307392);

  // The same link again: the disk it created is attached, no second one made.
  await bootFromUrl(page, query);
  expect(
    await gsEvalInPage(page, "machine.scsi.device[0].image.filename"),
  ).toBe(attached);
  expect(await blankDisks(page)).toEqual([attached]);
});

// A spec the core cannot create is reported like a failed download: the
// machine boots without that disk and the page says why.
test("?hd0=blank: with an unknown model boots without the disk and says why", async ({
  page,
}) => {
  test.setTimeout(120_000);
  await routeRom(page);
  await bootFromUrl(
    page,
    "rom=url-blank-plus.rom&model=plus&hd0=blank:HD999SC",
  );
  await expect(
    page
      .locator(".toast .msg")
      .filter({ hasText: 'HD0: could not create a blank disk of "HD999SC"' }),
  ).toBeVisible();
  expect(await blankDisks(page)).toEqual([]);
});
