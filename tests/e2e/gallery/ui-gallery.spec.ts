// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Screenshot every UI gallery story, in every variant, in both schemes, and
// compare against the committed baselines pixel for pixel.  A styling change
// that claims to change nothing must leave every baseline as it is; an
// intentional change regenerates the affected ones (--update-snapshots, in
// the CI image) and says so in its PR.

import { test, expect, type Page } from '@playwright/test';
import { STORIES } from '../../../app/web2/src/gallery/registry';

const SCHEMES = ['dark', 'light'] as const;

// Open one story variant and wait until it has rendered and settled.
async function openStory(page: Page, story: string, variant: string, scheme: string) {
  const q = new URLSearchParams({ story, variant, theme: scheme });
  await page.goto(`/?gallery&${q.toString()}`);
  await page.waitForSelector('body[data-gallery-ready="1"]');
}

for (const story of STORIES) {
  test.describe(story.name, () => {
    for (const variant of story.variants) {
      for (const scheme of SCHEMES) {
        test(`${variant} ${scheme}`, async ({ page }) => {
          await page.setViewportSize({ width: story.width, height: story.height });
          await openStory(page, story.name, variant, scheme);
          const focus = story.focus?.[variant];
          if (focus) {
            // Keyboard focus, so :focus-visible applies.
            await page.locator(focus).first().focus();
            await page.keyboard.press('Shift');
          }
          const hover = story.hover?.[variant];
          if (hover) await page.locator(hover).first().hover();
          else await page.mouse.move(story.width - 1, story.height - 1);
          // A story that failed to render is a failure, not a baseline.
          await expect(page.locator('.gallery-error')).toHaveCount(0);
          await expect(page).toHaveScreenshot(`${story.name}-${variant}-${scheme}.png`);
        });
      }
    }
  });
}
