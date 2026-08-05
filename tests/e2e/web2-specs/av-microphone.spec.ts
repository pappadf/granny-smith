// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the AV microphone control and the browser mic → sound-in path
// (C side: src/platform/wasm/em_audio_in.c).
//
// This spec exists because the bug it now guards could not be seen from
// outside. The guest's recorder gains up hard, so it amplifies the codec's
// own ~1 LSB dither into full-scale white noise — meaning a capture path
// delivering NOTHING sounds exactly like one delivering garbage. Three
// separate fixes were aimed at the wrong layer before anyone ran the thing
// in a browser. Nothing short of a real getUserMedia + AudioWorklet +
// shared-heap round trip can tell those two apart, so that is what this does.
//
// Runs against Chromium's fake capture device (--use-fake-device-for-media-
// stream synthesises a tone; --use-fake-ui-for-media-stream auto-grants the
// prompt), so no hardware is involved and it is CI-safe.
//
// What it proves, in order:
//   1. The mic button is capability-gated: absent on a Plus, present on the
//      840AV (capabilities.audio_in).
//   2. Clicking it drives the core: machine.audioin.source flips none → host
//      and `connected` follows the shared-heap flag em_audio_in.c publishes.
//   3. Samples REACH GUEST RAM. The input engine is driven the way the
//      Singer driver would (sndInBase / sndSize / pSndInEn), exactly as
//      suite-av's av-audio-in-device row does with the deterministic tone
//      source — no OS boot needed — and the guest's input double buffer is
//      then checked for actual audio.
//
// (3) is the one that matters. Steps 1-2 passed the whole time the feature
// was completely dead.

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";

const DATA = path.resolve(__dirname, "../../data");
const AV_ROM = path.join(DATA, "roms", "q840av-q660av-5bf10fd1.rom");
const PLUS_ROM = path.join(DATA, "roms", "plus-v3-4d1f8172.rom");

// Per-spec launchOptions REPLACE the config array, so the swiftshader flags
// (web2 will not mount without WebGL2) are re-listed alongside the fake
// capture device.
test.use({
  launchOptions: {
    args: [
      "--use-gl=angle",
      "--use-angle=swiftshader-webgl",
      "--ignore-gpu-blocklist",
      "--disable-dev-shm-usage",
      "--use-fake-device-for-media-stream",
      "--use-fake-ui-for-media-stream",
    ],
  },
});

async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator(".xterm");
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press("Enter");
}

// See av-camera.spec.ts for why the answer is bracketed on both sides.
async function readKey(page: Page, key: string): Promise<string | null> {
  const text = await page.locator(".xterm-rows").innerText();
  const re = new RegExp(`${key}=([^=${"${}"}\\s]+)=${key}`);
  for (const line of text.split("\n")) {
    const m = line.trim().match(re);
    if (m) return m[1];
  }
  return null;
}

let probeSeq = 0;
async function probe(
  page: Page,
  expr: string,
  timeoutMs = 10_000,
): Promise<string | null> {
  const key = `q${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${${expr}}=${key}"`);
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await readKey(page, key);
    if (v !== null) return v;
    if (Date.now() > deadline) return null;
    await page.waitForTimeout(250);
  }
}

async function bootModel(
  page: Page,
  romFile: string,
  model: string,
): Promise<void> {
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await romChooser.setFiles(romFile);

  await page.getByRole("button", { name: "New Machine..." }).click();
  const sel = page.locator("#cfg-model");
  await expect(sel.locator(`option[value="${model}"]`)).toHaveCount(1, {
    timeout: 30_000,
  });
  await sel.selectOption(model);
  await page.getByRole("button", { name: "Start Machine" }).click();
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Machine started" }),
  ).toBeVisible({
    timeout: 60_000,
  });
}

