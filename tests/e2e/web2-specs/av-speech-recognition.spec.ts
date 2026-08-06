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
// 1x, gs_audio_in_frames discards the backlog (`rd = wr - need`) to bound
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

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";
import { buildFakeCaptureWav } from "../helpers/fake-audio";

const DATA = path.resolve(__dirname, "../../data");
const AV_ROM = path.join(DATA, "roms", "q840av-q660av-5bf10fd1.rom");
const AV_HD = path.join(DATA, "systems", "system_7_1_77mb_av.img");
const AV_HD_NAME = "system_7_1_77mb_av.img";
const UTTERANCE = path.join(DATA, "speech", "sr-open-the-trash.wav");

// Built at import time: --use-file-for-fake-audio-capture is a browser
// launch flag, so the file has to exist before test.use() is applied.
const FAKE_WAV = path.join(os.tmpdir(), "gs-fake-capture-open-the-trash-48k.wav");
let fixtureError = "";
try {
  buildFakeCaptureWav(UTTERANCE, FAKE_WAV);
} catch (e) {
  fixtureError = String(e);
}

test.use({
  launchOptions: {
    args: [
      "--use-gl=angle",
      "--use-angle=swiftshader-webgl",
      "--ignore-gpu-blocklist",
      "--disable-dev-shm-usage",
      "--use-fake-device-for-media-stream",
      "--use-fake-ui-for-media-stream",
      // The fake capture device plays this file instead of its beep, and
      // loops it. The fixture pads the tail so the loop point leaves
      // EndPoint the >=400 ms of quiet it needs to close an utterance.
      `--use-file-for-fake-audio-capture=${FAKE_WAV}`,
    ],
  },
});

// --- terminal plumbing (identical to av-sound-record.spec.ts) ---------------

async function focusTerminal(page: Page): Promise<void> {
  const ta = page.locator(".xterm textarea.xterm-helper-textarea");
  if ((await ta.count()) > 0) {
    await ta.focus();
    await page.waitForFunction(
      () =>
        document.activeElement instanceof HTMLTextAreaElement &&
        document.activeElement.classList.contains("xterm-helper-textarea"),
      undefined,
      { timeout: 15_000 },
    );
    return;
  }
  await page.locator(".xterm").click();
}

async function terminalRun(page: Page, line: string): Promise<void> {
  await focusTerminal(page);
  await page.keyboard.type(line, { delay: 10 });
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
async function probe(page: Page, expr: string, timeoutMs = 10_000): Promise<string | null> {
  const key = `s${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${${expr}}=${key}"`);
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await readKey(page, key);
    if (v !== null) return v;
    if (Date.now() > deadline) return null;
    await page.waitForTimeout(250);
  }
}

// suite-av counts settle time in Mac ticks; the browser runs on wall clock,
// so the same counts convert at 60 Hz.
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

// Open the selected Finder item with Command-O, NOT a double-click.
//
// suite-av double-clicks, and in emulated time its run_ticks(4)/(6) really
// are 4 and 6 ticks. Here every shell command is typed into xterm one
// keystroke at a time (typing a burst gets characters dropped — see
// av-microphone.spec.ts), so a double-click spans ~900 ms of wall clock and
// the Finder sees two separate selections. Widening DoubleTime does not
// rescue it either; that was measured, not assumed. Command-O has no timing
// constraint at all, which is the property that makes it right for a GUI
// driven over a terminal.
//
// The letter has to go in as an ADB keycode: debug_mac_resolve_key_name
// knows the modifiers and the named keys but not a-z, and `o` is $1F.
async function openSelected(page: Page): Promise<void> {
  await terminalRun(page, 'machine.adb.keyboard.down "command"');
  await page.waitForTimeout(ticksMs(4));
  await terminalRun(page, "machine.adb.keyboard.press 0x1F");
  await page.waitForTimeout(ticksMs(4));
  await terminalRun(page, 'machine.adb.keyboard.up "command"');
}

