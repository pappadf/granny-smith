// A link to a friend's Granny Smith over WebRTC, set up by hand: one
// invitation and one reply, swapped by whatever chat or mail the two people
// already use.  No signalling server, no account.
//
// The emulator's AppleTalk router (src/core/network/appletalk_router.c) runs
// in the emulator's worker; a WebRTC data channel can only live here, on the
// page's main thread.  So each friend link opens a private BroadcastChannel
// and asks the core to link through it (appletalk.router.link_channel); this
// module carries the router's records between that channel and the data
// channels (src/platform/wasm/em_atalk_tabs.c has the worker's end):
//
//   worker -> page   {t: "rec", d, r}   a record; r: wants the reliable channel
//                    {t: "closed"}      the router is done with the link
//   page -> worker   {t: "rec", d}      a record from the friend
//                    {t: "lost"}        the peer connection went away
//
// Two data channels: "ctl", reliable and ordered, for the link's control
// records (HELLO, PING, BYE); "ddp", unordered with no retransmits, for
// datagrams -- AppleTalk's own protocols retransmit, and a reliable channel
// would hold every packet behind a lost one.

import { gsEval } from '@/bus/emulator';

export type FriendRole = 'offer' | 'answer';

// What an invitation or reply carries, before packing.
export interface Signal {
  v: 1;
  type: FriendRole;
  sdp: string;
  name: string; // the sender's display name
}

// Public STUN, to learn each side's public address; there is no TURN relay.
export const DEFAULT_ICE: RTCIceServer[] = [{ urls: 'stun:stun.l.google.com:19302' }];

const GATHER_TIMEOUT_MS = 5000;

// --- packing: JSON, deflate-raw, base64url ------------------------------------

