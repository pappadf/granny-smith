// The Network tab's state helpers: tabs the browser suspended, the relay
// (TURN) the user may add, and what a friend's round trip means.

import { beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('@/bus/emulator', () => ({ gsEval: vi.fn(async () => null) }));

import {
  network,
  suspendedTabs,
  iceServers,
  turnProblem,
  latencyNote,
  type RouterLink,
} from '@/state/network.svelte';
import { DEFAULT_ICE } from '@/network/friendLink';

const SUSPENDED = 'the other tab was suspended by the browser';

function link(index: number, state: string, peerNet: number, reason = ''): RouterLink {
  return {
    index,
    state,
    transport: 'tabs',
    peerNet,
    peerName: '',
    peerZone: '',
    rttMs: -1,
    reason,
  };
}

describe('suspendedTabs', () => {
  it('lists a tab link the browser suspended, once per network', () => {
    network.links = [
      link(0, 'closed', 7, SUSPENDED),
      link(1, 'closed', 7, SUSPENDED),
      link(2, 'open', 9),
    ];
    expect(suspendedTabs().map((l) => l.peerNet)).toEqual([7]);
  });
  it('drops it once the tab has linked again', () => {
    network.links = [link(0, 'closed', 7, SUSPENDED), link(1, 'open', 7)];
    expect(suspendedTabs()).toEqual([]);
  });
  it('ignores tabs that left or went silent', () => {
    network.links = [link(0, 'closed', 7, 'the other tab left')];
    expect(suspendedTabs()).toEqual([]);
  });
});

describe('the relay', () => {
  beforeEach(() => {
    network.turn = { url: '', username: '', credential: '' };
  });
  it('is STUN alone when none is set', () => {
    expect(iceServers()).toEqual(DEFAULT_ICE);
  });
  it('adds the relay with its credential', () => {
    network.turn = { url: 'turn:relay.example.net:3478', username: 'u', credential: 'p' };
    expect(iceServers()).toEqual([
      ...DEFAULT_ICE,
      { urls: 'turn:relay.example.net:3478', username: 'u', credential: 'p' },
    ]);
  });
  it('refuses an address that is not a relay, and leaves it out', () => {
    network.turn = { url: 'https://relay.example.net', username: '', credential: '' };
    expect(turnProblem(network.turn)).not.toBe('');
    expect(iceServers()).toEqual(DEFAULT_ICE);
  });
});

describe('latencyNote', () => {
  it('says nothing for a short or unknown round trip', () => {
    expect(latencyNote(-1)).toBe('');
    expect(latencyNote(80)).toBe('');
  });
  it('warns as games start to feel it, and past what they bear', () => {
    expect(latencyNote(150)).toMatch(/games/);
    expect(latencyNote(300)).toMatch(/file sharing works/);
  });
});