async function waitForStableScreen(page: Page, timeoutMs: number): Promise<void> {
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

interface MicStats {
  produced: number;
  consumed: number;
  overruns: number;
  underruns: number;
  rate: number;
}

async function micStats(page: Page): Promise<MicStats> {
  return JSON.parse(
    (await page.evaluate(() =>
      JSON.stringify((globalThis as unknown as { __micStats?: () => unknown }).__micStats?.() ?? "{}"),
    )) as string,
  ) as MicStats;
}

// The Trash window lands mid-screen (suite-av's goldens/av-sr-command.png).
// This rectangle sits deep inside it and clear of the recognizer's windoid
// in the bottom-left corner — which changes on ANY utterance, matched or
// not ("Pardon me?"), so it cannot be the signal.
const TRASH_RECT = "365, 260, 445, 550";

test("PlainTalk recognises speech from the browser microphone", async ({ page }) => {
  test.setTimeout(1_200_000);
  test.skip(fixtureError !== "", `fake-capture fixture unavailable: ${fixtureError}`);
  test.skip(!fs.existsSync(AV_HD), "the AV System 7.1 disk image is not staged");

  page.on("console", (m) => console.log(`  [page:${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.log(`  [pageerror] ${e.message}`));
  console.log(`  fake capture file: ${FAKE_WAV}`);

  await gotoWeb2(page);

  // --- 1. Start the 840AV with its System 7.1 disk. ------------------------
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await romChooser.setFiles(AV_ROM);

  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="q840av"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption("q840av");

  const hd = page.locator("#cfg-hd");
  const [hdChooser] = await Promise.all([page.waitForEvent("filechooser"), hd.selectOption("Upload image...")]);
  await hdChooser.setFiles(AV_HD);
  await expect(hd.locator("option", { hasText: AV_HD_NAME })).toHaveCount(1, { timeout: 120_000 });
  await hd.selectOption(AV_HD_NAME);

  await page.getByRole("button", { name: "Start Machine" }).click();
  await expect(page.locator(".toast .msg").filter({ hasText: "Machine started" })).toBeVisible({
    timeout: 60_000,
  });

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".xterm")).toBeVisible({ timeout: 15_000 });
  await expect.poll(async () => probe(page, "machine.id"), { timeout: 30_000 }).toBe("q840av");

  // --- 2. Boot to the Finder, accelerated; then back to real time. ---------
  // The microphone is a real-time stream: a guest running faster than wall
  // clock drains the ring faster than the browser fills it, a guest running
  // slower overruns it. Only paced mode is meaningful for what follows.
  await page.getByRole("button", { name: "accelerated", exact: true }).click();
  await waitForStableScreen(page, 420_000);
  await page.getByRole("button", { name: "real-time", exact: true }).click();
  await expect.poll(async () => probe(page, "scheduler.mode"), { timeout: 30_000 }).toBe("paced");

  // 100% zoom so the whole 640x480 guest screen is in the viewport: the
  // failure screenshot Playwright captures is the only view of a GUI drive
  // that went wrong, and at 200% it shows a panned fragment.
  const zoom = page.locator('input[aria-label="Zoom level"]');
  await zoom.fill("100%");
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
  const micBtn = page.locator('.gs-toolbar button[aria-label*="icrophone"]');
  await expect(micBtn).toHaveCount(1);
  await micBtn.click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "true");
  await expect.poll(async () => probe(page, "machine.audioin.source")).toBe("host");
  await expect
    .poll(async () => probe(page, "machine.audioin.connected"), { timeout: 30_000 })
    .toBe("true");

  const baseline = await probe(page, `machine.screen.checksum(${TRASH_RECT})`);
  console.log(`  Trash-window rectangle before listening: ${baseline}`);

  // --- 5. Listen. The file loops, so the utterance repeats every ~4.3 s;
  // rung 7's own reliability is about one take in two, which is why the row
  // in suite-av says it twice. Poll for the Trash window while recording
  // what the transport is doing underneath.
  let peak = 0;
  let opened = false;
  let last: MicStats | null = null;
  const deadline = Date.now() + 180_000;
  while (Date.now() < deadline && !opened) {
    const p = Number(await probe(page, "machine.audioin.peak"));
    if (Number.isFinite(p)) peak = Math.max(peak, p);
    last = await micStats(page);
    const now = await probe(page, `machine.screen.checksum(${TRASH_RECT})`);
    if (now !== null && now !== baseline) opened = true;
    console.log(
      `  t+${Math.round((180_000 - (deadline - Date.now())) / 1000)}s  peak ${peak}  ` +
        `produced ${last.produced} consumed ${last.consumed} ` +
        `overruns ${last.overruns} underruns ${last.underruns}  rect ${now}`,
    );
    if (!opened) await page.waitForTimeout(2000);
  }

  // --- 6. The diagnostics, printed before anything is asserted. ------------
  console.log(`  RESULT: Trash window opened: ${opened}`);
  // Reference: suite-av's av-sr-command feeds this same asset straight at
  // machine.audioin and recognises it. Its peak there is the file's own
  // 5318 counts times the codec's 2.37x A/D gain — about 12,600. A browser
  // figure far above that is the level error that makes Casper's AGC wind
  // the codec gain down and reject the utterance (sr-test-audio-assets §2).
  console.log(`  codec saw a peak of ${peak} counts (headless av-sr-command sees ~12600 for this asset)`);
  console.log(`  final micStats: ${JSON.stringify(last)}`);
  console.log(`  audioin.level=${await probe(page, "machine.audioin.level")}`);
  console.log(`  scheduler.mode=${await probe(page, "scheduler.mode")}`);

  // Audio arrived at all. Below this bar the codec saw its own dither, and
  // nothing downstream can be judged.
  expect(peak, `the codec saw a peak of ${peak} counts — the microphone delivered silence`).toBeGreaterThan(64);

  expect(await probe(page, "machine.dsp.emr"), "the DSP kernel died while listening").toBe("0x8000");

  // The outcome is the gate; the transport counters are the explanation.
  //
  // `overruns` is deliberately NOT a separate assertion. It is the number of
  // times the browser got half a ring ahead of the guest and
  // gs_audio_in_frames discarded the backlog to bound latency — correct for
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