function toBase64Url(bytes: Uint8Array): string {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function fromBase64Url(text: string): Uint8Array<ArrayBuffer> {
  const b64 = text.replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(b64 + '='.repeat((4 - (b64.length % 4)) % 4));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function streamThrough(
  data: Uint8Array<ArrayBuffer>,
  t: CompressionStream | DecompressionStream,
): Promise<Uint8Array<ArrayBuffer>> {
  const stream = new Blob([data]).stream().pipeThrough(t);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function packSignal(s: Signal): Promise<string> {
  const json = new TextEncoder().encode(JSON.stringify(s));
  return toBase64Url(await streamThrough(json, new CompressionStream('deflate-raw')));
}

// The invitation or reply in `text`: the bare blob, or a link with it in the
// fragment (#join=...).  Throws with a message for the user if it is not one.
export async function unpackSignal(text: string, want: FriendRole): Promise<Signal> {
  let blob = text.trim();
  const at = blob.indexOf('#join=');
  if (at >= 0) blob = blob.slice(at + 6);
  let s: Signal;
  try {
    const json = await streamThrough(fromBase64Url(blob), new DecompressionStream('deflate-raw'));
    s = JSON.parse(new TextDecoder().decode(json)) as Signal;
  } catch {
    throw new Error(want === 'offer' ? 'That is not an invitation.' : 'That is not a reply.');
  }
  if (s.v !== 1 || typeof s.sdp !== 'string')
    throw new Error('It comes from another version of Granny Smith.');
  if (s.type !== want)
    throw new Error(
      want === 'offer'
        ? 'That is a reply, not an invitation.'
        : 'That is an invitation, not a reply.',
    );
  return s;
}

// --- the link ------------------------------------------------------------------

export type FriendState = 'gathering' | 'waiting' | 'connecting' | 'open' | 'closed' | 'failed';

export class FriendLink {
  readonly role: FriendRole;
  peerName = '';
  state: FriendState = 'gathering';
  error = '';
  onchange: (() => void) | null = null;

  private pc: RTCPeerConnection;
  private ctl: RTCDataChannel | null = null;
  private ddp: RTCDataChannel | null = null;
  private bc: BroadcastChannel | null = null;
  private started = false;

  constructor(role: FriendRole, ice: RTCIceServer[] = DEFAULT_ICE) {
    this.role = role;
    this.pc = new RTCPeerConnection({ iceServers: ice });
    this.pc.onconnectionstatechange = () => {
      const s = this.pc.connectionState;
      if (s === 'failed') this.fail('The two networks could not reach each other.');
      else if (s === 'closed' || s === 'disconnected') this.lost();
    };
    if (role === 'offer') {
      this.adopt(this.pc.createDataChannel('ctl', { ordered: true }));
      this.adopt(this.pc.createDataChannel('ddp', { ordered: false, maxRetransmits: 0 }));
    } else {
      this.pc.ondatachannel = (ev) => this.adopt(ev.channel);
    }
  }

  // The invitation (offerer): the full offer once ICE gathering is done
  // (non-trickle: one message carries every candidate).
  async invite(name: string): Promise<string> {
    await this.pc.setLocalDescription(await this.pc.createOffer());
    await this.gathered();
    this.set('waiting');
    return packSignal({ v: 1, type: 'offer', sdp: this.pc.localDescription!.sdp, name });
  }

  // The reply (answerer) to `invitation`.
  async accept(invitation: string, name: string): Promise<string> {
    const offer = await unpackSignal(invitation, 'offer');
    this.peerName = offer.name;
    await this.pc.setRemoteDescription({ type: 'offer', sdp: offer.sdp });
    await this.pc.setLocalDescription(await this.pc.createAnswer());
    await this.gathered();
    this.set('waiting');
    return packSignal({ v: 1, type: 'answer', sdp: this.pc.localDescription!.sdp, name });
  }

  // The offerer pastes the reply; the connection then forms by itself.
  async complete(reply: string): Promise<void> {
    const answer = await unpackSignal(reply, 'answer');
    this.peerName = answer.name;
    this.set('connecting');
    await this.pc.setRemoteDescription({ type: 'answer', sdp: answer.sdp });
  }

  close(): void {
    this.bc?.postMessage({ t: 'lost' });
    this.teardown('closed');
  }

  private set(s: FriendState): void {
    this.state = s;
    this.onchange?.();
  }

  private async gathered(): Promise<void> {
    if (this.pc.iceGatheringState === 'complete') return;
    await new Promise<void>((resolve) => {
      const done = () => {
        if (this.pc.iceGatheringState === 'complete') resolve();
      };
      this.pc.addEventListener('icegatheringstatechange', done);
      setTimeout(resolve, GATHER_TIMEOUT_MS); // what has been gathered by now will do
    });
  }

  private adopt(dc: RTCDataChannel): void {
    dc.binaryType = 'arraybuffer';
    if (dc.label === 'ctl') this.ctl = dc;
    else if (dc.label === 'ddp') this.ddp = dc;
    dc.onopen = () => void this.maybeStart();
    dc.onclose = () => this.lost();
    dc.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer)
        this.bc?.postMessage({ t: 'rec', d: new Uint8Array(ev.data) });
    };
  }

  // Both channels open: hand the link to the router.
  private async maybeStart(): Promise<void> {
    if (this.started || this.ctl?.readyState !== 'open' || this.ddp?.readyState !== 'open') return;
    this.started = true;
    const name = `gs-atalk-friend-${crypto.randomUUID()}`;
    this.bc = new BroadcastChannel(name);
    this.bc.onmessage = (
      ev: MessageEvent<{ t: string; d?: Uint8Array<ArrayBuffer>; r?: boolean }>,
    ) => {
      const m = ev.data;
      if (m.t === 'rec' && m.d) {
        const ch = m.r ? this.ctl : this.ddp;
        if (ch?.readyState === 'open') ch.send(m.d);
      } else if (m.t === 'closed') {
        this.teardown('closed');
      }
    };
    const r = await gsEval('appletalk.router.link_channel', [name, true]);
    if (r && typeof r === 'object' && 'error' in r) {
      this.fail(String((r as { error: unknown }).error));
      return;
    }
    this.set('open');
  }

  private lost(): void {
    if (this.state === 'closed' || this.state === 'failed') return;
    this.bc?.postMessage({ t: 'lost' });
    this.teardown(this.state === 'open' ? 'closed' : 'failed');
  }

  private fail(why: string): void {
    this.error = why;
    this.bc?.postMessage({ t: 'lost' });
    this.teardown('failed');
  }

  private teardown(s: FriendState): void {
    try {
      this.pc.close();
    } catch {
      /* already closed */
    }
    this.bc?.close();
    this.bc = null;
    this.set(s);
  }
}
