// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: PlainTalk speech recognition from the BROWSER MICROPHONE.
//
// suite-av's av-sr-command row already proves the recognizer works end to
// end — spoken "Computer, open the Trash" opens the Trash window — but it
// injects the utterance at machine.audioin, BELOW the seam. Everything
// unique to a live microphone is therefore untested by it:
//
//   getUserMedia → AudioContext at the browser's NATIVE rate (44.1 kHz on
//   the CI runner) → AudioWorklet → shared-heap ring → em_audio_in.c
//   (100 Hz high-pass, sensitivity normaliser, polyphase resample to the
//   codec's 24 kHz) → Singer DMA → the DSP → Casper
//
// …and the CLOCK RELATIONSHIP, which nothing else exercises. The browser
// produces on wall time and the guest consumes on emulated time, so when
// the emulated 840AV — a 68040 and a DSP3210, in WASM — does not sustain
// 1x, platform_audio_in_frames discards the backlog (`rd = wr - need`) to bound
// latency. That punches holes in the utterance while the level meter keeps
// waving, because amplitude survives what sequence does not.
//
// WHAT THIS MEASURED, first green run: recognition succeeded while
// carrying 19 overruns and 30 underruns. So the dropping is real and it is
// NOT on its own fatal — the fixture loops every ~4.3 s, and a damaged take
// is simply followed by an intact one. Do not read a non-zero overrun count
// here as the cause of a failure; read it as the first thing to check.
// That is why the counters are reported and not asserted.
//
// This spec exists because a live session failed to recognise anything and
// could not be reproduced any other way. --use-file-for-fake-audio-capture
// replaces Chromium's 440 Hz beep with a real utterance, which is what lets
// the whole live path run in CI with deterministic content.
//
// It is deliberately loud in its diagnostics: overruns/underruns, produced
// vs consumed, and the peak the codec actually saw are all printed BEFORE
// anything is asserted. A recognition failure with those numbers next to it
// is a diagnosis; without them it is another round of guessing.

import { test, expect } from "../helpers/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";
import { buildFakeCaptureWav } from "../helpers/fake-audio";
import {
  AV_HD,
  DATA,
  FAKE_MEDIA_ARGS,
  MIC_BUTTON,
  type MicStats,
  bootAVToFinder,
  click,
  micStats,
  moveTo,
  openSelected,
  probe,
  probeNumber,
  ticksMs,
} from "../helpers/av";

const UTTERANCE = path.join(DATA, "speech", "sr-open-the-trash.wav");

// Built at import time: --use-file-for-fake-audio-capture is a browser
// launch flag, so the file has to exist before test.use() is applied.
//
// Kept inside the workspace rather than os.tmpdir(): on the CI runner the
// browser read nothing from a /tmp path and Chromium's FileSource fell back
// to SILENCE (not to its beep), which is indistinguishable from a dead
// capture path from the inside — peak 59 counts, the codec's own dither.
const FAKE_WAV = path.resolve(__dirname, "../test-results/gs-fake-capture-open-the-trash-48k.wav");
let fixtureError = "";
try {
  fs.mkdirSync(path.dirname(FAKE_WAV), { recursive: true });
  buildFakeCaptureWav(UTTERANCE, FAKE_WAV);
} catch (e) {
  fixtureError = String(e);
}

test.use({
  // Real-time media feeds the guest: keep web2's default pacing.
  gsSpeed: null,
  launchOptions: {
    args: [
      ...FAKE_MEDIA_ARGS,
      // The fake capture device plays this file instead of its beep, and
      // loops it. The fixture pads the tail so the loop point leaves
      // EndPoint the >=400 ms of quiet it needs to close an utterance.
      `--use-file-for-fake-audio-capture=${FAKE_WAV}`,
    ],
  },
});

// The Trash window lands mid-screen (suite-av's goldens/av-sr-command.png).
// This rectangle sits deep inside it and clear of the recognizer's windoid
// in the bottom-left corner — which changes on ANY utterance, matched or
// not ("Pardon me?"), so it cannot be the signal.
const TRASH_RECT = [365, 260, 445, 550];

// The codec's own dither is ±1 LSB, which the A/D gain lifts to a handful of
// counts. Anything at or under this is silence however loud it sounds after
// the guest's recorder has gained it up.
//
// Set well above the dither, not just clear of it. At 64 this gate passed a
// build whose conditioning had collapsed the level to 90 counts — audible as
// nothing — and the spec went on to report a recognition. A healthy run
// delivers ~13,700 here (headless av-sr-command sees ~12,600 for the same
// asset), so 2000 is ~16 dB of margin below a good run and ~30 dB above the
// floor: wide enough not to be brittle, tight enough that "the fixture is
// not arriving" cannot masquerade as a pass.
const AUDIO_FLOOR = 2000;

