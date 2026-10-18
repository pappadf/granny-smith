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

import { test, expect } from "../helpers/test";
import { gsCallInPage } from "../helpers/web2-eval";
import { gotoWeb2 } from "../helpers/web2-fs";
import {
  FAKE_MEDIA_ARGS,
  MIC_BUTTON,
  bootAVToFinder,
  click,
  doubleClick,
  micStats,
  mouseButton,
  moveTo,
  probe,
  probeNumber,
  ticksMs,
} from "../helpers/av";

test.use({
  // Real-time media feeds the guest: keep web2's default pacing.
  gsSpeed: null,
  launchOptions: { args: FAKE_MEDIA_ARGS },
});

test("record from the browser microphone in the Sound control panel", async ({
  page,
}) => {
  test.setTimeout(600_000);
  page.on("console", (m) => console.log(`  [page:${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.log(`  [pageerror] ${e.message}`));

  await gotoWeb2(page);

  // --- 1. Boot the 840AV with its System 7.1 disk to the Finder. ----------
  await bootAVToFinder(page);

  // --- 3. Connect the browser microphone. ----------------------------------
  const micBtn = page.locator(MIC_BUTTON);
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
  await mouseButton(page, true);
  await page.waitForTimeout(ticksMs(40));
  await moveTo(page, 70, 150); // Control Panels
  await page.waitForTimeout(ticksMs(30));
  await mouseButton(page, false);
  await page.waitForTimeout(ticksMs(400));
  await doubleClick(page, 104, 131); // the Sound cdev icon
  await page.waitForTimeout(ticksMs(600));

  // Volume up — the disk ships at 0, so the listen-back would be silent for a
  // reason that has nothing to do with the recording.
  await moveTo(page, 173, 205);
  await page.waitForTimeout(ticksMs(20));
  await mouseButton(page, true);
  await page.waitForTimeout(ticksMs(15));
  await moveTo(page, 173, 165);
  await page.waitForTimeout(ticksMs(15));
  await moveTo(page, 173, 128);
  await page.waitForTimeout(ticksMs(15));
  await mouseButton(page, false);
  await page.waitForTimeout(ticksMs(300));

  // --- 5. "Add…" opens the standard SndRecord dialog. ----------------------
  await click(page, 261, 240);
  await page.waitForTimeout(ticksMs(600));

  const samplesBefore = await probeNumber(page, "machine.audioin.samples");
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
    await probeNumber(page, "machine.audioin.samples"),
    "the frame engine pulled nothing from the host source",
  ).toBeGreaterThan(samplesBefore);

  // …and the samples it pulled are AUDIO. Poll: Chromium's fake device beeps
  // periodically, so a single reading can legitimately land in its silence.
  // The bar sits well above the codec's ±1 LSB dither, which is exactly what a
  // dead browser-mic path leaves here — and what the guest's record AGC then
  // amplifies into the "aggressive static" this test exists to catch.
  let peak = 0;
  for (let i = 0; i < 20 && peak <= 64; i++) {
    peak = Math.max(peak, await probeNumber(page, "machine.audioin.peak"));
    await page.waitForTimeout(500);
  }
  console.log(`  machine.audioin.peak while recording: ${peak}`);
  console.log(`  micStats: ${JSON.stringify(await micStats(page))}`);
  expect(
    peak,
    `browser audio peaks at ${peak} counts — at or below the codec's dither, so the microphone delivered silence`,
  ).toBeGreaterThan(64);

  await click(page, 174, 152); // Stop
  await page.waitForTimeout(ticksMs(200));

  // --- 6. Listen back. -----------------------------------------------------
  // Play sends the recorded sound out through the Sound Manager and the DSP to
  // the sdev; capturing the producer side is the host's ear on it.
  await gsCallInPage(page, "machine.sound.capture.start");
  await click(page, 248, 152); // Play
  await page.waitForTimeout(ticksMs(400));

  let outPeak = 0;
  for (let i = 0; i < 12 && outPeak <= 256; i++) {
    outPeak = Math.max(outPeak, await probeNumber(page, "machine.sound.capture.peak"));
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
