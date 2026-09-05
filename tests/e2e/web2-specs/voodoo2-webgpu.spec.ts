// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Voodoo2's WebGPU TAKEOVER (proposal-voodoo2-webgpu-
// takeover §7, gates 2 and 3).
//
// raster=webgpu is the browser's alternative to the software walker: the
// emulator's raster pthread translates the card's command stream into
// records a GPU worker turns into WebGPU render passes, and the GPU's
// own rasteriser draws the guest's triangles onto an overlay canvas.
// Nothing native can see any of it, so this spec boots a 7500 with the
// card in GPU mode on Chromium's software WebGPU adapter (swiftshader),
// replays 3dfx's bring-up through the aperture, takes the monitor (the
// edge that ENGAGES GPU mode), and runs the takeover's drawing section
// (tests/integration/tnt-pci-voodoo2/draw-webgpu.script): the 136-pixel
// triangle must cover exactly 136 pixels on the GPU — the half-pixel
// alignment that makes the GPU's centre test the walker's integer test
// — its complement exactly 120 more with no seam, and every pixel value
// comes back through LFB reads, i.e. through readbacks of the GPU's own
// attachments.  The sibling voodoo2-webgpu-fallback.spec.ts runs the
// same boot in a browser WITHOUT WebGPU.
//
// Everything goes through the shipped Terminal panel; the drawing section
// is staged into OPFS and `include`d, so it runs inside the worker at
// full speed instead of one register per terminal line.
//
// The scheduler stays HALTED throughout, deliberately: headless Chromium
// destroys the WebGPU device on the first canvas present (measured on
// both the headless shell and the full binary, worker or main thread),
// so a vblank here would drop the card back to the walker.  Everything
// up to the present pass is exercised; the present itself is a real-
// browser matter (voodoo2.md, "The WebGPU takeover").

import { test, expect } from "@playwright/test";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { stageOpfsFile } from "../helpers/web2-fs";
import {
  BASE_ARGS,
  WEBGPU_ARGS,
  bootWithCard,
  composeScript,
  probe,
  terminalRun,
} from "../helpers/voodoo2";

test.use({ launchOptions: { args: [...BASE_ARGS, ...WEBGPU_ARGS] } });

test("the takeover engages when the card takes the monitor, and the GPU covers exactly the walker pixels", async ({
  page,
}) => {
  test.setTimeout(12 * 60 * 1000);
  await bootWithCard(page, "webgpu");

  // The backend the browser build installed: "webgpu" means the page
  // had a device and the GPU worker attached within the create-time
  // wait; "thread" would be the fallback this test exists to catch.
  expect(await probe(page, "machine.pci.slot[1].card.regs.raster")).toBe(
    "webgpu",
  );
  expect(await probe(page, "machine.pci.slot[1].card.regs.gpu_engaged")).toBe(
    "false",
  );
  await expect(page.locator("#screen3d")).toBeHidden();

  // Stage the composed script and run it inside the worker.
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "v2gpu-"));
  const scriptPath = path.join(tmp, "takeover.script");
  fs.writeFileSync(scriptPath, composeScript());
  await stageOpfsFile(page, "/opfs/upload/voodoo2-takeover.script", scriptPath);
  await terminalRun(page, 'include "/opfs/upload/voodoo2-takeover.script"');

  // The script's last line, or its first failed assertion.
  let text = "";
  for (let attempt = 0; attempt < 240; attempt++) {
    await page.waitForTimeout(1000);
    text = await page.locator(".xterm-rows").innerText();
    if (text.includes("draw-webgpu: takeover drawing section complete")) break;
    if (/assert(ion)? failed|error:|Error/.test(text)) break;
  }
  expect(text, text).toContain(
    "draw-webgpu: takeover drawing section complete",
  );
  expect(text, text).not.toMatch(/assert(ion)? failed/);

  // The overlay is what the user sees: shown exactly while engaged.
  await expect(page.locator("#screen3d")).toBeVisible();
  expect(await probe(page, "machine.pci.slot[1].card.regs.gpu_engaged")).toBe(
    "true",
  );

  // Release the monitor: GPU mode disengages, reading the frame back,
  // and the overlay hides.  (The script's shell functions persist.)
  await terminalRun(page, "vreg_wr(0x210, vreg_rd(0x210) & 0xFFFFFFFE)");
  await terminalRun(page, 'echo "w=${machine.screen.width}"');
  expect(
    await probe(page, "machine.pci.slot[1].card.video.drives_monitor"),
  ).toBe("false");
  expect(await probe(page, "machine.pci.slot[1].card.regs.gpu_engaged")).toBe(
    "false",
  );
  await expect(page.locator("#screen3d")).toBeHidden();
});
