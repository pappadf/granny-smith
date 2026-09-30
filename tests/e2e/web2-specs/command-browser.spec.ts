// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Terminal's command browser is a structural view of the live
// object model.  With no machine booted, the root shows the emulator's nodes
// under their domain dividers; expanding `files` lists its members, and
// selecting a method both writes its path into the prompt and shows the
// usage text the core renders (shell.usage).

import { test, expect } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";
import { CONSOLE_INPUT, focusTerminal } from "../helpers/terminal";

const SE30_ROM = path.join(
  path.resolve(__dirname, "../../data"),
  "roms",
  "iix-iicx-se30-97221136.rom",
);

test("the command browser walks the model and shows usage", async ({
  page,
}) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  const browser = page.locator(".cmd-browser");
  await expect(browser).toBeVisible({ timeout: 15_000 });

  // Domain dividers, from the model's root domains (Network appears once a
  // machine brings AppleTalk up).
  await expect(browser.locator(".divider")).toHaveText(
    ["Machine", "Emulator"],
    { timeout: 15_000 },
  );

  const rowNamed = (name: string) =>
    browser
      .locator(".cmd-row")
      .filter({
        has: page.locator(".name", { hasText: new RegExp(`^${name}$`) }),
      });

  await rowNamed("files").locator(".twistie").click();
  const ls = rowNamed("ls");
  await expect(ls).toBeVisible({ timeout: 10_000 });
  await ls.locator(".cmd-line").click();
  await expect(ls.locator(".usage")).toContainText("files.ls [path]", {
    timeout: 10_000,
  });
  await expect(page.locator(".console .cm-content")).toContainText("files.ls", {
    timeout: 10_000,
  });
});

// Typing follows into the browser, and the browser writes back: on an SE/30,
// `machine.floppy.drive[0].ins` opens machine → floppy → drive → [0] and
// selects `insert` with its usage; in its arguments the signature hint
// underlines the current one.  A selection in the browser rewrites the
// path token, and Esc puts the input back.
test("the browser follows the console and writes to it", async ({ page }) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Upload ROM..." }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);
  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await model.selectOption("se30");
  await page.locator("#cfg-ram").selectOption("8 MB");
  await page.getByRole("button", { name: "Start Machine" }).click();
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Machine started" }),
  ).toBeVisible({
    timeout: 60_000,
  });
  await page.locator('button.ptab[data-tab="terminal"]').click();

  const browser = page.locator(".cmd-browser");
  const rowNamed = (name: string) =>
    browser
      .locator(".cmd-row")
      .filter({
        has: page.locator(".name", {
          hasText: new RegExp(`^${name.replace(/[[\]]/g, "\\$&")}$`),
        }),
      });

  await focusTerminal(page);
  await page.keyboard.type("machine.floppy.drive[0].ins");
  const insert = rowNamed("insert");
  await expect(insert).toHaveClass(/selected/, { timeout: 10_000 });
  await expect(insert.locator(".usage")).toContainText(
    "machine.floppy.drive[0].insert",
    {
      timeout: 10_000,
    },
  );

  // Into the arguments: the hint underlines the current one.
  await page.keyboard.type("ert ");
  await expect(page.locator(".sig-hint .sig-arg")).toHaveText("<path>", {
    timeout: 10_000,
  });

  // The browser writes: selecting `drive` rewrites the token; Esc restores.
  await page.keyboard.press("Control+a");
  await page.keyboard.type("echo mach");
  await browser.locator(".cmd-tree").focus();
  await rowNamed("scsi").locator(".cmd-line").first().click();
  await expect(page.locator(CONSOLE_INPUT)).toHaveText("echo machine.scsi.", {
    timeout: 10_000,
  });
  await page.keyboard.press("Escape");
  await expect(page.locator(CONSOLE_INPUT)).toHaveText("echo mach");
});
