// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: record from the BROWSER MICROPHONE through the guest's own Sound
// control panel, then listen back — the exact sequence a user performs:
//
//   Boot to Finder → Apple menu → Control Panels → Sound → under "Alert
//   Sound" pick "Add…" → Record → Stop → Play.
//
// suite-av's av-sound-in row already walks this GUI with the emulator's
// deterministic tone source, headless, and pins the result against a golden.
// It passed the entire time the browser microphone was delivering silence,
// because the tone source is injected below the seam this test exercises:
// getUserMedia → AudioWorklet → shared-heap ring → em_audio_in.c's PlainTalk
// conditioning → the Singer's input DMA. Only a real browser reaches that.
//
// av-microphone.spec.ts covers the same transport but pokes the Singer's
// registers directly, with no OS. This one is deliberately the slow, whole-
// stack version: a real System 7 boot and real clicks in the real cdev, so
// nothing between the user's voice and the recorded sound is stubbed.
//
// The assertion that matters is machine.audioin.peak WHILE RECORDING. The
// guest's recorder gains up hard, so a dead capture path and a live one are
// indistinguishable by ear — both come back as loud noise (the codec's own
// ±1 LSB dither, amplified). Measuring at the source is the only way to tell
// them apart, and that is what regressed three times.
//
// Runs against Chromium's fake capture device (a periodic beep), so no
// hardware is involved.

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";

const DATA = path.resolve(__dirname, "../../data");
const AV_ROM = path.join(DATA, "roms", "q840av-q660av-5bf10fd1.rom");
const AV_HD = path.join(DATA, "systems", "system_7_1_77mb_av.img");
const AV_HD_NAME = "system_7_1_77mb_av.img";

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

// --- terminal plumbing (see av-camera.spec.ts for why answers are bracketed) --

async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator(".xterm");
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press("Enter");
}

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
  const key = `p${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${${expr}}=${key}"`);
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await readKey(page, key);
    if (v !== null) return v;
    if (Date.now() > deadline) return null;
    await page.waitForTimeout(250);
  }
}

// --- guest GUI driving -------------------------------------------------------
//
// suite-av measures its settle times in Mac ticks via run_ticks(); in the
// browser the machine runs on wall-clock, so the same counts convert directly
// at 60 Hz. The floor keeps the very short waits from being swallowed by
// scheduling jitter.
function ticksMs(n: number): number {
  return Math.max(120, Math.round((n / 60) * 1000));
}

async function moveTo(page: Page, x: number, y: number): Promise<void> {
  await terminalRun(page, `machine.adb.mouse.move ${x} ${y} "global"`);
}

async function click(page: Page, x: number, y: number): Promise<void> {
  await moveTo(page, x, y);
  await page.waitForTimeout(ticksMs(20));
  await terminalRun(page, "machine.adb.mouse.click true");
  await page.waitForTimeout(ticksMs(4));
  await terminalRun(page, "machine.adb.mouse.click false");
  await page.waitForTimeout(ticksMs(6));
}

async function doubleClick(page: Page, x: number, y: number): Promise<void> {
  await moveTo(page, x, y);
  await page.waitForTimeout(ticksMs(20));
  for (let i = 0; i < 2; i++) {
    await terminalRun(page, "machine.adb.mouse.click true");
    await page.waitForTimeout(ticksMs(4));
    await terminalRun(page, "machine.adb.mouse.click false");
    await page.waitForTimeout(ticksMs(6));
  }
}

// The Finder is up once the screen stops changing. Polling the checksum is
// what suite-av's wait_stable() does; here the scheduler is free-running, so
// the poll is on wall-clock instead of budgeted instructions.
async function waitForStableScreen(page: Page, timeoutMs: number) {
  const deadline = Date.now() + timeoutMs;
  let previous = "";
  let stable = 0;
  while (Date.now() < deadline) {
    const now = await probe(page, "machine.screen.checksum()", 15_000);
    stable = now !== null && now === previous ? stable + 1 : 0;
    previous = now ?? "";
    if (stable >= 3) return;
    await page.waitForTimeout(2000);
  }
  throw new Error("the guest screen never settled — no Finder");
}

