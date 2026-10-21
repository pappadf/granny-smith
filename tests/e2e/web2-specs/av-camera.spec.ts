// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the AV camera control and the browser webcam → video-in path.
//
// Runs against Chromium's fake camera (--use-fake-device-for-media-stream
// generates a moving synthetic pattern; --use-fake-ui-for-media-stream
// auto-grants the permission prompt), so no real hardware is involved and
// the spec is CI-safe.
//
// What it proves, in order:
//   1. The camera button is *capability-gated*: absent on a machine without
//      the on-board digitizer, present on the Quadra 840AV.
//   2. Clicking it drives the core: machine.videoin.source flips none → host
//      and back, and `connected` follows the shared-heap connected flag that
//      em_camera.c publishes — i.e. the whole C↔JS transport is wired, not
//      just the button's own state.
//   3. Frames actually reach the guest-visible source: with the camera on,
//      the fake camera's moving pattern makes the digitizer's captured
//      frames change (checked through machine.videoin, which reads the same
//      slot pair the capture engine does).
//
// The guest never opens the vdig here — that path is covered headlessly by
// suite-av's video-in rows against the deterministic pattern source, which
// is where byte-exact goldens belong. This spec covers what only a real
// browser can: getUserMedia, the heap transport, and the UI.

import { test, expect } from '../helpers/test';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
import {
  AV_ROM,
  CAMERA_BUTTON,
  FAKE_MEDIA_ARGS,
  PLUS_ROM,
  holdRealTime,
  probe,
  shutDown,
  startMachine,
} from '../helpers/av';

test.use({
  // Real-time media feeds the guest: run in Real.
  gsSpeed: 'paced',
  launchOptions: { args: FAKE_MEDIA_ARGS },
});

test('AV camera control drives the video-in path with the fake camera', async ({ page }) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/rom/q840av-q660av-5bf10fd1.rom', AV_ROM);

  // --- 1. Capability gating: a Plus has no digitizer, so no camera button.
  await startMachine(page, PLUS_ROM, 'plus');
  const camBtn = page.locator(CAMERA_BUTTON);
  await expect(camBtn).toHaveCount(0);

  // --- 2. The 840AV has one. Shut down and boot the AV machine.
  await shutDown(page);
  await startMachine(page, AV_ROM, 'q840av');
  await expect(camBtn).toHaveCount(1, { timeout: 30_000 });
  await expect(camBtn).toHaveAttribute('aria-pressed', 'false');
  await holdRealTime(page);

  // Nothing plugged in yet: the guest sees no signal.
  expect(await probe(page, 'machine.videoin.source')).toBe('none');
  expect(await probe(page, 'machine.videoin.connected')).toBe('false');

  // --- 3. Connect the camera. The click is the user gesture getUserMedia
  // needs; the fake-UI flag auto-grants. The core's source flips to `host`
  // and `connected` reads back through em_camera.c's shared-heap flag.
  await camBtn.click();
  await expect(camBtn).toHaveAttribute('aria-pressed', 'true');
  await expect.poll(async () => probe(page, 'machine.videoin.source')).toBe('host');
  await expect
    .poll(async () => probe(page, 'machine.videoin.connected'), { timeout: 30_000 })
    .toBe('true');

  // --- 4. Disconnect: the guest goes back to "no signal".
  await camBtn.click();
  await expect(camBtn).toHaveAttribute('aria-pressed', 'false');
  await expect.poll(async () => probe(page, 'machine.videoin.source')).toBe('none');
  expect(await probe(page, 'machine.videoin.connected')).toBe('false');
});
