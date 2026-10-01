// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the real workbench in fixed states, screenshotted in both colour
// schemes.  The UI gallery (tests/e2e/gallery) covers every component against
// fixture state; this spec covers what only a live core shows — the machine
// configuration form, the SYSTEM tree and command browser, the Debug view of a
// paused machine — so a styling change that claims to change nothing can be
// checked against the whole app.  Text that moves on its own (the emulated
// screen, the console's greeting, the disassembly of a machine paused at an
// arbitrary instruction) is masked.
//
// Baselines are platform-sensitive and are recorded only in the CI image
// (tests/e2e/README.md, "UI screenshots").

import { test, expect, type Page, type Locator } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gsEvalInPage } from "../helpers/web2-eval";
import { stageOpfsFile } from "../helpers/web2-fs";
import { MANIFESTS } from "../../../app/web2/src/skins/manifests";

const PLUS_ROM = path.resolve(
  __dirname,
  "../../data/roms/plus-v3-4d1f8172.rom",
);
// The automation-only UI hook (app/web2/src/bus/testHook.ts).
interface GsUi {
  notify: (msg: string, severity: "info" | "warning" | "error") => void;
  showPdf: (doc: {
    name: string;
    title: string;
    pages: number;
    pdf: number[];
  }) => void;
}

// A one-page, empty PDF: the viewer's frame needs a document to show.
const MINIMAL_PDF = Array.from(
  Buffer.from(
    "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n" +
      "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n" +
      "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\n" +
      "trailer<</Root 1 0 R>>\n%%EOF\n",
    "latin1",
  ),
);

// Every (skin, scheme) the build has; the default skin's shots are named by
// scheme alone, another skin's carry its id (welcome-platinum-light).
const LOOKS = MANIFESTS.flatMap((m) =>
  m.schemes.map((scheme) => ({
    skin: m.id,
    scheme,
    look: m.id === "workbench" ? scheme : `${m.id}-${scheme}`,
  })),
);
const TABS = [
  "terminal",
  "machine",
  "filesystem",
  "images",
  "checkpoints",
  "debug",
  "logs",
];

test.use({ viewport: { width: 1280, height: 800 } });

// The skin and scheme come from the persisted preferences; the preview
// notice is already dismissed, so no first-visit modal covers the page.
async function prepare(
  page: Page,
  skin: string,
  scheme: string,
): Promise<void> {
  await page.addInitScript(
    ([k, s]) => {
      localStorage.setItem("gs-skin", k);
      localStorage.setItem("gs-theme", s);
      localStorage.setItem("gs-preview-notice-dismissed-v1", "1");
      localStorage.setItem("gs-panel-pos", "bottom");
    },
    [skin, scheme],
  );
}

// Load the app and wait for the core.
async function open(page: Page, url = "/index.html"): Promise<void> {
  await page.goto(url);
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
}

// What changes between runs on its own: the console's greeting names the
// build, and a checkpoint is named after the time it was made.
function masks(page: Page): Locator[] {
  return [
    page.locator(".console-output"),
    page.locator(".checkpoints-view .tbody .td"),
  ];
}

// Screenshot the page once it has settled.
async function shot(
  page: Page,
  name: string,
  extra: Locator[] = [],
): Promise<void> {
  // The skin's fonts and overrides load after the first paint.
  await page.waitForSelector("html[data-skin-ready]", { state: "attached" });
  await page.evaluate(() => document.fonts.ready);
  // The pointer rests over the empty display background, hovering nothing.
  await page.mouse.move(1279, 60);
  await page.waitForTimeout(400);
  await expect(page).toHaveScreenshot(name, {
    maxDiffPixels: 0,
    threshold: 0,
    animations: "disabled",
    caret: "hide",
    mask: [...masks(page), ...extra],
    // The emulated screen is out of scope (and its picture varies): hide it
    // instead of masking it, since a mask covers whatever lies on top.
    style: "#screen, #screen3d { visibility: hidden !important; }",
  });
}