test("PlainTalk recognises speech from the browser microphone", async ({ page }) => {
  test.setTimeout(1_200_000);
  test.skip(fixtureError !== "", `fake-capture fixture unavailable: ${fixtureError}`);
  test.skip(!fs.existsSync(AV_HD), "the AV System 7.1 disk image is not staged");

  page.on("console", (m) => console.log(`  [page:${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.log(`  [pageerror] ${e.message}`));
  console.log(`  fake capture file: ${FAKE_WAV}`);

  await gotoWeb2(page);

  // --- 1. Boot the 840AV with its System 7.1 disk to the Finder. ----------
  await bootAVToFinder(page);

  // 50% zoom so the WHOLE 640x480 guest screen fits above the panel. The
  // failure screenshot is the only view of a GUI drive that went wrong, and
  // at 100% the viewport crops it at about guest row 390 — which is where
  // the window this test watches for actually lives, so the artifact could
  // not answer the one question it was collected for.
  const zoom = page.locator('input[aria-label="Zoom level"]');
  await zoom.fill("50%");
  await zoom.press("Enter");

  const shot = async (name: string) => {
    await page.screenshot({ path: test.info().outputPath(`${name}.png`) });
  };
  await shot("01-finder");

  // --- 3. Turn recognition on (suite-av's sr_bring_up, through the GUI). ---
  await moveTo(page, 50, 50);
  await page.waitForTimeout(ticksMs(30));
  await click(page, 536, 306); // select the Speech Setup desktop alias
  await page.waitForTimeout(ticksMs(30));
  await openSelected(page); // …and open it (see openSelected)
  await page.waitForTimeout(ticksMs(600));
  await shot("02-speech-setup");
  await click(page, 176, 107); // Recognition: On
  await page.waitForTimeout(ticksMs(1800)); // the recognizer's startup blackout
  await shot("03-recognition-on");
  await click(page, 114, 51); // close the Speech Setup panel
  await page.waitForTimeout(ticksMs(200));
  await moveTo(page, 320, 400); // park the pointer clear of everything
  await page.waitForTimeout(ticksMs(600));
  await shot("04-listening");

  expect(await probe(page, "machine.dsp.state"), "the DSP is not running after the recognizer started").toBe(
    "running",
  );
  expect(await probe(page, "machine.dsp.emr"), "the DSP kernel died bringing the recognizer up").toBe("0x8000");
  expect(
    await probe(page, "machine.sound.in_enabled"),
    "input DMA is not running — the recognizer is not listening",
  ).toBe("true");

  // --- 4. Connect the browser microphone. ----------------------------------
  // Only now: the track attaches while the guest is genuinely listening, and
  // connecting earlier would feed the looping utterance into the startup
  // blackout, where it would be heard but not yet matchable.
  const micBtn = page.locator(MIC_BUTTON);
  await expect(micBtn).toHaveCount(1);
  await micBtn.click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "true");
  await expect.poll(async () => probe(page, "machine.audioin.source")).toBe("host");
  await expect
    .poll(async () => probe(page, "machine.audioin.connected"), { timeout: 30_000 })
    .toBe("true");

  // --- 5a. Wait for AUDIO before judging anything. -------------------------
  //
  // A "the window opened" reading taken while the codec is hearing its own
  // dither is worthless, and the CI runner produced exactly that: RESULT
  // true at t+2s with a peak of 59 counts, because the baseline had been
  // taken before the bring-up finished settling and the first poll then
  // saw the difference. Audio first, THEN baseline, then watch.
  //
  // If the fake capture device never delivers, this spec has no fixture and
  // cannot judge the product at all — that is a skip, not a failure. The
  // browser-mic transport itself is covered by av-microphone.spec.ts with
  // the default fake device, so nothing goes unwatched.
  const rectBeforeAudio = await probe(page, "machine.screen.checksum", TRASH_RECT);
  let peak = 0;
  const audioDeadline = Date.now() + 45_000;
  while (Date.now() < audioDeadline && peak <= AUDIO_FLOOR) {
    const p = await probeNumber(page, "machine.audioin.peak");
    if (Number.isFinite(p)) peak = Math.max(peak, p);
    await page.waitForTimeout(1000);
  }
  console.log(`  codec peak after the audio wait: ${peak} counts`);
  test.skip(
    peak <= AUDIO_FLOOR,
    `the fake capture device delivered silence (codec peak ${peak} counts, at the dither floor) — ` +
      "--use-file-for-fake-audio-capture is not working in this environment, so there is no " +
      "utterance to recognise",
  );

  const baseline = await probe(page, "machine.screen.checksum", TRASH_RECT);
  console.log(`  Trash-window rectangle: ${rectBeforeAudio} before audio, ${baseline} at baseline`);
  if (baseline !== rectBeforeAudio) {
    // Not fatal — recognition genuinely can fire during the audio wait, and
    // then the baseline already contains the window we are watching for and
    // no further change will come. Say so, so a "never opened" result below
    // is read as this race rather than as a dead recognizer.
    console.log("  NOTE: the rectangle changed while waiting for audio — a 'never opened' result may be that race");
  }

  // --- 5. Listen. The file loops, so the utterance repeats every ~4.3 s;
  // The recognizer needs a WARM-UP utterance: Casper's AGC settles on the
  // first and recognises the second. That is deterministic, not odds -- the
  // headless row av-sr-command fails every time with one sr_say and passes
  // every time (bit-exact, instr=1506469288) with two, which is why it says
  // it twice. An earlier comment here called it "about one take in two",
  // which sent a flake investigation after a coin flip that does not exist.
  // Here the fixture loops every ~4.5 s, so the warm-up take arrives on its
  // own. Poll for the Trash window while recording what the transport is
  // doing underneath.
  let opened = false;
  let last: MicStats | null = null;
  const deadline = Date.now() + 180_000;
  while (Date.now() < deadline && !opened) {
    const p = await probeNumber(page, "machine.audioin.peak");
    if (Number.isFinite(p)) peak = Math.max(peak, p);
    last = await micStats(page);
    const now = await probe(page, "machine.screen.checksum", TRASH_RECT);
    if (now !== baseline) opened = true;
    console.log(
      `  t+${Math.round((180_000 - (deadline - Date.now())) / 1000)}s  peak ${peak}  ` +
        `produced ${last.produced} consumed ${last.consumed} ` +
        `overruns ${last.overruns} underruns ${last.underruns}  rect ${now}`,
    );
    if (!opened) await page.waitForTimeout(2000);
  }

  // The transport gate below wants an utterance's worth of frames (~4.5 s)
  // over the listening window, and the recognizer can act sooner than that:
  // the warm-up take arrived during the audio wait, and the poll above
  // stops the moment the window opens.  Let the counter run out the rest of
  // that span before reading it, so a fast recognition is not misread as a
  // capture-path failure.
  const minFrames = Math.round((last?.rate || 44100) * 4);
  const transportDeadline = Date.now() + 15_000;
  while ((last?.captured ?? 0) < minFrames && Date.now() < transportDeadline) {
    await page.waitForTimeout(500);
    last = await micStats(page);
  }

  // --- 6. The diagnostics, printed before anything is asserted. ------------
  console.log(`  RESULT: Trash window opened: ${opened}`);
  // Reference: suite-av's av-sr-command feeds this same asset straight at
  // machine.audioin and recognises it. Its peak there is the file's own
  // 5318 counts times the codec's 2.37x A/D gain — about 12,600. A browser
  // figure far above that is the level error that makes Casper's AGC wind
  // the codec gain down and reject the utterance.
  console.log(`  codec saw a peak of ${peak} counts (headless av-sr-command sees ~12600 for this asset)`);
  console.log(`  final micStats: ${JSON.stringify(last)}`);
  console.log(`  audioin.level=${await probe(page, "machine.audioin.level")}`);
  console.log(`  scheduler.mode=${await probe(page, "scheduler.mode")}`);

  expect(await probe(page, "machine.dsp.emr"), "the DSP kernel died while listening").toBe("0x8000");

  // Transport before outcome. Until this existed, a browser that delivered no
  // audio failed as "the recognizer did not act on the utterance", which sent
  // two separate investigations at the recognizer and the emulator's speed
  // before anyone read the frame counter. One utterance is ~4.5 s; require at
  // least one utterance's worth over the whole listening window before the
  // recognition result is allowed to mean anything.
  expect(
    last?.captured ?? 0,
    `the browser never delivered audio: ${last?.captured ?? 0} frames captured in ${
      (180_000 / 1000) | 0
    }s at ${last?.rate}Hz (want >= ${minFrames}). This is a capture-path failure, NOT a ` +
      `recognition failure — check the AudioContext state and the MediaStream track, not the DSP. ` +
      `transport ${JSON.stringify(last)}`,
  ).toBeGreaterThanOrEqual(minFrames);

  // The outcome is the gate; the transport counters are the explanation.
  //
  // `overruns` is deliberately NOT a separate assertion. It is the number of
  // times the browser got half a ring ahead of the guest and
  // platform_audio_in_frames discarded the backlog to bound latency — correct for
  // a monitor path, destructive for recognition, which needs the whole
  // utterance in order. Whether it is fatal depends on how much it ate, so
  // the honest test is: did the recognizer act? If it did not, this number
  // is the first thing to read, which is why it is in the message.
  expect(
    opened,
    `the Trash window never opened — the recognizer did not act on the utterance. ` +
      `codec peak ${peak}; transport ${JSON.stringify(last)}. ` +
      "A non-zero `overruns` here means the emulated machine is not keeping up with wall " +
      "clock and the guest heard the utterance with holes punched through it.",
  ).toBe(true);
});