test("record from the browser microphone in the Sound control panel", async ({
  page,
}) => {
  test.setTimeout(600_000);
  page.on("console", (m) => console.log(`  [page:${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.log(`  [pageerror] ${e.message}`));

  await gotoWeb2(page);

  // --- 1. Configure and start the 840AV with its System 7.1 disk. -----------
  // The ROM goes in through the Welcome button, not straight into OPFS:
  // the upload pipeline is what identifies the ROM and offers 'q840av' in the
  // model list. Dropping the file in behind it leaves the list empty.
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await romChooser.setFiles(AV_ROM);

  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="q840av"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await model.selectOption("q840av");

  const hd = page.locator("#cfg-hd");
  const [hdChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    hd.selectOption("Upload image..."),
  ]);
  await hdChooser.setFiles(AV_HD);
  await expect(hd.locator("option", { hasText: AV_HD_NAME })).toHaveCount(1, {
    timeout: 120_000,
  });
  await hd.selectOption(AV_HD_NAME);

  await page.getByRole("button", { name: "Start Machine" }).click();
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Machine started" }),
  ).toBeVisible({ timeout: 60_000 });

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".xterm")).toBeVisible({ timeout: 15_000 });
  await expect
    .poll(async () => probe(page, "machine.id"), { timeout: 30_000 })
    .toBe("q840av");

  // --- 2. Boot to the Finder, accelerated. ---------------------------------
  // Accelerated only for the boot: the microphone is a REAL-TIME stream, and a
  // guest running faster than wall-clock drains the ring quicker than the
  // browser fills it. Back to paced before anything touches audio.
  await page.getByRole("button", { name: "accelerated", exact: true }).click();
  await waitForStableScreen(page, 420_000);
  await page.getByRole("button", { name: "real-time", exact: true }).click();
  await expect
    .poll(async () => probe(page, "scheduler.mode"), { timeout: 30_000 })
    .toBe("paced");

  // --- 3. Connect the browser microphone. ----------------------------------
  const micBtn = page.locator('.gs-toolbar button[aria-label*="icrophone"]');
  await expect(micBtn).toHaveCount(1);
  await micBtn.click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "true");
  await expect
    .poll(async () => probe(page, "machine.audioin.source"))
    .toBe("host");

  // --- 4. Apple menu → Control Panels, then open the Sound cdev. ------------
  await moveTo(page, 50, 50);
  await page.waitForTimeout(ticksMs(30));
  await moveTo(page, 14, 8);
  await page.waitForTimeout(ticksMs(20));
  await terminalRun(page, "machine.adb.mouse.click true");
  await page.waitForTimeout(ticksMs(40));
  await moveTo(page, 70, 150); // Control Panels
  await page.waitForTimeout(ticksMs(30));
  await terminalRun(page, "machine.adb.mouse.click false");
  await page.waitForTimeout(ticksMs(400));
  await doubleClick(page, 104, 131); // the Sound cdev icon
  await page.waitForTimeout(ticksMs(600));

  // Volume up — the disk ships at 0, so the listen-back would be silent for a
  // reason that has nothing to do with the recording.
  await moveTo(page, 173, 205);
  await page.waitForTimeout(ticksMs(20));
  await terminalRun(page, "machine.adb.mouse.click true");
  await page.waitForTimeout(ticksMs(15));
  await moveTo(page, 173, 165);
  await page.waitForTimeout(ticksMs(15));
  await moveTo(page, 173, 128);
  await page.waitForTimeout(ticksMs(15));
  await terminalRun(page, "machine.adb.mouse.click false");
  await page.waitForTimeout(ticksMs(300));

  // --- 5. "Add…" opens the standard SndRecord dialog. ----------------------
  await click(page, 261, 240);
  await page.waitForTimeout(ticksMs(600));

  const samplesBefore = Number(await probe(page, "machine.audioin.samples"));
  await click(page, 138, 152); // Record
  await page.waitForTimeout(ticksMs(400));

  // The guest really is recording…
  expect(
    await probe(page, "machine.sound.in_enabled"),
    "input DMA is not running while the recorder is armed",
  ).toBe("true");
  expect(
    await probe(page, "machine.dsp.emr"),
    "the DSP kernel died during recording",
  ).toBe("0x8000");
  expect(
    Number(await probe(page, "machine.audioin.samples")),
    "the frame engine pulled nothing from the host source",
  ).toBeGreaterThan(samplesBefore);

  // …and the samples it pulled are AUDIO. Poll: Chromium's fake device beeps
  // periodically, so a single reading can legitimately land in its silence.
  // The bar sits well above the codec's ±1 LSB dither, which is exactly what a
  // dead browser-mic path leaves here — and what the guest's record AGC then
  // amplifies into the "aggressive static" this test exists to catch.
  let peak = 0;
  for (let i = 0; i < 20 && peak <= 64; i++) {
    peak = Math.max(peak, Number(await probe(page, "machine.audioin.peak")));
    await page.waitForTimeout(500);
  }
  console.log(`  machine.audioin.peak while recording: ${peak}`);
  const stats = await page.evaluate(() =>
    JSON.stringify(
      (
        globalThis as unknown as { __micStats?: () => unknown }
      ).__micStats?.() ?? "n/a",
    ),
  );
  console.log(`  micStats: ${stats}`);
  expect(
    peak,
    `browser audio peaks at ${peak} counts — at or below the codec's dither, so the microphone delivered silence`,
  ).toBeGreaterThan(64);

  await click(page, 174, 152); // Stop
  await page.waitForTimeout(ticksMs(200));

  // --- 6. Listen back. -----------------------------------------------------
  // Play sends the recorded sound out through the Sound Manager and the DSP to
  // the sdev; capturing the producer side is the host's ear on it.
  await terminalRun(page, "machine.sound.capture.start");
  await click(page, 248, 152); // Play
  await page.waitForTimeout(ticksMs(400));

  let outPeak = 0;
  for (let i = 0; i < 12 && outPeak <= 256; i++) {
    outPeak = Math.max(
      outPeak,
      Number(await probe(page, "machine.sound.capture.peak")),
    );
    await page.waitForTimeout(500);
  }
  console.log(`  playback capture peak: ${outPeak}`);
  expect(
    await probe(page, "machine.dsp.emr"),
    "the DSP kernel died during playback",
  ).toBe("0x8000");
  expect(
    outPeak,
    `the playback captured ${outPeak} counts — the recording came back silent`,
  ).toBeGreaterThan(256);

  // --- 7. Cancel: never add the sound to the System file. ------------------
  await click(page, 377, 169);
  await page.waitForTimeout(ticksMs(100));
});
