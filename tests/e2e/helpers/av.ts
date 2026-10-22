// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// What the AV (Quadra 840AV) browser specs share: the fixtures, the fake
// media device launch flags, booting the machine, reading the core and
// driving the guest's GUI.  One copy of what used to be typed into each of
// av-camera, av-microphone, av-sound-record and av-speech-recognition.
//
// Every read and every guest command goes through gsEvalInPage, not the
// terminal: typing into the console on this machine (a 68040 and a DSP in
// WASM, plus a live AudioWorklet on the main thread) lost characters and
// raced heavy output, and a probe cost seconds.  The terminal is for specs
// that test the terminal.

import { expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gsCallInPage, gsEvalInPage } from "./web2-eval";

// --- fixtures ---------------------------------------------------------------

export const DATA = path.resolve(__dirname, "../../data");
export const AV_ROM = path.join(DATA, "roms", "q840av-q660av-5bf10fd1.rom");
export const PLUS_ROM = path.join(DATA, "roms", "plus-v3-4d1f8172.rom");
export const AV_HD = path.join(DATA, "systems", "system_7_1_77mb_av.img");
// A disk this large is stored compressed, as UDIF, under a .dmg name.
export const AV_HD_NAME = "system_7_1_77mb_av.dmg";

// Chromium's fake camera and microphone (--use-fake-device-for-media-stream
// synthesises a moving pattern and a periodic beep; --use-fake-ui-for-media-
// stream auto-grants the permission prompt), so no hardware is involved and
// the specs are CI-safe.  Per-spec launchOptions REPLACE the config array, so
// the swiftshader flags (web2 will not mount without WebGL2) are re-listed.
export const FAKE_MEDIA_ARGS = [
  "--use-gl=angle",
  "--use-angle=swiftshader-webgl",
  "--ignore-gpu-blocklist",
  "--disable-dev-shm-usage",
  "--use-fake-device-for-media-stream",
  "--use-fake-ui-for-media-stream",
];

export const MIC_BUTTON = '.gs-toolbar button[aria-label*="icrophone"]';
export const CAMERA_BUTTON = '.gs-toolbar button[aria-label*="amera"]';

// --- reading the core -------------------------------------------------------

// One object-model value as the text the shell would print for it: "true",
// "none", "0x8000", "12345", an enum's label.  The specs compare against
// those literals.
export async function probe(page: Page, path: string, args?: unknown[]): Promise<string> {
  const v = await gsEvalInPage(page, path, args);
  if (v !== null && typeof v === "object") {
    const o = v as { error?: unknown; enum?: unknown };
    if (o.error !== undefined) throw new Error(`${path}: ${String(o.error)}`);
    if (typeof o.enum === "string") return o.enum;
    return JSON.stringify(v);
  }
  return String(v);
}

export async function probeNumber(page: Page, path: string, args?: unknown[]): Promise<number> {
  return Number(await probe(page, path, args));
}

// --- booting -----------------------------------------------------------------

// Load a ROM through the Welcome dialog (the upload pipeline is what
// identifies it and offers its models; dropping it into OPFS behind the
// dialog leaves the list empty), configure the model and start it.  With a
// `disk`, that image goes in SCSI bay 0 first.
export async function startMachine(
  page: Page,
  romFile: string,
  model: string,
  disk?: { file: string; name: string },
): Promise<void> {
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Load ROM..." }).click(),
  ]);
  await romChooser.setFiles(romFile);

  await page.getByRole("button", { name: "New Machine..." }).click();
  const sel = page.locator("#cfg-model");
  await expect(sel.locator(`option[value="${model}"]`)).toHaveCount(1, { timeout: 30_000 });
  await sel.selectOption(model);

  if (disk) {
    const hd = page.locator("#cfg-media-scsi-0");
    const [hdChooser] = await Promise.all([page.waitForEvent("filechooser"), hd.selectOption("Load image...")]);
    await hdChooser.setFiles(disk.file);
    await expect(hd.locator("option", { hasText: disk.name })).toHaveCount(1, { timeout: 120_000 });
    await hd.selectOption(disk.name);
  }

  await page.getByRole("button", { name: "Start", exact: true }).click();
  await expect(page.locator(".toast .msg").filter({ hasText: "Machine started" })).toBeVisible({
    timeout: 60_000,
  });
  await expect.poll(async () => probe(page, "machine.id"), { timeout: 30_000 }).toBe(model);
}

export async function shutDown(page: Page): Promise<void> {
  await page.getByRole("button", { name: "Shut down" }).click();
  await page.getByRole("dialog").getByRole("button", { name: "Shut Down" }).click();
}

// The guest runs in real time from here and GUI drives are paced in guest
// ticks, so it must run at 1x: floor the adaptive governor there (the
// setting a budget-pinned run uses), or a loaded runner slows the CPU and
// every wait comes up short.
export async function holdRealTime(page: Page): Promise<void> {
  await gsCallInPage(page, "scheduler.min_speed", [1]);
}

// Stay in real time until the 840AV's DSP kernel is up and idling (emr
// $8000): its bring-up, in the Enabler's RTM init a few guest seconds into
// boot, is a 68040<->DSP handshake that holds at 1x and is lost above it --
// at 2x the guest gives up on the DSP in about one boot in three, at a
// pinned 4x or 8x every time -- and a DSP held in reset means no sound
// input for the rest of the session (docs/internals/machines/av/dsp.md).
export async function waitForDspKernel(page: Page): Promise<void> {
  await expect
    .poll(async () => `${await probe(page, "machine.dsp.state")}/${await probe(page, "machine.dsp.emr")}`, {
      timeout: 120_000,
      intervals: [500],
    })
    .toBe("running/0x8000");
}

