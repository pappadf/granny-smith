// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Starting a machine the way a user does: Welcome's ROM upload, then the
// New Machine dialog.

import { expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { ROMS } from "./paths";
import { uploadRomViaWelcome } from "./upload";

// The SE/30 ROM (also the IIx's and IIcx's).
export const SE30_ROM = path.join(ROMS, "iix-iicx-se30-97221136.rom");

// Upload the SE/30 ROM, then start an 8 MB SE/30 from the New Machine
// dialog and wait for the "Machine started" toast.
export async function bootSE30(page: Page): Promise<void> {
  await uploadRomViaWelcome(page, SE30_ROM);
  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption("se30");
  await page.locator("#cfg-opt-memory").selectOption("8 MB");
  await page.getByRole("button", { name: "Start", exact: true }).click();
  await expect(page.locator(".toast .msg").filter({ hasText: "Machine started" })).toBeVisible({
    timeout: 60_000,
  });
}
