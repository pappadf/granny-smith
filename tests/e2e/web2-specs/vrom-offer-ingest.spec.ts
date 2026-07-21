// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: content-addressed vROM provisioning — offer-on-ingest, no reload.
//
// Pins proposal-content-addressed-rom-provisioning.md §5/§8: the wasm
// platform enumerates /opfs/images/vrom once at startup, so a vROM uploaded
// MID-SESSION must be offered to the core's registry by the ingest path
// itself (upload.ts persist → machine.vrom.offer) or an "(auto)" boot —
// one with no explicit machine.vrom.load — would not see the file until the
// next page reload.  Also pins §3.6a: the stored name is the content hash
// (the declaration ROM's Format-Block CRC), the upload name is discarded,
// and discovery is content-based so the weird upload name never matters.
//
// Flow (all in ONE page session, no reload):
//   1. drop a JMFB vROM under a deliberately meaningless name — the toast
//      identifies it by the card it provides, and it lands at
//      /opfs/images/vrom/d1629664 (content-hashed);
//   2. upload the IIcx ROM via the Welcome button (no auto-boot);
//   3. boot the IIcx from the Terminal panel WITHOUT any vrom.load — the
//      slot-$9 default card (mdc_8_24) must content-match the offered file
//      and expose its declaration ROM (machine.nubus.slot[9].card.declrom).

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";

const DATA = path.resolve(__dirname, "../../data");
const IICX_ROM = path.join(DATA, "roms", "iix-iicx-se30-97221136.rom");
const JMFB_VROM = path.join(DATA, "roms", "mdc-8-24-revb-d1629664.vrom");

// Dispatch dragenter/dragover/drop onto the Display area with a real File in
// the DataTransfer — same concession as display-drop.spec.ts: only the drag
// gesture is synthetic; DropOverlay, bus/upload staging, the C-side probes,
// persist and the vrom offer all run for real.
async function dropOnDisplay(page: Page, fileName: string, hostFile: string) {
  const b64 = fs.readFileSync(hostFile).toString("base64");
  await page.evaluate(
    ({ name, data }: { name: string; data: string }) => {
      const el = document.querySelector(".gs-display-content, .screen-view");
      if (!el) throw new Error("display area not found");
      const r = el.getBoundingClientRect();
      const cx = r.x + r.width / 2;
      const cy = r.y + r.height / 2;
      const bin = atob(data);
      const buf = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
      const file = new File([buf], name, { type: "application/octet-stream" });
      const dt = new DataTransfer();
      dt.items.add(file);
      const fire = (type: string) =>
        el.dispatchEvent(
          new DragEvent(type, {
            bubbles: true,
            cancelable: true,
            dataTransfer: dt,
            clientX: cx,
            clientY: cy,
          }),
        );
      fire("dragenter");
      fire("dragover");
      fire("drop");
    },
    { name: fileName, data: b64 },
  );
}

// Type one shell line into the Terminal panel's xterm.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator(".xterm");
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press("Enter");
}

// Echo an expression through the terminal under a unique key and return the
// printed value (fresh key per probe so stale echoes can't satisfy the
// match). The typed line itself is echoed too — `voiN=${expr}` — so matches
// whose value still starts with `$` are the input echo, not the result;
// poll until the evaluated line lands.
let probeSeq = 0;
async function terminalEval(page: Page, expr: string): Promise<string | null> {
  const key = `voi${++probeSeq}`;
  await terminalRun(page, `echo ${key}=\${${expr}}`);
  for (let i = 0; i < 25; i++) {
    await page.waitForTimeout(400);
    const text = await page.locator(".xterm-rows").innerText();
    const values = [...text.matchAll(new RegExp(`${key}=(\\S+)`, "g"))]
      .map((m) => m[1])
      .filter((v) => !v.startsWith("$"));
    if (values.length) return values[values.length - 1];
  }
  return null;
}

test('mid-session vROM upload is offered: "(auto)" boot content-matches it without a reload', async ({
  page,
}) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  // 1. Drop the JMFB vROM under a meaningless name. The classifier probes it
  //    as a vROM by content; the toast names the card it provides (the
  //    "we identified it" signal — no filenames anywhere).
  await dropOnDisplay(page, "my_weird_upload.bin", JMFB_VROM);
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Video ROM for 'mdc_8_24'" }),
  ).toBeVisible({ timeout: 60_000 });

  // 2. Upload the IIcx ROM via the Welcome button (autoBootOnRom=false there,
  //    so nothing boots yet). Stored content-addressed under its checksum.
  const [chooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await chooser.setFiles(IICX_ROM);
  await expect(
    page
      .locator(".toast .msg")
      .filter({ hasText: "iix-iicx-se30-97221136.rom uploaded" }),
  ).toBeVisible({ timeout: 60_000 });

  // 3. Everything below runs in the shipped Terminal panel — web2 has no
  //    window.gsEval. Open it and verify the vROM landed content-hashed.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".xterm")).toBeVisible({ timeout: 15_000 });
  expect(
    await terminalEval(page, 'storage.path_size("/opfs/images/vrom/d1629664")'),
  ).toBe("32768");

  // 4. "(auto)" boot: machine.boot + rom.load with NO vrom.load. The slot-$9
  //    default card (mdc_8_24) must find its declaration ROM among the
  //    offered candidates — the file we just dropped, under its hash name.
  await terminalRun(page, 'machine.boot "iicx" 8192');
  await terminalRun(page, 'machine.rom.load "/opfs/images/rom/97221136"');
  expect(await terminalEval(page, "machine.id")).toBe("iicx");
  expect(
    await terminalEval(page, "machine.nubus.slot[9].card.declrom.present"),
  ).toBe("true");
});
