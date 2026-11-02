// The Network tab's state: what this machine's AppleTalk router reaches
// (read from appletalk.router), and the friend link being set up by hand
// (network/friendLink.ts).
//
// Tabs of this browser link by themselves (the tab mesh, in the emulator's
// worker); this module only shows them.  A friend link is the one thing the
// user sets up: invite (copy an invitation, paste the reply) or accept (open
// or paste an invitation, copy the reply back).

import { gsEval } from '@/bus/emulator';
import { DEFAULT_ICE, FriendLink } from '@/network/friendLink';

export interface TurnServer {
  url: string; // turn:host:port or turns:host:port
  username: string;
  credential: string;
}

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
  linked: boolean; // a link to another instance is open (appletalk.router.linked)
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
  // A relay of the user's own (TURN), for networks that block direct
  // connections; kept in this browser's local storage
  turn: TurnServer;
  // An invitation the page was opened with (#join=...), waiting for a machine
  pendingJoin: string | null;
}

// Where the relay (below) is kept
const TURN_KEY = 'gs-atalk-turn';

export const network: NetworkState = $state({
  linked: false,
  active: false,
  thisNet: 0,
  zone: '',
  links: [],
  friend: { phase: 'none', invitation: '', reply: '', error: '', peerName: '' },
  yourName: '',
  turn: loadTurn(),
  pendingJoin: null,
});

let link: FriendLink | null = null;

// --- the relay (TURN) --------------------------------------------------------
// Granny Smith runs no relay; a user whose network blocks direct connections
// brings their own (a managed service's free tier, or their own coturn).
// The credential stays in this browser's local storage, nowhere else.

function loadTurn(): TurnServer {
  const none = { url: '', username: '', credential: '' };
  try {
    const t = JSON.parse(localStorage.getItem(TURN_KEY) ?? 'null') as Partial<TurnServer> | null;
    if (!t || typeof t !== 'object') return none;
    return { url: str(t.url), username: str(t.username), credential: str(t.credential) };
  } catch {
    return none;
  }
}

// Why the relay address is not usable, or '' if it is (or none is set).
export function turnProblem(t: TurnServer): string {
  const url = t.url.trim();
  if (!url) return '';
  if (!/^turns?:[^\s]+$/.test(url)) return 'A relay address starts with turn: or turns:';
  return '';
}

export function saveTurn(): void {
  try {
    const t = network.turn;
    if (t.url.trim()) localStorage.setItem(TURN_KEY, JSON.stringify(t));
    else localStorage.removeItem(TURN_KEY);
  } catch {
    // No local storage: the relay holds for this page only
  }
}

// STUN, plus the user's relay when one is set.
export function iceServers(): RTCIceServer[] {
  const t = network.turn;
  const url = t.url.trim();
  if (!url || turnProblem(t)) return DEFAULT_ICE;
  return [...DEFAULT_ICE, { urls: url, username: t.username, credential: t.credential }];
}

// What a round trip of `ms` means for the Macs: file sharing and the Chooser
// work at any delay; Marathon's ring of players turns the round trip into
// input lag (each tick goes around every player).
export function latencyNote(ms: number): string {
  if (ms < 0 || ms <= 100) return '';
  if (ms <= 200) return 'Network games feel the delay';
  return 'Too slow for action games; file sharing works';
}

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
  link = new FriendLink('offer', iceServers());
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
  link = new FriendLink('answer', iceServers());
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

// Follow appletalk.router.linked, whatever tab the panel shows: Max speed
// is not offered while this Mac is linked to another (its clock would run
// ahead of the other's, and AppleTalk's timers with it).
export function watchLinks(): void {
  void keepNetworkNumber();
  setInterval(async () => {
    network.linked = (await gsEval('appletalk.router.linked')) === true;
    if (network.linked) linkedNet = num(await gsEval('appletalk.router.net'));
  }, 2000);
  document.addEventListener('freeze', sayFrozen);
}

// The browser is freezing this tab (Page Lifecycle API: a long-hidden tab
// under Energy Saver, say).  Nothing runs once it is frozen, the emulator's
// worker included, so tell the other tabs now: they drop the link to this
// one saying why ("suspended by the browser"), instead of noticing seven
// seconds of silence.  On resume this tab's hellos bring the links back.
// The handler must not wait for anything, so the network number is the one
// the last poll saw.
let linkedNet = 0;
function sayFrozen(): void {
  if (!network.linked || !linkedNet || typeof BroadcastChannel === 'undefined') return;
  const ch = new BroadcastChannel('gs-atalk');
  ch.postMessage({ t: 'suspend', net: linkedNet });
  ch.close();
}

// The core's reason for a tab link the browser suspended (em_atalk_tabs.c).
const TAB_SUSPENDED = 'the other tab was suspended by the browser';

// Other tabs the browser has suspended: their links closed for that reason,
// and no open link to the same network has replaced them yet.
export function suspendedTabs(): RouterLink[] {
  const shown: number[] = linkedTabs().map((l) => l.peerNet);
  return network.links.filter((l) => {
    if (l.transport !== 'tabs' || l.state !== 'closed' || l.reason !== TAB_SUSPENDED) return false;
    if (shown.includes(l.peerNet)) return false;
    shown.push(l.peerNet);
    return true;
  });
}

// This tab's network number survives a reload (sessionStorage is per tab),
// so a guest that remembered its network finds the same one: the core picked
// a random number at startup; an earlier one of this tab's takes its place
// before any machine runs.
const NET_KEY = 'gs-atalk-net';
async function keepNetworkNumber(): Promise<void> {
  try {
    const kept = Number(sessionStorage.getItem(NET_KEY));
    if (kept >= 1 && kept <= 0xfeff) {
      await gsEval('appletalk.router.net', [kept]);
    } else {
      const now = num(await gsEval('appletalk.router.net'));
      if (now) sessionStorage.setItem(NET_KEY, String(now));
    }
  } catch {
    // No session storage: a new number each load, as before
  }
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
