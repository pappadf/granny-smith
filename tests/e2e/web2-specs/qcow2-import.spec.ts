// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a QCOW2 disk image given as URL media is converted to a UDIF.
//
// A QCOW2 cannot be written into the UDIF writer as it arrives -- its cluster
// tables say where each guest cluster lives, and the clusters need not be in
// disk order -- so the import stages it as it is and the core converts it
// (bus/importImage.ts).  Asserted: an HD0= value serving a QCOW2 (one far
// smaller than the small-file threshold, so the size-based small path must
// not take it) ends up as one stored .dmg whose decoded bytes are the guest
// disk exactly (the CRC-32 files.udif_info reports, against the disk built
// here), and nothing is left in staging.
//
// The image is built here, laid out as QEMU's qcow2 spec describes: data
// clusters out of disk order, two of them adjacent, one deflated at an
// unaligned offset, and the L2 table after all of them.

import { test, expect, type Page } from "../helpers/test";
import * as http from "node:http";
import type { AddressInfo } from "node:net";
import * as zlib from "node:zlib";
import { gsEvalInPage, scratchFiles } from "../helpers/web2-eval";

const CLUSTER_BITS = 16;
const CLUSTER = 1 << CLUSTER_BITS;
const DISK_BYTES = 64 * 1024 * 1024; // 1024 guest clusters: one L2 table

// Deterministic incompressible bytes.
function noise(seed: number, len: number): Buffer {
  const b = Buffer.alloc(len);
  let x = (seed * 2654435761) >>> 0 || 1;
  for (let k = 0; k < len; k += 4) {
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    b.writeUInt32LE(x >>> 0, k);
  }
  return b;
}

// The guest disk (its non-zero clusters) and the QCOW2 file holding it.
function buildImage(): { disk: Buffer; file: Buffer } {
  const guest = new Map<number, Buffer>();
  const c0 = noise(1, CLUSTER);
  c0.write("ER", 0, "latin1"); // looks like a partitioned disk
  c0.writeUInt16BE(512, 2);
  guest.set(0, c0);
  guest.set(5, noise(5, CLUSTER));
  guest.set(6, noise(6, CLUSTER));
  guest.set(1023, noise(1023, CLUSTER));
  guest.set(100, Buffer.alloc(CLUSTER, "Mac OS X 10.1 Puma "));
  const disk = Buffer.alloc(DISK_BYTES);
  for (const [i, b] of guest) b.copy(disk, i * CLUSTER);

  // Host clusters: 0 header, 1 L1, then data (guest 1023, 5, 6, 0), the
  // deflated guest 100 from 300 bytes into the next cluster, and last the L2.
  const parts: Buffer[] = [];
  const header = Buffer.alloc(CLUSTER);
  const l1 = Buffer.alloc(CLUSTER);
  const l2 = Buffer.alloc(CLUSTER);
  parts.push(header, l1);
  let host = 2 * CLUSTER;
  for (const g of [1023, 5, 6, 0]) {
    parts.push(guest.get(g)!);
    l2.writeBigUInt64BE(BigInt(host) | (1n << 63n), g * 8);
    host += CLUSTER;
  }
  const deflated = zlib.deflateRawSync(guest.get(100)!);
  const lead = 300;
  const off = host + lead;
  const extra = Math.ceil(((off % 512) + deflated.length) / 512) - 1;
  const x = 62n - BigInt(CLUSTER_BITS - 8);
  l2.writeBigUInt64BE((1n << 62n) | (BigInt(extra) << x) | BigInt(off), 100 * 8);
  const compSpan = Math.ceil((lead + deflated.length) / CLUSTER) * CLUSTER;
  const comp = Buffer.alloc(compSpan);
  deflated.copy(comp, lead);
  parts.push(comp);
  host += compSpan;
  const l2At = host;
  parts.push(l2);
  l1.writeBigUInt64BE(BigInt(l2At) | (1n << 63n), 0);

  header.writeUInt32BE(0x514649fb, 0); // "QFI\xfb"
  header.writeUInt32BE(3, 4);
  header.writeUInt32BE(CLUSTER_BITS, 20);
  header.writeBigUInt64BE(BigInt(DISK_BYTES), 24);
  header.writeUInt32BE(1, 36); // l1_size
  header.writeBigUInt64BE(BigInt(CLUSTER), 40); // l1_table_offset
  header.writeUInt32BE(4, 96); // refcount_order
  header.writeUInt32BE(112, 100); // header_length
  return { disk, file: Buffer.concat(parts) };
}

const image = buildImage();

let server: http.Server;
let origin = "";

test.beforeAll(async () => {
  server = http.createServer((req, res) => {
    const headers = {
      "Access-Control-Allow-Origin": "*",
      "Cross-Origin-Resource-Policy": "cross-origin",
      "Content-Type": "application/octet-stream",
    };
    if ((req.url ?? "").startsWith("/qcow2/")) {
      res.writeHead(200, {
        ...headers,
        "Content-Length": String(image.file.length),
      });
      res.end(image.file);
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

async function storedDisks(
  page: Page,
): Promise<Array<{ name: string; size: number }>> {
  const list = (await gsEvalInPage(page, "files.list", [
    "/opfs/images/hd",
  ])) as Array<{ name: string; kind: string; size: number }>;
  return Array.isArray(list) ? list.filter((e) => e.kind !== "directory") : [];
}

test("HD= of a QCOW2 image is stored as a UDIF of its guest disk", async ({
  page,
}) => {
  test.setTimeout(180_000);
  const log: string[] = [];
  page.on("console", (m) => log.push(`[${m.type()}] ${m.text()}`));
  const value = `${origin}/qcow2/MacOSPuma.img`;
  await page.goto(`/index.html?HD0=${encodeURIComponent(value)}`);
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    { timeout: 60_000 },
  );
  const STORED = /^MacOSPuma(?: \d+)?\.dmg$/;
  await expect
    .poll(
      async () => (await storedDisks(page)).filter((d) => STORED.test(d.name)).length,
      {
        timeout: 150_000,
        message: `never stored.\nConsole:\n${log.slice(-40).join("\n")}`,
      },
    )
    .toBe(1);
  const disk = (await storedDisks(page)).find((d) => STORED.test(d.name))!;
  const info = (await gsEvalInPage(page, "files.udif_info", [
    `/opfs/images/hd/${disk.name}`,
  ])) as { sectors: number; crc: number; origin: string };
  expect(info.sectors).toBe(DISK_BYTES / 512);
  expect(info.crc >>> 0).toBe(zlib.crc32(image.disk) >>> 0);
  expect(info.origin).toBe(value);
  // Neither the staged QCOW2 nor the UDIF being written is left behind.
  const staging = await scratchFiles(page);
  expect(staging.filter((n) => n.includes(".part"))).toEqual([]);
});
