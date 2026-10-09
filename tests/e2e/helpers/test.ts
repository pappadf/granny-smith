// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The web2 specs' `test`: Playwright's, with every page.goto() made at max
// speed.  web2 paces machines in real time unless the URL says otherwise,
// and a spec that waits for a guest to boot or redraw only needs the guest
// to get there, not to take as long as a real Mac would.  So the `page`
// fixture appends `speed=turbo` to each URL it loads (the same ?speed= the
// toolbar's pacing preference reads at start-up) unless the URL already
// names a speed.
//
// A spec whose point is the pacing itself, or that feeds real-time media
// into the guest, opts out at file level:
//
//   test.use({ gsSpeed: null });

import { test as base, expect } from '@playwright/test';

export { expect };
export type { Page, Locator, Download, Route } from '@playwright/test';

// Append speed=<speed> to a page URL that names no speed of its own.
export function withSpeed(url: string, speed: string): string {
  const hash = url.indexOf('#');
  const head = hash < 0 ? url : url.slice(0, hash);
  const tail = hash < 0 ? '' : url.slice(hash);
  const q = head.indexOf('?');
  if (q >= 0 && /(^|&)speed=/i.test(head.slice(q + 1))) return url;
  return `${head}${q < 0 ? '?' : '&'}speed=${speed}${tail}`;
}

export const test = base.extend<{ gsSpeed: string | null }>({
  // The pacing every page.goto() asks for; null leaves web2's default.
  gsSpeed: ['turbo', { option: true }],
  page: async ({ page, gsSpeed }, use) => {
    if (gsSpeed) {
      const goto = page.goto.bind(page);
      page.goto = (url, options) => goto(withSpeed(url, gsSpeed), options);
    }
    await use(page);
  },
});
