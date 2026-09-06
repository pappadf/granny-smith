// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Voodoo2 WebGPU takeover's FALLBACK (proposal-voodoo2-
// webgpu-takeover §7 gate 6).  The same boot as voodoo2-webgpu.spec.ts,
// in a browser launched WITHOUT WebGPU: the "voodoo2_webgpu" kind is not
// offered there (machine.profile leaves it out), but named anyway it must
// fall back to the thread backend at creation and say so, and the overlay
// must never appear.  A file of its own because launchOptions are per file.

import { test, expect } from "@playwright/test";
import { BASE_ARGS, bootWithCard, probe } from "../helpers/voodoo2";

test.use({ launchOptions: { args: BASE_ARGS } });

test("voodoo2_webgpu falls back to the thread backend, honestly, at creation", async ({
  page,
}) => {
  test.setTimeout(6 * 60 * 1000);
  await bootWithCard(page, "voodoo2_webgpu");
  expect(await probe(page, "machine.pci.slot[1].card.regs.raster")).toBe(
    "thread",
  );
  // ...and the dialog would not have offered it: the profile omits the kind.
  expect(
    await probe(page, 'len(machine.profile("pm7500").pci_slots[0].cards)'),
  ).toBe("2");
  expect(await probe(page, "machine.pci.slot[1].card.regs.gpu_engaged")).toBe(
    "false",
  );
  await expect(page.locator("#screen3d")).toBeHidden();
});
