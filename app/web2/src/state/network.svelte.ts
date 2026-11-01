// The Network tab's state: what this machine's AppleTalk router reaches
// (read from appletalk.router), and the friend link being set up by hand
// (network/friendLink.ts).
//
// Tabs of this browser link by themselves (the tab mesh, in the emulator's
// worker); this module only shows them.  A friend link is the one thing the
// user sets up: invite (copy an invitation, paste the reply) or accept (open
// or paste an invitation, copy the reply back).

import { gsEval } from '@/bus/emulator';
import { FriendLink } from '@/network/friendLink';

export interface RouterLink {
  index: number;
  state: string; // connecting | open | closed
  transport: string; // tabs | webrtc | socket | loopback
  peerNet: number;
  peerName: string;
  peerZone: string;
  rttMs: number;
  reason: string;
}

export type FriendPhase =
  | 'none' // nothing set up
  | 'inviting' // gathering addresses for an invitation
  | 'invitation' // the invitation is ready; waiting for the reply
  | 'accepting' // gathering addresses for a reply
  | 'reply' // the reply is ready; waiting for the inviter to paste it
  | 'connecting' // both sides done; the connection is forming
  | 'connected'
  | 'failed';

interface NetworkState {
  active: boolean; // the router runs (AppleTalk on, a machine on the cable)
  thisNet: number;
  zone: string;
  links: RouterLink[];
  friend: {
    phase: FriendPhase;
    invitation: string;
    reply: string;
    error: string;
    peerName: string;
  };
  yourName: string;
  // An invitation the page was opened with (#join=...), waiting for a machine
  pendingJoin: string | null;
}

export const network: NetworkState = $state({
  active: false,
  thisNet: 0,
  zone: '',
  links: [],
  friend: { phase: 'none', invitation: '', reply: '', error: '', peerName: '' },
  yourName: '',
  pendingJoin: null,
});

let link: FriendLink | null = null;

function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}
function num(v: unknown): number {
  return typeof v === 'number' ? v : Number(v) || 0;
}

// Read the router's state into `network`.
export async function refreshNetwork(): Promise<void> {
  const net = num(await gsEval('appletalk.router.net'));
  const seed = (await gsEval('appletalk.router.seed')) === true;
  network.thisNet = net;
  network.active = seed;
  network.zone = str(await gsEval('appletalk.router.zone'));
  const links: RouterLink[] = [];
  for (let i = 0; i < 16; i++) {
    const state = await gsEval(`appletalk.router.links[${i}].state`);
    if (typeof state !== 'string') break; // slots fill from the first
    const p = `appletalk.router.links[${i}]`;
    links.push({
      index: i,
      state,
      transport: str(await gsEval(`${p}.transport`)),
      peerNet: num(await gsEval(`${p}.peer_net`)),
      peerName: str(await gsEval(`${p}.peer_name`)),
      peerZone: str(await gsEval(`${p}.peer_zone`)),
      rttMs: num(await gsEval(`${p}.rtt_ms`)),
      reason: str(await gsEval(`${p}.reason`)),
    });
  }
  network.links = links;
}

// The other tabs of this browser this router is linked with.
export function linkedTabs(): RouterLink[] {
  return network.links.filter((l) => l.transport === 'tabs' && l.state === 'open');
}

// The friend links (WebRTC) that are open.
export function linkedFriends(): RouterLink[] {
  return network.links.filter((l) => l.transport === 'webrtc' && l.state === 'open');
}

function follow(l: FriendLink): void {
  l.onchange = () => {
    const f = network.friend;
    f.peerName = l.peerName;
    if (l.state === 'open') f.phase = 'connected';
    else if (l.state === 'connecting' && f.phase !== 'connected') f.phase = 'connecting';
    else if (l.state === 'failed') {
      f.phase = 'failed';
      f.error = l.error || 'The connection failed.';
    } else if (l.state === 'closed' && f.phase === 'connected') {
      f.phase = 'none';
    }
    void refreshNetwork();
  };
}

function nameForSignal(): string {
  return network.yourName.trim() || 'A friend';
}

async function useName(): Promise<void> {
  const n = network.yourName.trim();
  if (n) await gsEval('appletalk.router.name', [n]);
}

// Invite: gather addresses, then show the invitation to copy.
export async function startInvitation(): Promise<void> {
  link?.close();
  resetFriend();
  network.friend.phase = 'inviting';
  await useName();
  link = new FriendLink('offer');
  follow(link);
  try {
    const blob = await link.invite(nameForSignal());
    network.friend.invitation = `${location.origin}${location.pathname}#join=${blob}`;
    network.friend.phase = 'invitation';
  } catch (e) {
    network.friend.phase = 'failed';
    network.friend.error = e instanceof Error ? e.message : String(e);
  }
}

// The inviter pastes the friend's reply.
export async function completeWithReply(reply: string): Promise<void> {
  if (!link || link.role !== 'offer') return;
  network.friend.error = '';
  try {
    await link.complete(reply);
  } catch (e) {
    // A bad paste: say why, keep the invitation open for the right one
    network.friend.error = e instanceof Error ? e.message : String(e);
  }
}

// Accept an invitation (a link or the bare text): gather, then show the
// reply to send back.
export async function acceptInvitation(invitation: string): Promise<void> {
  link?.close();
  resetFriend();
  network.friend.phase = 'accepting';
  await useName();
  link = new FriendLink('answer');
  follow(link);
  try {
    network.friend.reply = await link.accept(invitation, nameForSignal());
    network.friend.peerName = link.peerName;
    network.friend.phase = 'reply';
  } catch (e) {
    network.friend.phase = 'failed';
    network.friend.error = e instanceof Error ? e.message : String(e);
  }
}

export function disconnectFriend(): void {
  link?.close();
  link = null;
  resetFriend();
}

export function resetFriend(): void {
  network.friend.phase = 'none';
  network.friend.invitation = '';
  network.friend.reply = '';
  network.friend.error = '';
  network.friend.peerName = '';
}

// The page was opened from an invitation link (#join=...): keep it for the
// Network tab and clear it from the address bar, so a reload does not
// accept it again.
export function takeJoinFromUrl(): void {
  const h = location.hash;
  if (!h.startsWith('#join=')) return;
  network.pendingJoin = h;
  history.replaceState(null, '', location.pathname + location.search);
}