for (const { skin, scheme, look } of LOOKS) {
  test.describe(`app states (${look})`, () => {
    test(`welcome and panel tabs, no machine (${look})`, async ({ page }) => {
      test.setTimeout(180_000);
      await prepare(page, skin, scheme);
      await open(page);
      // The storage the Filesystem and Images tabs list.
      await stageOpfsFile(
        page,
        "/opfs/images/rom/plus-v3-4d1f8172.rom",
        PLUS_ROM,
      );
      await shot(page, `welcome-${look}.png`);
      for (const tab of TABS) {
        await page.locator(`button.ptab[data-tab="${tab}"]`).click();
        await page.waitForTimeout(300);
        await shot(page, `tab-${tab}-${look}.png`);
      }
    });

    test(`machine configuration (${look})`, async ({ page }) => {
      test.setTimeout(120_000);
      await prepare(page, skin, scheme);
      await open(page);
      await stageOpfsFile(
        page,
        "/opfs/images/rom/plus-v3-4d1f8172.rom",
        PLUS_ROM,
      );
      await page.reload();
      await open(page);
      await page.locator(".card-row", { hasText: "New Machine" }).click();
      await expect(page.locator(".config-form select").first()).toBeVisible({
        timeout: 30_000,
      });
      await shot(page, `config-${look}.png`);
    });

    test(`toasts and the print dialog (${look})`, async ({ page }) => {
      test.setTimeout(120_000);
      await prepare(page, skin, scheme);
      await open(page);
      // One toast of each severity (the automation-only UI hook).
      await page.evaluate(() => {
        const ui = (window as unknown as { __gsUiForTests: GsUi })
          .__gsUiForTests;
        ui.notify("State saved", "info");
        ui.notify("Full screen blocked by the browser", "warning");
        ui.notify("Save State failed: disk full", "error");
      });
      await expect(page.locator(".toast.show")).toHaveCount(3);
      await shot(page, `toasts-${look}.png`);
      // Dismiss them one by one (each close removes a toast from the list).
      while ((await page.locator(".toast").count()) > 0)
        await page.locator(".toast .close-btn").first().click();
      await expect(page.locator(".toast")).toHaveCount(0);
      // The Logs tab behind the dialog: the console's greeting names the
      // build, and its mask would be drawn over the dialog.
      await page.locator(`button.ptab[data-tab="logs"]`).click();
      // A printed document in the viewer; the browser's PDF viewer inside
      // the frame is not ours to pin, so it is masked.
      await page.evaluate((pdf) => {
        const ui = (window as unknown as { __gsUiForTests: GsUi })
          .__gsUiForTests;
        ui.showPdf({ name: "ReadMe.pdf", title: "Read Me", pages: 1, pdf });
      }, MINIMAL_PDF);
      await expect(page.locator(".pdf-frame")).toBeVisible();
      await shot(page, `print-dialog-${look}.png`, [page.locator(".pdf-frame")]);
    });

    test(`URL boot downloading (${look})`, async ({ page }) => {
      test.setTimeout(120_000);
      await prepare(page, skin, scheme);
      // The ROM never arrives: the page stays on its download progress.
      await page.route("**/never.rom", () => undefined);
      await open(page, "/index.html?rom=never.rom&model=plus");
      await expect(page.locator(".url-boot-layer")).toBeVisible();
      await shot(page, `url-boot-${look}.png`);
    });

    test(`debug view of a paused machine (${look})`, async ({ page }) => {
      test.setTimeout(180_000);
      await prepare(page, skin, scheme);
      const body = fs.readFileSync(PLUS_ROM);
      await page.route("**/url-machine.rom", (route) =>
        route.fulfill({
          status: 200,
          contentType: "application/octet-stream",
          body,
        }),
      );
      await open(page, "/index.html?rom=url-machine.rom&model=plus");
      await expect(page.locator(".gs-statusbar .sb-state .label")).toHaveText(
        "Running",
        {
          timeout: 60_000,
        },
      );
      await gsEvalInPage(page, "scheduler.stop");
      await expect(page.locator(".gs-statusbar .sb-state .label")).toHaveText(
        "Paused",
        {
          timeout: 15_000,
        },
      );
      await page.locator('button.ptab[data-tab="debug"]').click();
      for (const title of ["Registers", "Breakpoints"]) {
        const toggle = page
          .locator("header.header", { hasText: title })
          .locator("button.toggle");
        if ((await toggle.getAttribute("aria-expanded")) !== "true")
          await toggle.click();
      }
      await expect(page.locator(".disasm-pane .row").first()).toBeVisible({
        timeout: 15_000,
      });
      // Where the machine stopped varies: mask the values, keep the chrome.
      await shot(page, `debug-paused-${look}.png`, [
        page.locator(
          ".disasm-pane .banner, .disasm-pane .addr, .disasm-pane .mnem, .disasm-pane .ops",
        ),
        page.locator("input.reg-value"),
        page.locator(".sb-drive"),
      ]);
    });
  });
}
