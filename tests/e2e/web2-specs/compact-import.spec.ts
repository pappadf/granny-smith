// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: large disk images are imported streamed into a compact UDIF.
//
// The browser charges a file's logical length against the origin's quota,
// zeros and holes included, so an image stored expanded costs its full size,
// and staging it first costs that twice.  The import pipeline
// (bus/importImage.ts) streams the decoded bytes into the core's UDIF writer
// instead.  The invariant asserted here: while a 2 GiB image that is 1 %
// non-zero is imported, navigator.storage.estimate().usage never grows by
// more than the stored image plus 8 MiB, and what is stored is a small
// fraction of the disk.
//
// The image is never a fixture file: a local HTTP server generates it as a
// stream (deterministic, so every run is the same bytes), raw with and
// without Content-Length, and inside a zip whose deflated member trails its
// sizes in a data descriptor -- the forward-only case.  A blank disk made by
// the Create Image path is checked too.
//
// Also: the zip import's JS heap stays bounded (it never holds the disk); an
// import cancelled midway, or one that runs out of quota, leaves nothing
// behind -- no stored image, no .part.

import { test, expect, type Page } from "@playwright/test";
import * as http from "node:http";
import type { AddressInfo } from "node:net";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as zlib from "node:zlib";
import { gsEvalInPage, scratchFiles } from "../helpers/web2-eval";

// GS_COMPACT_IMPORT_MB shrinks the disk for a quick local run.
const DISK_BYTES =
  Number(process.env.GS_COMPACT_IMPORT_MB ?? 2048) * 1024 * 1024;
const CHUNK = 64 * 1024;

// Chunk `i` of the generated disk: every 100th chunk is pseudo-random bytes
// (incompressible), the rest zeros; chunk 0 starts with an Apple partition
// map signature so the disk looks like one.
function diskChunk(i: number): Buffer {
  const b = Buffer.alloc(CHUNK);
  if (i === 0) b.write("ER", 0, "latin1");
  if (i % 100 === 1) {
    let x = (i * 2654435761) >>> 0;
    for (let k = 0; k < CHUNK; k += 4) {
      x ^= x << 13;
      x ^= x >>> 17;
      x ^= x << 5;
      b.writeUInt32LE(x >>> 0, k);
    }
  }
  return b;
}

const N_CHUNKS = DISK_BYTES / CHUNK;

// Write the whole disk to `res`, honouring backpressure; `pauseMs` paces it
// (a slow server), and a client that goes away ends it.
async function writeDisk(
  res: NodeJS.WritableStream,
  onChunk?: (b: Buffer) => void,
  pauseMs = 0,
): Promise<void> {
  let gone = false;
  res.once("close", () => (gone = true));
  for (let i = 0; i < N_CHUNKS && !gone; i++) {
    const c = diskChunk(i);
    onChunk?.(c);
    if (!res.write(c)) await new Promise((r) => res.once("drain", r));
    if (pauseMs) await new Promise((r) => setTimeout(r, pauseMs));
  }
}

// A zip of one deflated member, sizes in a trailing data descriptor (bit 3),
// streamed as it is compressed.
async function writeZip(res: http.ServerResponse, name: string): Promise<void> {
  const nameBuf = Buffer.from(name, "utf8");
  const lh = Buffer.alloc(30);
  lh.writeUInt32LE(0x04034b50, 0);
  lh.writeUInt16LE(20, 4);
  lh.writeUInt16LE(0x0808, 6); // bit 3 (descriptor) + UTF-8 name
  lh.writeUInt16LE(8, 8); // deflate
  lh.writeUInt16LE(nameBuf.length, 26);
  res.write(Buffer.concat([lh, nameBuf]));
  let crc = 0;
  let compressed = 0;
  const deflate = zlib.createDeflateRaw({ level: 1 });
  deflate.on("data", (d: Buffer) => {
    compressed += d.length;
    res.write(d);
  });
  const done = new Promise<void>((r) => deflate.on("end", () => r()));
  await writeDisk(deflate, (c) => (crc = zlib.crc32(c, crc)));
  deflate.end();
  await done;
  const dd = Buffer.alloc(16);
  dd.writeUInt32LE(0x08074b50, 0);
  dd.writeUInt32LE(crc >>> 0, 4);
  dd.writeUInt32LE(compressed >>> 0, 8);
  dd.writeUInt32LE(DISK_BYTES >>> 0, 12); // 2^30 fits in 32 bits
  res.write(dd);
  res.end(); // no central directory needed: the reader stops at the descriptor
}