// Start the 840AV with its System 7.1 disk and boot it to the Finder:
// real time until the DSP kernel is up, accelerated for the rest of the
// boot, then back to paced.  Accelerated only for the boot: a microphone is
// a REAL-TIME stream, and a guest running faster than wall clock drains the
// ring quicker than the browser fills it, one running slower overruns it.
export async function bootAVToFinder(page: Page): Promise<void> {
  await startMachine(page, AV_ROM, "q840av", { file: AV_HD, name: AV_HD_NAME });
  await holdRealTime(page);
  await waitForDspKernel(page);
  await page.getByRole("button", { name: "Faster", exact: true }).click();
  await waitForStableScreen(page, 420_000);
  await page.getByRole("button", { name: "Real", exact: true }).click();
  await expect.poll(async () => probe(page, "scheduler.mode"), { timeout: 30_000 }).toBe("paced");
}

// --- driving the guest's GUI -------------------------------------------------
//
// suite-av measures its settle times in Mac ticks via run_ticks(); in the
// browser the machine runs on wall-clock, so the same counts convert directly
// at 60 Hz.  The floor keeps the very short waits from being swallowed by
// scheduling jitter.
export function ticksMs(n: number): number {
  return Math.max(120, Math.round((n / 60) * 1000));
}

export async function moveTo(page: Page, x: number, y: number): Promise<void> {
  await gsCallInPage(page, "machine.adb.mouse.move", [x, y, "global"]);
}

export async function mouseButton(page: Page, down: boolean): Promise<void> {
  await gsCallInPage(page, "machine.adb.mouse.click", [down]);
}

export async function click(page: Page, x: number, y: number): Promise<void> {
  await moveTo(page, x, y);
  await page.waitForTimeout(ticksMs(20));
  await mouseButton(page, true);
  await page.waitForTimeout(ticksMs(4));
  await mouseButton(page, false);
  await page.waitForTimeout(ticksMs(6));
}

export async function doubleClick(page: Page, x: number, y: number): Promise<void> {
  await moveTo(page, x, y);
  await page.waitForTimeout(ticksMs(20));
  for (let i = 0; i < 2; i++) {
    await mouseButton(page, true);
    await page.waitForTimeout(ticksMs(4));
    await mouseButton(page, false);
    await page.waitForTimeout(ticksMs(6));
  }
}

// Open the selected Finder item with Command-O.  It has no timing
// constraint, which a double-click does (the Finder's DoubleTime), so it is
// the robust way to open something from a script.  The letter goes in as an
// ADB keycode: debug_mac_resolve_key_name knows the modifiers and the named
// keys but not a-z, and `o` is $1F.
export async function openSelected(page: Page): Promise<void> {
  await gsCallInPage(page, "machine.adb.keyboard.down", ["command"]);
  await page.waitForTimeout(ticksMs(4));
  await gsCallInPage(page, "machine.adb.keyboard.press", [0x1f]);
  await page.waitForTimeout(ticksMs(4));
  await gsCallInPage(page, "machine.adb.keyboard.up", ["command"]);
}

// The Finder is up once the screen stops changing.  Polling the checksum is
// what suite-av's wait_stable() does; here the scheduler is free-running, so
// the poll is on wall-clock instead of budgeted instructions.  The screen has
// to hold still for STABLE_MS: the boot pauses for seconds at a time (the
// Welcome screen, the empty desktop before the Finder draws), and a shorter
// window called those the Finder.
const STABLE_MS = 30_000;
export async function waitForStableScreen(page: Page, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let previous = "";
  let since = Date.now();
  while (Date.now() < deadline) {
    const now = await probe(page, "machine.screen.checksum");
    if (now !== previous) {
      previous = now;
      since = Date.now();
    } else if (Date.now() - since >= STABLE_MS) {
      return;
    }
    await page.waitForTimeout(2000);
  }
  throw new Error("the guest screen never settled — no Finder");
}

// --- the browser microphone's own counters ------------------------------------

export interface MicStats {
  produced: number;
  consumed: number;
  overruns: number;
  underruns: number;
  rate: number;
  // Session-cumulative frames, unaffected by graph rebuilds. `produced` is the
  // ring's own `wr`, which resetRing() zeroes on every rebuild — so a small
  // `produced` beside a large `captured` means the graph was torn down, and a
  // small `captured` means the browser never delivered audio at all.
  captured: number;
}

export async function micStats(page: Page): Promise<MicStats> {
  return JSON.parse(
    (await page.evaluate(() =>
      JSON.stringify((globalThis as unknown as { __micStats?: () => unknown }).__micStats?.() ?? "{}"),
    )) as string,
  ) as MicStats;
}

// The capture graph as the page reports it: which node (the AudioWorklet, or
// the deprecated fallback addModule() fails SILENTLY into under cross-origin
// isolation) and whether it is live.
export async function micGraph(page: Page): Promise<{ node: string; live: boolean }> {
  return JSON.parse(
    (await page.evaluate(() =>
      JSON.stringify((globalThis as unknown as { __micLive?: () => unknown }).__micLive?.() ?? "{}"),
    )) as string,
  ) as { node: string; live: boolean };
}