test("AV microphone control delivers browser audio into guest RAM", async ({
  page,
}) => {
  test.setTimeout(240_000);
  // Surface what the page says: a silent failure in the capture graph is
  // exactly what made this bug invisible from the outside.
  page.on("console", (m) => console.log(`  [page:${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.log(`  [pageerror] ${e.message}`));
  await gotoWeb2(page);
  await stageOpfsFile(
    page,
    "/opfs/images/rom/q840av-q660av-5bf10fd1.rom",
    AV_ROM,
  );

  // --- 1. Capability gating: a Plus has no Singer, so no microphone button.
  await bootModel(page, PLUS_ROM, "plus");
  const micBtn = page.locator('.gs-toolbar button[aria-label*="icrophone"]');
  await expect(micBtn).toHaveCount(0);

  // --- 2. The 840AV has one.
  await page.getByRole("button", { name: "Shut down" }).click();
  await bootModel(page, AV_ROM, "q840av");
  await expect(micBtn).toHaveCount(1, { timeout: 30_000 });
  await expect(micBtn).toHaveAttribute("aria-pressed", "false");

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".xterm")).toBeVisible({ timeout: 15_000 });
  await expect
    .poll(async () => probe(page, "machine.id"), { timeout: 30_000 })
    .toBe("q840av");

  expect(await probe(page, "machine.audioin.source")).toBe("none");
  expect(await probe(page, "machine.audioin.connected")).toBe("false");

  // --- 3. Connect the microphone.
  await micBtn.click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "true");
  await expect
    .poll(async () => probe(page, "machine.audioin.source"))
    .toBe("host");
  await expect
    .poll(async () => probe(page, "machine.audioin.connected"), {
      timeout: 30_000,
    })
    .toBe("true");

  // --- 4. Drive the Singer input engine the way its driver would: input
  // double buffer at $10000, 240-frame halves, pSndInEn on. This is what
  // makes the guest actually pull, and it is also what flips pSndInEn, so
  // the browser track attaches (the mic is live only while the guest is
  // genuinely recording).
  // Typing through the xterm is not always reliable on the first attempt,
  // so each poke is verified by reading the register back.
  for (let attempt = 0; attempt < 5; attempt++) {
    await terminalRun(page, "machine.memory.poke.l 0x50F31210 0x10000");
    await terminalRun(page, "machine.memory.poke.w 0x50F31218 240");
    await terminalRun(page, "machine.memory.poke.w 0x50F31200 0x0080");
    await page.waitForTimeout(500);
    if ((await probe(page, "machine.sound.in_enabled")) === "true") break;
  }
  // Log the whole picture BEFORE asserting: a failed assertion that takes
  // the diagnostics down with it is what made the last three rounds guesswork.
  await page.waitForTimeout(4000);
  console.log(
    "  micStats AFTER pSndInEn:",
    await page.evaluate(() =>
      JSON.stringify(
        (
          globalThis as unknown as { __micStats?: () => unknown }
        ).__micStats?.() ?? "n/a",
      ),
    ),
  );
  console.log(
    "  graph:",
    await page.evaluate(() =>
      JSON.stringify(
        (
          globalThis as unknown as { __micLive?: () => unknown }
        ).__micLive?.() ?? "n/a",
      ),
    ),
  );
  expect(await probe(page, "machine.sound.in_enabled")).toBe("true");
  // The worklet, not the deprecated fallback: addModule() from a blob: URL
  // is rejected under cross-origin isolation and fails SILENTLY into it.
  const graph = JSON.parse(
    (await page.evaluate(() =>
      JSON.stringify(
        (
          globalThis as unknown as { __micLive?: () => unknown }
        ).__micLive?.() ?? "{}",
      ),
    )) as string,
  ) as { node: string; live: boolean };
  expect(graph.node, "fell back off the AudioWorklet path").toBe(
    "AudioWorkletNode",
  );
  expect(graph.live).toBe(true);

  // The seam is pulled: the frame engine takes samples from the host source.
  await expect
    .poll(async () => Number(await probe(page, "machine.audioin.samples")), {
      timeout: 30_000,
    })
    .toBeGreaterThan(0);

  // --- 5. THE ASSERTIONS THAT MATTER.
  //
  // (a) The transport is alive end to end. `produced` is written by the
  //     AudioWorklet on the browser main thread and `consumed` by the
  //     emulator worker through the seam, so both climbing proves
  //     getUserMedia → worklet → shared-heap ring → gs_audio_in_frames →
  //     the Singer's DMA. This is the assertion that would have caught the
  //     original bug (the worklet was never rendered, so `produced` stayed
  //     at 0 forever).
  const stats = async () =>
    JSON.parse(
      (await page.evaluate(() =>
        JSON.stringify(
          (
            globalThis as unknown as { __micStats?: () => unknown }
          ).__micStats?.() ?? "{}",
        ),
      )) as string,
    ) as { produced: number; consumed: number; overruns: number };
  const s1 = await stats();
  expect(s1.produced, "the worklet is not producing samples").toBeGreaterThan(
    10_000,
  );
  expect(s1.consumed, "the guest is not consuming samples").toBeGreaterThan(
    10_000,
  );
  await page.waitForTimeout(2000);
  const s2 = await stats();
  expect(s2.produced, "production stalled").toBeGreaterThan(s1.produced);
  expect(s2.consumed, "consumption stalled").toBeGreaterThan(s1.consumed);

  // (b) Real audio reaches guest RAM. Chromium's fake microphone emits a
  //     PERIODIC BEEP rather than a continuous tone, so a single snapshot of
  //     the double buffer usually lands in its silence — poll instead, and
  //     take the loudest sample seen. The bar is well above the codec's ±1
  //     LSB dither, which is exactly what a dead capture path would leave
  //     here (and what came back as full-scale white noise).
  let peak = 0;
  for (let round = 0; round < 12 && peak <= 16; round++) {
    for (let i = 0; i < 8; i++) {
      const w = Number(
        await probe(page, `machine.memory.peek.w(0x10000 + ${i} * 244)`),
      );
      const sgn = w > 32767 ? w - 65536 : w;
      peak = Math.max(peak, Math.abs(sgn));
    }
  }
  console.log("  peak sample seen in the guest double buffer:", peak);
  expect(
    peak,
    `guest buffer peaks at ${peak} — at or below the codec's dither, so no audio is arriving`,
  ).toBeGreaterThan(16);

  // --- 6. Disconnect: the guest goes back to no microphone.
  await micBtn.click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "false");
  await expect
    .poll(async () => probe(page, "machine.audioin.source"))
    .toBe("none");
  expect(await probe(page, "machine.audioin.connected")).toBe("false");
});
