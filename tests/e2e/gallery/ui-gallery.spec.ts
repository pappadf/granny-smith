// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Screenshot every UI gallery story, in every variant, in every skin, and
// compare against the committed baselines pixel for pixel.  The shots are
// named <story>-<variant>-<skin>.  A styling change
// that claims to change nothing must leave every baseline as it is; an
// intentional change regenerates the affected ones (--update-snapshots, in
// the CI image) and says so in its PR.

import { test, expect, type Page } from "@playwright/test";
import { STORIES } from "../../../app/web2/src/gallery/registry";
import { MANIFESTS } from "../../../app/web2/src/skins/manifests";

// The Workbench skins are compared exactly.  The others draw large anti-aliased
// curves, gradients and translucency, which Chromium re-rasters a level or
// few apart depending on the page's compositing history (the same DOM, shot
// twice, differs): they allow that much colour noise per pixel, and still not
// a single pixel beyond it.
const RASTER_NOISE = { threshold: 0.03 };
const EXACT = new Set(["workbench", "workbench-light"]);

// Open one story variant and wait until it has rendered and settled.
async function openStory(
  page: Page,
  story: string,
  variant: string,
  skin: string,
) {
  const q = new URLSearchParams({ story, variant, skin });
  await page.goto(`/?gallery&${q.toString()}`);
  await page.waitForSelector('body[data-gallery-ready="1"]');
}

for (const story of STORIES) {
  test.describe(story.name, () => {
    for (const variant of story.variants) {
      for (const { id: skin } of MANIFESTS) {
        test(`${variant} ${skin}`, async ({ page }) => {
          await page.setViewportSize({
            width: story.width,
            height: story.height,
          });
          await openStory(page, story.name, variant, skin);
          const focus = story.focus?.[variant];
          if (focus) {
            // Keyboard focus, so :focus-visible applies.
            await page.locator(focus).first().focus();
            await page.keyboard.press("Shift");
          }
          const hover = story.hover?.[variant];
          if (hover) await page.locator(hover).first().hover();
          else await page.mouse.move(story.width - 1, story.height - 1);
          // A story that failed to render is a failure, not a baseline.
          await expect(page.locator(".gallery-error")).toHaveCount(0);
          await expect(page).toHaveScreenshot(
            `${story.name}-${variant}-${skin}.png`,
            EXACT.has(skin) ? {} : RASTER_NOISE,
          );
        });
      }
    }
  });
}
