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

import { test, expect } from "../helpers/test";
import { gsCallInPage } from "../helpers/web2-eval";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";
import {
  AV_ROM,
  FAKE_MEDIA_ARGS,
  MIC_BUTTON,
  PLUS_ROM,
  holdRealTime,
  micGraph,
  micStats,
  probe,
  probeNumber,
  shutDown,
  startMachine,
} from "../helpers/av";

test.use({
  // Real-time media feeds the guest: keep web2's default pacing.
  gsSpeed: null,
  launchOptions: { args: FAKE_MEDIA_ARGS },
});

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
  await startMachine(page, PLUS_ROM, "plus");
  const micBtn = page.locator(MIC_BUTTON);
  await expect(micBtn).toHaveCount(0);

  // --- 2. The 840AV has one.
  await shutDown(page);
  await startMachine(page, AV_ROM, "q840av");
  await expect(micBtn).toHaveCount(1, { timeout: 30_000 });
  await expect(micBtn).toHaveAttribute("aria-pressed", "false");
  await holdRealTime(page);

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
  // The ROM is still booting at this point (no disk: it ends at the
  // flashing "?"), and its own sound init writes the Singer's control
  // register on the way, which switches the input engine back off.  So arm
  // it, hold for a few seconds, and re-arm until it sticks; each poke is
  // read back so a failure names the one that did not land.
  let armed = false;
  for (let attempt = 0; attempt < 15 && !armed; attempt++) {
    await gsCallInPage(page, "machine.memory.poke.l", [0x50f31210, 0x10000]);
    expect(await probeNumber(page, "machine.memory.peek.l", [0x50f31210])).toBe(0x10000);
    await gsCallInPage(page, "machine.memory.poke.w", [0x50f31218, 240]);
    expect(await probeNumber(page, "machine.memory.peek.w", [0x50f31218])).toBe(240);
    await gsCallInPage(page, "machine.memory.poke.w", [0x50f31200, 0x0080]);
    expect(await probe(page, "machine.sound.in_enabled")).toBe("true");
    await page.waitForTimeout(4000);
    armed = (await probe(page, "machine.sound.in_enabled")) === "true";
  }
  // Log the whole picture BEFORE asserting: a failed assertion that takes
  // the diagnostics down with it is what made the last three rounds guesswork.
  console.log("  micStats AFTER pSndInEn:", JSON.stringify(await micStats(page)));
  console.log("  graph:", JSON.stringify(await micGraph(page)));
  expect(await probe(page, "machine.sound.in_enabled")).toBe("true");
  // The worklet, not the deprecated fallback: addModule() from a blob: URL
  // is rejected under cross-origin isolation and fails SILENTLY into it.
  const graph = await micGraph(page);
  expect(graph.node, "fell back off the AudioWorklet path").toBe(
    "AudioWorkletNode",
  );
  expect(graph.live).toBe(true);

  // The seam is pulled: the frame engine takes samples from the host source.
  await expect
    .poll(async () => probeNumber(page, "machine.audioin.samples"), {
      timeout: 30_000,
    })
    .toBeGreaterThan(0);

  // --- 5. THE ASSERTIONS THAT MATTER.
  //
  // (a) The transport is alive end to end. `produced` is written by the
  //     AudioWorklet on the browser main thread and `consumed` by the
  //     emulator worker through the seam, so both climbing proves
  //     getUserMedia → worklet → shared-heap ring → platform_audio_in_frames →
  //     the Singer's DMA. This is the assertion that would have caught the
  //     original bug (the worklet was never rendered, so `produced` stayed
  //     at 0 forever).
  const s1 = await micStats(page);
  expect(s1.produced, "the worklet is not producing samples").toBeGreaterThan(
    10_000,
  );
  expect(s1.consumed, "the guest is not consuming samples").toBeGreaterThan(
    10_000,
  );
  await page.waitForTimeout(2000);
  const s2 = await micStats(page);
  expect(s2.produced, "production stalled").toBeGreaterThan(s1.produced);
  expect(s2.consumed, "consumption stalled").toBeGreaterThan(s1.consumed);

  // (b) Real audio reaches guest RAM. Chromium's fake microphone emits a
  //     PERIODIC BEEP rather than a continuous tone, so a single snapshot of
  //     the double buffer usually lands in its silence — poll instead, and
  //     take the loudest sample seen. The bar is well above the codec's ±1
  //     LSB dither, which is exactly what a dead capture path would leave
  //     here (and what came back as full-scale white noise).
  let peak = 0;
  const peakDeadline = Date.now() + 30_000;
  while (Date.now() < peakDeadline && peak <= 16) {
    for (let i = 0; i < 8; i++) {
      const w = await probeNumber(page, "machine.memory.peek.w", [0x10000 + i * 244]);
      const sgn = w > 32767 ? w - 65536 : w;
      peak = Math.max(peak, Math.abs(sgn));
    }
    await page.waitForTimeout(250);
  }
  console.log("  peak sample seen in the guest double buffer:", peak);
  expect(
    peak,
    `guest buffer peaks at ${peak} — at or below the codec's dither, so no audio is arriving`,
  ).toBeGreaterThan(16);

  // --- 6. Once connected, the button is a DEVICE MENU.
  // getUserMedia takes the system default input, which on a docked machine is
  // routinely not the user's microphone — an empty headset jack on a dock is
  // live, well-behaved, and delivers its own ~-80 dBFS dither forever, which
  // from inside the ring is indistinguishable from a broken capture path.
  // Being able to pick the device is the fix, so the menu has to be there.
  await micBtn.click();
  const menu = page.locator('[role="menu"]');
  await expect(menu).toBeVisible();
  await expect(menu.getByText("System default")).toBeVisible();
  // The fake device enumerates like a real one, so a selectable input is
  // listed alongside the default.
  const deviceItems = menu.locator("[role^='menuitem']");
  expect(await deviceItems.count()).toBeGreaterThan(2); // disconnect + default + ≥1

  // --- 7. Disconnect from that menu: the guest goes back to no microphone.
  await menu.getByText("Disconnect microphone").click();
  await expect(micBtn).toHaveAttribute("aria-pressed", "false");
  await expect
    .poll(async () => probe(page, "machine.audioin.source"))
    .toBe("none");
  expect(await probe(page, "machine.audioin.connected")).toBe("false");
});
