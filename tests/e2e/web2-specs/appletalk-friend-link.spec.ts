// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: linking to a friend over WebRTC, set up by hand in the Network
// tab (app/web2/src/network/friendLink.ts).
//
// Two browser contexts are two people: no shared storage, no BroadcastChannel
// between them.  Each boots a Plus.  A invites (the invitation link), B
// pastes it and accepts (the reply), A pastes the reply and connects: the
// WebRTC data channels open, each page hands its end to the router
// (appletalk.router.link_channel), and each router learns the other's
// network one hop away.  Inside one machine the peer connection needs no
// STUN: host candidates reach each other.

import { test, expect, type Page } from "../helpers/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";
import { gsCallInPage, gsEvalInPage } from "../helpers/web2-eval";

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms", "plus-v3-4d1f8172.rom");
const ROM_OPFS = "/opfs/images/rom/4d1f8172";

async function bootPlus(page: Page): Promise<void> {
  await gotoWeb2(page);
  await stageOpfsFile(page, ROM_OPFS, ROM);
  await gsCallInPage(page, "machine.boot", { model: "plus", rom: ROM_OPFS });
  await expect.poll(async () => gsEvalInPage(page, "machine.id"), { timeout: 30_000 }).toBe("plus");
  await gsCallInPage(page, "scheduler.run", []);
}

async function openNetworkTab(page: Page): Promise<void> {
  await page.getByRole("tab", { name: /network/i }).click();
}

// How many of the page's router links are open WebRTC links.
async function friendLinks(page: Page): Promise<number> {
  let n = 0;
  for (let i = 0; i < 16; i++) {
    const s = await gsEvalInPage(page, `appletalk.router.links[${i}].state`);
    if (typeof s !== "string") break;
    if (s === "open" && (await gsEvalInPage(page, `appletalk.router.links[${i}].transport`)) === "webrtc") n++;
  }
  return n;
}

async function distanceTo(page: Page, net: number): Promise<number> {
  const st = await gsEvalInPage(page, `appletalk.router.routes["${net}"].state`);
  if (st !== "good" && st !== "suspect") return -1;
  return Number(await gsEvalInPage(page, `appletalk.router.routes["${net}"].distance`));
}

test("two people link their Macs with an invitation and a reply", async ({ browser }) => {
  test.setTimeout(4 * 60 * 1000);
  const ctxA = await browser.newContext();
  const ctxB = await browser.newContext();
  const a = await ctxA.newPage();
  const b = await ctxB.newPage();
  await bootPlus(a);
  await bootPlus(b);

  // A invites
  await openNetworkTab(a);
  await a.getByRole("textbox", { name: "Your name" }).fill("Alice");
  await a.getByRole("button", { name: "Invite a friend" }).click();
  const invitationBox = a.getByRole("textbox", { name: "Invitation", exact: true });
  await expect(invitationBox).toHaveValue(/#join=/, { timeout: 30_000 });
  const invitation = await invitationBox.inputValue();

  // B accepts and gets a reply
  await openNetworkTab(b);
  await b.getByRole("textbox", { name: "Your name" }).fill("Bob");
  await b.getByRole("textbox", { name: "Invitation to accept" }).fill(invitation);
  await b.getByRole("button", { name: "Accept the invitation" }).click();
  const replyBox = b.getByRole("textbox", { name: "Reply to send" });
  await expect(replyBox).not.toHaveValue("", { timeout: 30_000 });
  await expect(b.getByText("Send this reply to Alice")).toBeVisible();
  const reply = await replyBox.inputValue();

  // A pastes the reply and connects
  await a.getByRole("textbox", { name: "Reply", exact: true }).fill(reply);
  await a.getByRole("button", { name: "Connect" }).click();

  for (const p of [a, b]) await expect.poll(() => friendLinks(p), { timeout: 60_000 }).toBe(1);
  const netA = Number(await gsEvalInPage(a, "appletalk.router.net"));
  const netB = Number(await gsEvalInPage(b, "appletalk.router.net"));
  await expect.poll(() => distanceTo(a, netB), { timeout: 60_000 }).toBe(1);
  await expect.poll(() => distanceTo(b, netA), { timeout: 60_000 }).toBe(1);
  await expect(a.getByTestId("network-friend-peer")).toContainText("Bob", { timeout: 30_000 });

  // Disconnect: both sides see the link go
  await a.getByRole("button", { name: "Disconnect" }).click();
  for (const p of [a, b]) await expect.poll(() => friendLinks(p), { timeout: 60_000 }).toBe(0);
  await ctxA.close();
  await ctxB.close();
});
