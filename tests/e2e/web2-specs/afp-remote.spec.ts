// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Remote AFP volumes in the browser: the page puts "Internet Archive" on the
// AppleTalk network beside Shared Folders, and answers the core's requests
// for listings and files (bus/remoteAfp.ts).  The guest side is the
// integration suite's appletalk-afp-remote script (Chooser -> the second
// server -> Guest -> mount -> open the volume -> copy a file -> open an
// archive as a folder on System 6.0.8), driven here from the terminal; the
// volume is an archive.org item, its listing and files served by
// page.route() from the test data, so only the transport is stubbed:
// the provider's metadata listing, its /cors/ fetch, the deferred AFP
// commands and the unpacking all run for real.

import { test, expect, type Route } from '../helpers/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile, stageOpfsText } from '../helpers/web2-fs';
import { gsEvalInPage } from '../helpers/web2-eval';
import { terminalRun } from '../helpers/terminal';

const DATA = path.resolve(__dirname, '../../data');
const PLUS_ROM = path.join(DATA, 'roms', 'plus-v3-4d1f8172.rom');
const SYSTEM_HD = path.join(DATA, 'systems', 'system_6_0_8_20mb_8_24gc.img');
const ARCHIVE = path.join(DATA, 'apps', 'MacTest_Disk.image_.sit_.hqx');

const ITEM = 'gs-e2e-item';
const CACHE = '/opfs/afp-remote/e2e';
const FILES: Record<string, Buffer> = {
  'Read Me': Buffer.from('Served from afar.\n'),
  'Docs/Notes': Buffer.from('Notes.\n'),
  'MacTest_Disk.image_.sit_.hqx': fs.readFileSync(ARCHIVE),
};

// The item's file list as archive.org's metadata API gives it, with the
// files of its own that the provider leaves out.
const METADATA = {
  result: [
    ...Object.entries(FILES).map(([name, body]) => ({
      name,
      source: 'original',
      size: String(body.length),
      mtime: '1777240000',
      format: 'Unknown',
    })),
    { name: `${ITEM}_meta.xml`, source: 'original', format: 'Metadata' },
    { name: '__ia_thumb.jpg', source: 'original', format: 'Item Tile', size: '10' },
  ],
};

const CORS = { 'access-control-allow-origin': '*' };

const SCRIPT = `machine.boot model="plus" rom="/opfs/images/rom/4D1F8172" config="{\\"options\\":{\\"appletalk\\":\\"inactive\\"}}"
scheduler.stop
scheduler.mode = "turbo"
machine.scsi.attach_hd "/opfs/images/hd/afp-hd.img" 0
machine.rtc.pram.boot_device = 0
machine.rtc.time = "2026-04-26T21:30:36"
scheduler.run 30000000
def wait_mouse_taken(ceiling) {
    let spent = 0
    while machine.adb.mouse.pending {
        assert $spent < $ceiling "the guest did not take the mouse input"
        scheduler.run 250000
        $spent = $spent + 250000
    }
}
def wait_remote() {
    let spent = 0
    while len(appletalk.remote.requests) > 0 {
        assert $spent < 4000000000 "a request was not answered"
        scheduler.run 1000000
        $spent = $spent + 1000000
    }
}
def afp_click(x, y, settle) {
    machine.adb.mouse.move $x $y
    scheduler.run 2000000
    machine.adb.mouse.click true
    machine.adb.mouse.click false
    wait_mouse_taken(100000000)
    scheduler.run $settle
}
def afp_open(x, y) {
    machine.adb.mouse.move $x $y
    scheduler.run 2000000
    machine.adb.mouse.click true
    machine.adb.mouse.click false
    wait_mouse_taken(100000000)
    scheduler.run 80000
    machine.adb.mouse.click true
    machine.adb.mouse.click false
    wait_mouse_taken(100000000)
    scheduler.run 20000000
    wait_remote()
    scheduler.run 40000000
    wait_remote()
    scheduler.run 20000000
}
machine.adb.mouse.move 25 10
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 4000000
machine.adb.mouse.move 25 123
scheduler.run 4000000
machine.adb.mouse.click false
scheduler.run 60000000
afp_click(95, 90, 20000000)
afp_click(190, 206, 60000000)
afp_click(280, 101, 6000000)
afp_click(333, 178, 40000000)
afp_click(392, 259, 40000000)
machine.adb.mouse.move 150 113
scheduler.run 4000000
machine.adb.mouse.click true
machine.adb.mouse.click false
wait_mouse_taken(100000000)
scheduler.run 8000000
afp_click(333, 259, 60000000)
afp_click(72, 47, 40000000)
assert appletalk.afp.sessions[0].server_socket == appletalk.nbp["Internet Archive"].socket "the session is not with Internet Archive"
afp_open(468, 108)
assert appletalk.remote.issued == 1 "the root listing was not the one request"
machine.adb.mouse.move 110 105
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 4000000
machine.adb.mouse.move 200 90
scheduler.run 2000000
machine.adb.mouse.move 400 60
scheduler.run 2000000
machine.adb.mouse.move 470 55
scheduler.run 4000000
machine.adb.mouse.click false
wait_mouse_taken(100000000)
scheduler.run 20000000
wait_remote()
scheduler.run 60000000
afp_open(237, 110)
echo "remote: issued=\${appletalk.remote.issued} failed=\${appletalk.remote.failed} readme=\${files.path_size("${CACHE}/Read Me")} system=\${files.path_exists("${CACHE}/MacTest_Disk/System")}"
`;