let server: http.Server;
let origin = "";

test.beforeAll(async () => {
  server = http.createServer((req, res) => {
    const headers = {
      "Access-Control-Allow-Origin": "*",
      "Cross-Origin-Resource-Policy": "cross-origin",
      "Content-Type": "application/octet-stream",
    };
    const url = req.url ?? "";
    if (url.startsWith("/sized/")) {
      res.writeHead(200, { ...headers, "Content-Length": String(DISK_BYTES) });
      writeDisk(res).then(() => res.end());
    } else if (url.startsWith("/chunked/")) {
      res.writeHead(200, headers); // no length: chunked transfer
      writeDisk(res).then(() => res.end());
    } else if (url.startsWith("/slow/")) {
      // Chunked and paced to take a minute or more: long enough to cancel.
      res.writeHead(200, headers);
      writeDisk(res, undefined, Math.max(5, Math.ceil(60_000 / N_CHUNKS))).then(
        () => res.end(),
      );
    } else if (url.startsWith("/zip/")) {
      res.writeHead(200, headers);
      void writeZip(res, "Big Disk.img");
    } else {
      res.writeHead(404, headers);
      res.end();
    }
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  origin = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
});

test.afterAll(async () => {
  await new Promise<void>((r) => server.close(() => r()));
});

async function waitReady(page: Page): Promise<void> {
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
}

// Sample the origin's usage every 250 ms from the moment each document of the
// page starts (an init script, so the import a URL starts on load is
// covered); the sampler keeps the first sample and the max.
async function installUsageSampler(page: Page): Promise<void> {
  await page.addInitScript(() => {
    const w = window as unknown as { __usageBase?: number; __usageMax: number };
    w.__usageMax = 0;
    const sample = async () => {
      const u = (await navigator.storage.estimate()).usage ?? 0;
      if (w.__usageBase === undefined) w.__usageBase = u;
      if (u > w.__usageMax) w.__usageMax = u;
    };
    void sample();
    window.setInterval(sample, 250);
  });
}

// The usage growth seen: the max sample (and the usage now) less the first.
async function usageGrowth(page: Page): Promise<number> {
  return page.evaluate(async () => {
    const w = window as unknown as { __usageBase?: number; __usageMax: number };
    const u = (await navigator.storage.estimate()).usage ?? 0;
    return Math.max(w.__usageMax, u) - (w.__usageBase ?? 0);
  });
}

// The files under /opfs/images/hd, with their sizes.
async function storedDisks(
  page: Page,
): Promise<Array<{ name: string; size: number }>> {
  const list = (await gsEvalInPage(page, "files.list", [
    "/opfs/images/hd",
  ])) as Array<{
    name: string;
    kind: string;
    size: number;
  }>;
  return Array.isArray(list) ? list.filter((e) => e.kind !== "directory") : [];
}

for (const variant of ["sized", "chunked", "zip"] as const) {
  test(`HD= of a 2 GiB disk (${variant}) is stored compact, never expanded`, async ({
    page,
  }) => {
    test.setTimeout(600_000);
    const log: string[] = [];
    page.on("console", (m) => log.push(`[${m.type()}] ${m.text()}`));
    await installUsageSampler(page);
    // The page's JS heap, sampled from outside (CDP) through the import.
    const cdp = await page.context().newCDPSession(page);
    let heapBase = -1;
    let heapMax = 0;
    const heapTimer = setInterval(() => {
      cdp
        .send("Runtime.getHeapUsage")
        .then(({ usedSize }) => {
          if (heapBase < 0) heapBase = usedSize;
          heapMax = Math.max(heapMax, usedSize);
        })
        .catch(() => undefined);
    }, 250);

    // URL media without a ROM: the disk is fetched and stored; nothing boots.
    const value =
      variant === "zip"
        ? `${origin}/zip/big.zip/Big%20Disk.img`
        : `${origin}/${variant}/Big%20Disk.img`;
    await page.goto(`/index.html?HD0=${encodeURIComponent(value)}`);
    await waitReady(page);
    await expect
      .poll(async () => (await storedDisks(page)).map((d) => d.name), {
        timeout: 540_000,
        message: `never stored.\nConsole:\n${log.slice(-40).join("\n")}`,
      })
      .toContain("Big_Disk.dmg");
    clearInterval(heapTimer);
    const growth = await usageGrowth(page);
    const disk = (await storedDisks(page)).find(
      (d) => d.name === "Big_Disk.dmg",
    )!;

    // The zip is unpacked as it streams: the heap never holds the disk, or
    // much of it.  (The raw variants have no page-side decoding to bound.)
    if (variant === "zip") {
      expect(heapBase).toBeGreaterThan(0);
      expect(heapMax - heapBase).toBeLessThan(32 * 1024 * 1024);
    }

    // Stored: about the 1 % that is not zero.
    expect(disk.size).toBeLessThan(DISK_BYTES * 0.015);
    // Never more than the stored image plus one transfer window or so.
    expect(growth).toBeLessThan(disk.size + 8 * 1024 * 1024);
    // Nothing left in staging.
    const staging = await scratchFiles(page);
    expect(staging.filter((n) => n.includes(".part"))).toEqual([]);
    // The image is the disk: its decoded size, verified checksums.
    const info = (await gsEvalInPage(page, "files.udif_info", [
      "/opfs/images/hd/Big_Disk.dmg",
    ])) as { bytes: number; gs_profile: boolean };
    expect(info.bytes).toBe(DISK_BYTES);
    expect(info.gs_profile).toBe(true);
    expect(
      await gsEvalInPage(page, "files.verify", [
        "/opfs/images/hd/Big_Disk.dmg",
      ]),
    ).toMatchObject({
      sectors: DISK_BYTES / 512,
    });
  });
}

test("a blank 2 GB disk made as .dmg costs a few KB", async ({ page }) => {
  test.setTimeout(180_000);
  await installUsageSampler(page);
  await page.goto("/index.html");
  await waitReady(page);
  const before = await usageGrowth(page);
  expect(
    await gsEvalInPage(page, "files.hd_create", [
      "/opfs/images/hd/blank.dmg",
      "2147483648",
    ]),
  ).toBe(true);
  const growth = (await usageGrowth(page)) - before;
  const size = (await gsEvalInPage(page, "files.path_size", [
    "/opfs/images/hd/blank.dmg",
  ])) as number;
  expect(size).toBeLessThan(4096);
  expect(growth).toBeLessThan(1024 * 1024);
});

// The same disk as a local file, picked through Welcome's "Upload ROM..."
// (the auto-detecting path a drop takes).  The file is sparse on the test
// host -- free there -- and read by the page through Blob.slice, never whole.
test("a 2 GiB local file upload is stored compact, never expanded", async ({
  page,
}) => {
  test.setTimeout(600_000);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "gs-compact-"));
  const file = path.join(dir, "Local Disk.img");
  const fd = fs.openSync(file, "w");
  for (let i = 0; i < N_CHUNKS; i++) {
    if (i === 0 || i % 100 === 1)
      fs.writeSync(fd, diskChunk(i), 0, CHUNK, i * CHUNK);
  }
  fs.ftruncateSync(fd, DISK_BYTES);
  fs.closeSync(fd);
  try {
    await installUsageSampler(page);
    await page.goto("/index.html");
    await waitReady(page);
    const cont = page.getByRole("button", { name: "Continue" });
    if (await cont.isVisible().catch(() => false)) await cont.click();
    const [chooser] = await Promise.all([
      page.waitForEvent("filechooser"),
      page.getByRole("button", { name: "Upload ROM..." }).click(),
    ]);
    await chooser.setFiles(file);
    await expect
      .poll(async () => (await storedDisks(page)).map((d) => d.name), {
        timeout: 540_000,
      })
      .toContain("Local_Disk.dmg");
    const growth = await usageGrowth(page);
    const disk = (await storedDisks(page)).find(
      (d) => d.name === "Local_Disk.dmg",
    )!;
    expect(disk.size).toBeLessThan(DISK_BYTES * 0.015);
    expect(growth).toBeLessThan(disk.size + 8 * 1024 * 1024);
    const info = (await gsEvalInPage(page, "files.udif_info", [
      "/opfs/images/hd/Local_Disk.dmg",
    ])) as { bytes: number };
    expect(info.bytes).toBe(DISK_BYTES);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

// Nothing of an import is left once it fails or is cancelled.
async function expectNothingLeft(page: Page): Promise<void> {
  await expect
    .poll(async () => {
      const staging = await scratchFiles(page);
      return [
        ...staging.filter((n) => n.includes(".part")),
        ...(await storedDisks(page)).map((d) => d.name),
      ];
    })
    .toEqual([]);
}

test("an HD= import cancelled midway leaves nothing behind", async ({
  page,
}) => {
  test.setTimeout(180_000);
  await page.goto(
    `/index.html?HD0=${encodeURIComponent(`${origin}/slow/Big%20Disk.img`)}`,
  );
  await waitReady(page);
  const cont = page.getByRole("button", { name: "Continue" });
  if (await cont.isVisible().catch(() => false)) await cont.click();
  // Under way: the part file exists, and the status bar offers Cancel.
  await expect
    .poll(
      async () =>
        (await scratchFiles(page)).some((n) => n.endsWith(".dmg.part")),
      { timeout: 60_000 },
    )
    .toBe(true);
  await page.locator(".upload-cancel").click();
  await expect(
    page.locator(".gs-toast__msg", { hasText: "cancelled" }),
  ).toBeVisible();
  await expect(page.locator(".upload-cancel")).toHaveCount(0);
  await expectNothingLeft(page);
});

test("an import that runs out of quota fails cleanly", async ({ page }) => {
  test.setTimeout(300_000);
  await page.goto("/index.html");
  await waitReady(page);
  // Quota: what the origin uses now plus a third of what the disk stores
  // (about 1 % of it), so the writer runs out partway.  No Content-Length,
  // so the up-front check cannot refuse it: the failure is mid-write.
  const usage = await page.evaluate(
    async () => (await navigator.storage.estimate()).usage ?? 0,
  );
  const cdp = await page.context().newCDPSession(page);
  const pageOrigin = new URL(page.url()).origin;
  await cdp.send("Storage.overrideQuotaForOrigin", {
    origin: pageOrigin,
    quotaSize: usage + Math.round((DISK_BYTES * 0.01) / 3),
  });
  try {
    await page.goto(
      `/index.html?HD0=${encodeURIComponent(`${origin}/chunked/Big%20Disk.img`)}`,
    );
    await waitReady(page);
    await expect(
      page.locator(".gs-toast__msg", { hasText: "Could not import" }),
    ).toBeVisible({
      timeout: 240_000,
    });
    await expectNothingLeft(page);
  } finally {
    await cdp.send("Storage.overrideQuotaForOrigin", { origin: pageOrigin });
  }
});
