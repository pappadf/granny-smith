// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Rage 128's WebGPU TAKEOVER.
//
// With raster=webgpu the emulator thread translates the 3D engine's
// triangles into records a GPU worker turns into WebGPU render passes
// (rage128_gpu.c, rage128Gpu.worker.ts); the colour and Z surfaces live on
// the GPU, and every other access to their VRAM — the CPU through the
// apertures included — first reads the GPU's rows back.  So the native
// rage128-3d row, whose every assertion is an equality on VRAM read
// through the aperture, is the oracle: replayed here with the takeover
// engaged from the first draw (gpu=always), its assertions read the GPU's
// pixels.  Batches the GPU path does not take (points, lines, stencil,
// 24-bit Z) run on the walker against fenced VRAM, so the replay covers
// the hand-over in both directions as well.
//
// Runs on Chromium's software WebGPU adapter (swiftshader).  The scheduler
// stays HALTED: headless Chromium destroys the WebGPU device on the first
// canvas present (voodoo2-webgpu.spec.ts), so no vblank may come; the
// present pass is a real-browser matter.

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";
import { gsCallInPage, gsEvalInPage } from "../helpers/web2-eval";
import { BASE_ARGS, WEBGPU_ARGS, terminalRun } from "../helpers/voodoo2";

test.use({ launchOptions: { args: [...BASE_ARGS, ...WEBGPU_ARGS] } });

const DATA = path.resolve(__dirname, "../../data");
const TNT_ROM = path.join(DATA, "roms", "pm7500-pm8500-pm9500-96cd923d.rom");
const R128_PROM = path.join(DATA, "roms", "rage128-xclaimvr128-108-d0c84d42.prom");
const STORED_ROM = "/opfs/images/rom/96cd923d-c241cd82bf90797a";
const ROW = path.resolve(__dirname, "../../integration/rage128-3d/test.script");
const DONE = "rage128-webgpu: the 3D row's drawing assertions hold on the GPU";

async function upload(page: Page, file: string, toast: string): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await chooser.setFiles(file);
  await expect(page.locator(".toast .msg").filter({ hasText: toast })).toBeVisible({
    timeout: 60_000,
  });
}

// The row's script from its helpers through its last drawing assertion:
// the boot is the spec's own, and the DAC section (screenshots, a
// scratch directory) and the native-only kinds section are left out.
// The 2D engine's solid fills of a GPU surface are GPU fills (a Z buffer
// cleared every frame stays on the GPU).  The row's own clears all come
// while its surfaces are shorter than the 64-row fill, so they run in
// VRAM; this section grows both surfaces first (a Z-tested triangle
// reaching row 100, Z test ALWAYS), then clears colour and Z with the row's helpers and
// reads both sides of the filled rectangle back.
const FILLS = `
echo "section fills"
setup3d()
wr(0x1C90, $ZBUF)
wr(0x1C94, 64)
wr(0x1C98, (7 << 4))
wr(0x1C9C, 0x3)
let F100 = 0x42C80000
tri($F0, $F0, 0xFF0000FF, $F100, $F0, 0xFF0000FF, $F0, $F100, 0xFF0000FF, $F0_5)
clear32(0x00336699)
clearz16(0x1234)
assert px(5, 5) == 0x00336699 "a 2D fill of a GPU colour surface"
assert px(63, 63) == 0x00336699 "...over the whole rectangle"
assert px(70, 5) == 0xFF0000FF "...and only it: the triangle beside it stays"
assert (vram_rd($ZBUF + 5 * 1024 + 5 * 2) & 0xFFFF) == 0x1234 "a 2D fill of a GPU Z surface"
assert (vram_rd($ZBUF + 5 * 1024 + 70 * 2) & 0xFFFF) == 0x7FFF "...and only it: the triangle's Z beside it stays"
wr(0x1C9C, 0)
`;