test('a remote volume of archive.org is listed, fetched and unpacked for the guest', async ({ page }) => {
  test.setTimeout(12 * 60 * 1000);
  const asked: string[] = [];
  // archive.org from the test data; anything else of archive.org, and Mac
  // Hut's probe, fails (so only Internet Archive is published).
  await page.route('https://archive.org/**', (route: Route) => {
    const url = new URL(route.request().url());
    asked.push(url.pathname);
    if (url.pathname === `/metadata/${ITEM}/files`)
      return route.fulfill({ status: 200, headers: CORS, contentType: 'application/json', body: JSON.stringify(METADATA) });
    const prefix = `/cors/${ITEM}/`;
    if (url.pathname.startsWith(prefix)) {
      const body = FILES[decodeURIComponent(url.pathname.slice(prefix.length))];
      if (body) return route.fulfill({ status: 200, headers: CORS, contentType: 'application/octet-stream', body });
    }
    return route.fulfill({ status: 404, headers: CORS, body: 'not found' });
  });
  await page.route('https://machut.net/**', (route: Route) => route.abort());

  await gotoWeb2(page);
  // The page published the server and its volumes at startup.
  await expect
    .poll(async () => ((await gsEvalInPage(page, 'appletalk.remote.volumes')) as unknown[]).length, {
      timeout: 30_000,
    })
    .toBe(4);
  expect(await gsEvalInPage(page, 'appletalk.remote.servers')).toEqual(['Internet Archive']);
  // One volume of a test item in place of the collections.
  for (const v of ['Software Library', 'Shareware Games', 'HyperCard Stacks', 'Macintosh Garden'])
    expect(await gsEvalInPage(page, 'appletalk.remote.remove_volume', ['Internet Archive', v])).toBe(true);
  expect(
    await gsEvalInPage(page, 'appletalk.remote.add_volume', ['Internet Archive', 'Remote', `ia:item/${ITEM}`, CACHE]),
  ).toBe(true);

  await stageOpfsFile(page, '/opfs/images/hd/afp-hd.img', SYSTEM_HD);
  await stageOpfsFile(page, '/opfs/images/rom/4D1F8172', PLUS_ROM);
  await stageOpfsText(page, '/opfs/upload/afp-remote.script', SCRIPT);

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, 'include "/opfs/upload/afp-remote.script"');
  await expect(page.locator('.console-output')).toContainText(/remote: issued=|failed|error/i, {
    timeout: 10 * 60 * 1000,
  });
  const text = (await page.locator('.console-output').innerText()).replace(/\n/g, '');
  // The root listing, Read Me (dragged to the startup disk), the archive.
  expect(text).toContain('remote: issued=3 failed=0 readme=18 system=true');
  expect(asked).toContain(`/metadata/${ITEM}/files`);
  expect(asked).toContain(`/cors/${ITEM}/Read%20Me`);
  expect(asked).toContain(`/cors/${ITEM}/MacTest_Disk.image_.sit_.hqx`);
});