function composeScript(): string {
  const row = fs.readFileSync(ROW, "utf8");
  const boot = row.indexOf('machine.boot model="pm9500"');
  const dac = row.indexOf("# --- 13. The palette is the DAC table");
  if (boot < 0 || dac < 0) throw new Error("rage128-3d's layout moved: update the spec's cut points");
  // Each numbered section announces itself, so a failure or a stall
  // names where it is.
  const body = row
    .slice(row.indexOf("\n", boot) + 1, dac)
    .replace(/^# --- (\d+\.[^\n-]*)/gm, (m, title: string) => `echo "section ${title.trim()}"\n${m}`);
  return `${body}\n${FILLS}\necho "${DONE}"\n`;
}

test("the 3D engine's triangles drawn on the GPU read back as the walker drew them", async ({
  page,
}) => {
  test.setTimeout(15 * 60 * 1000);
  // The GPU worker's own complaints (a shader or pipeline error) belong in
  // the report when something fails.
  const gpuLog: string[] = [];
  page.on("console", (m) => {
    if (/rage ?128|webgpu|wgsl/i.test(m.text())) gpuLog.push(`${m.type()}: ${m.text()}`);
  });
  await gotoWeb2(page);
  await upload(page, TNT_ROM, "pm7500-pm8500-pm9500-96cd923d.rom uploaded");
  // The toast naming the card is the "identified it" signal.
  await upload(page, R128_PROM, "PCI expansion ROM for 'rage128'");
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });

  await gsCallInPage(page, "machine.boot", {
    model: "pm9500",
    ram: 32768,
    rom: STORED_ROM,
    pci_card: "rage128",
    pci_option: "raster=webgpu,gpu=always",
  });
  await gsEvalInPage(page, "scheduler.stop");
  expect(await gsEvalInPage(page, "machine.pci.slot[1].card.regs.raster")).toBe("webgpu");
  expect(await gsEvalInPage(page, "machine.pci.slot[1].card.regs.gpu_engaged")).toBe(false);

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".console")).toBeVisible({ timeout: 15_000 });
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "r128gpu-"));
  const scriptPath = path.join(tmp, "rage128-3d-gpu.script");
  fs.writeFileSync(scriptPath, composeScript());
  await stageOpfsFile(page, "/opfs/upload/rage128-3d-gpu.script", scriptPath);
  await terminalRun(page, 'include "/opfs/upload/rage128-3d-gpu.script"');

  let text = "";
  for (let attempt = 0; attempt < 480; attempt++) {
    await page.waitForTimeout(1000);
    if (await page.getByText("The emulator stopped").isVisible()) {
      console.log(`[rage128-webgpu] the emulator stalled after:\n${text.slice(-1500)}\n${gpuLog.join("\n")}`);
      throw new Error("the emulator stopped answering (a GPU wait held the core past the heartbeat)");
    }
    text = await page.locator(".console-output").innerText({ timeout: 5_000 });
    if (text.includes(DONE)) break;
    if (/ASSERT FAILED|assert(ion)? failed|include: .* failed/.test(text)) break;
  }
  console.log(`[rage128-webgpu] console tail:\n${text.slice(-1500)}`);
  const stats = String(
    await Promise.race([
      gsEvalInPage(page, "machine.pci.slot[1].card.regs.gpu_stats"),
      new Promise((resolve) => setTimeout(() => resolve("(no answer: the core is busy)"), 20_000)),
    ]),
  );
  console.log(`[rage128-webgpu] ${stats}`);
  if (gpuLog.length) console.log(gpuLog.join("\n"));
  expect(text, text).toContain(DONE);
  expect(text, text).not.toMatch(/ASSERT FAILED|assert(ion)? failed/);
  // The GPU drew: triangles went through the takeover and the aperture
  // reads came back through readbacks, not the walker.
  expect(stats).toMatch(/ tris=[1-9]/);
  expect(stats).toMatch(/ readback_bands=[1-9]/);
  expect(stats).toMatch(/ fills=([2-9]|[1-9][0-9]+) /);
  expect(stats).toMatch(/ lost=0/);
});
