<script lang="ts">
  // The Network tab: this Mac's AppleTalk network and what it is linked to.
  //
  //   This computer  read only: this network, and the other tabs of this
  //                  browser, which link by themselves (the tab mesh).
  //   Friends        the one thing set up by hand: invite a friend (copy an
  //                  invitation, paste the reply) or accept an invitation
  //                  (open or paste it, copy the reply back).
  import { onMount } from 'svelte';
  import { machine } from '@/state/machine.svelte';
  import {
    network,
    refreshNetwork,
    linkedTabs,
    suspendedTabs,
    linkedFriends,
    startInvitation,
    completeWithReply,
    acceptInvitation,
    disconnectFriend,
    resetFriend,
    latencyNote,
    saveTurn,
    turnProblem,
  } from '@/state/network.svelte';
  import Button from '@/components/ui/Button.svelte';
  import Badge from '@/components/ui/Badge.svelte';
  import Callout from '@/components/ui/Callout.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import SectionHeading from '@/components/ui/SectionHeading.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';
  import Spinner from '@/components/ui/Spinner.svelte';

  const running = $derived(machine.status === 'running' || machine.status === 'paused');

  let replyText = $state('');
  let inviteText = $state('');
  let showRelay = $state(!!network.turn.url);
  const relayProblem = $derived(turnProblem(network.turn));

  onMount(() => {
    void refreshNetwork();
    const t = setInterval(() => void refreshNetwork(), 2000);
    return () => clearInterval(t);
  });

  // An invitation the page was opened with: accept it once a Mac runs.
  $effect(() => {
    if (running && network.pendingJoin && network.friend.phase === 'none') {
      const inv = network.pendingJoin;
      network.pendingJoin = null;
      void acceptInvitation(inv);
    }
  });

  async function copy(text: string): Promise<void> {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      // No clipboard (an insecure context, a denied permission): the text
      // is in the field to select by hand.
    }
  }

  function rtt(ms: number): string {
    return ms >= 0 ? `${ms} ms` : '…';
  }
</script>

{#snippet relay()}
  <div class="actions">
    <Button variant="ghost" size="sm" onclick={() => (showRelay = !showRelay)}
      >{showRelay ? 'Hide relay server' : 'Relay server (TURN)…'}</Button
    >
  </div>
  {#if showRelay}
    <div class="relay" data-testid="network-relay">
      <label class="field">
        <span>Relay address</span>
        <TextInput
          size="sm"
          placeholder="turn:relay.example.net:3478"
          bind:value={network.turn.url}
          onchange={saveTurn}
          aria-label="Relay address"
        />
      </label>
      <label class="field">
        <span>User name</span>
        <TextInput
          size="sm"
          bind:value={network.turn.username}
          onchange={saveTurn}
          aria-label="Relay user name"
        />
      </label>
      <label class="field">
        <span>Password</span>
        <TextInput
          size="sm"
          type="password"
          bind:value={network.turn.credential}
          onchange={saveTurn}
          aria-label="Relay password"
        />
      </label>
      {#if relayProblem}<Callout intent="warning">{relayProblem}</Callout>{/if}
      <Hint
        >Used for the next invitation or reply, when a direct connection fails. Granny Smith runs no
        relay: use a service's free tier or your own. The password stays in this browser.</Hint
      >
    </div>
  {/if}
{/snippet}

<div class="gs-network">
  <section aria-label="This computer">
    <SectionHeading>This computer</SectionHeading>
    {#if !running}
      <Hint>Start a Mac with AppleTalk on to put it on a network.</Hint>
    {:else if !network.active}
      <Hint>This Mac's router is off (appletalk.router.seed): it is not linked to anything.</Hint>
    {:else}
      <div class="row">
        <span class="strong">This Mac</span>
        <span class="muted">network {network.thisNet} · zone {network.zone}</span>
      </div>
      {#each linkedTabs() as l (l.index)}
        <div class="peer" data-testid="network-tab-peer">
          <span>Other tab · network {l.peerNet}</span>
          <Badge intent="success">Linked</Badge>
        </div>
      {/each}
      {#each suspendedTabs() as l (l.index)}
        <div class="peer" data-testid="network-tab-suspended">
          <span>Other tab · network {l.peerNet}</span>
          <Badge intent="warning">Suspended by the browser</Badge>
        </div>
      {/each}
      <Hint>Tabs in this browser with AppleTalk on are linked automatically.</Hint>
    {/if}
  </section>

  <section aria-label="Friends">
    <SectionHeading rule>Friends</SectionHeading>
    {#each linkedFriends() as l (l.index)}
      <div class="peer" data-testid="network-friend-peer">
        <span>{l.peerName || 'Friend'} · network {l.peerNet}</span>
        <span class="muted" title={latencyNote(l.rttMs)}>{rtt(l.rttMs)}</span>
        <Badge intent="success">Connected</Badge>
      </div>
      {#if latencyNote(l.rttMs)}
        <Hint
          >{latencyNote(l.rttMs)}: a message takes {l.rttMs} ms to {l.peerName || 'your friend'} and back.
          In Marathon, choose Net Speed "AppleTalk Remote Access".</Hint
        >
      {/if}
    {/each}

    {#if !running}
      <Hint>Start a Mac first: a friend links to its network.</Hint>
      {#if network.pendingJoin}
        <Callout intent="info">You have an invitation. Start any Mac and it opens here.</Callout>
      {/if}
    {:else if network.friend.phase === 'none'}
      <label class="field">
        <span>Your name</span>
        <TextInput size="sm" bind:value={network.yourName} aria-label="Your name" />
      </label>
      <div class="actions">
        <Button onclick={() => void startInvitation()}>Invite a friend</Button>
      </div>
      <label class="field">
        <span>Or paste an invitation</span>
        <textarea rows="2" bind:value={inviteText} aria-label="Invitation to accept"></textarea>
      </label>
      <div class="actions">
        <Button
          variant="secondary"
          disabled={!inviteText.trim()}
          onclick={() => void acceptInvitation(inviteText)}
        >
          Accept the invitation
        </Button>
      </div>
      <Hint
        >You swap one invitation and one reply by chat or email. Your friend will see the Macs in
        all your tabs, and you theirs.</Hint
      >
      {@render relay()}
    {:else if network.friend.phase === 'inviting' || network.friend.phase === 'accepting'}
      <div class="row"><Spinner /> <span>Gathering network addresses…</span></div>
    {:else if network.friend.phase === 'invitation'}
      <span class="strong">1. Send this invitation</span>
      <textarea rows="3" readonly value={network.friend.invitation} aria-label="Invitation"
      ></textarea>
      <div class="actions">
        <Button onclick={() => void copy(network.friend.invitation)}>Copy invitation</Button>
      </div>
      <label class="field">
        <span class="strong">2. Paste your friend's reply</span>
        <textarea rows="2" bind:value={replyText} aria-label="Reply"></textarea>
      </label>
      {#if network.friend.error}<Callout intent="warning">{network.friend.error}</Callout>{/if}
      <div class="actions">
        <Button disabled={!replyText.trim()} onclick={() => void completeWithReply(replyText)}
          >Connect</Button
        >
        <Button variant="ghost" onclick={disconnectFriend}>Cancel</Button>
      </div>
      <Callout intent="warning">Keep this tab open: the connection is made here.</Callout>
    {:else if network.friend.phase === 'reply'}
      <span class="strong">Send this reply to {network.friend.peerName || 'your friend'}</span>
      <textarea rows="3" readonly value={network.friend.reply} aria-label="Reply to send"
      ></textarea>
      <div class="actions">
        <Button onclick={() => void copy(network.friend.reply)}>Copy reply</Button>
        <Button variant="ghost" onclick={disconnectFriend}>Cancel</Button>
      </div>
      <div class="row"><Spinner /> <span>Waiting for the connection…</span></div>
    {:else if network.friend.phase === 'connecting'}
      <div class="row"><Spinner /> <span>Connecting…</span></div>
    {:else if network.friend.phase === 'connected'}
      <Hint>Your friend's Mac appears in the Chooser, here and in your other tabs.</Hint>
      <div class="actions">
        <Button variant="secondary" onclick={disconnectFriend}>Disconnect</Button>
      </div>
    {:else if network.friend.phase === 'failed'}
      <Callout intent="danger">{network.friend.error || 'The connection failed.'}</Callout>
      {#if !network.turn.url}
        <Hint
          >Some networks block direct connections. A relay server of your own (TURN) gets through
          them: set one below, then start again.</Hint
        >
      {/if}
      <div class="actions">
        <Button variant="secondary" onclick={resetFriend}>Start again</Button>
      </div>
      {@render relay()}
    {/if}
  </section>
</div>

<style>
  .gs-network {
    height: 100%;
    overflow: auto;
    padding: var(--gs-space-3);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-4);
    box-sizing: border-box;
  }
  section {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
  }
  .row,
  .peer {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2);
  }
  .peer > :first-child {
    flex: 1 1 auto;
  }
  .strong {
    color: var(--gs-text-strong);
  }
  .muted {
    color: var(--gs-text-muted);
  }
  .relay {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
  }
  .actions {
    display: flex;
    gap: var(--gs-space-2);
    flex-wrap: wrap;
  }
  textarea {
    width: 100%;
    box-sizing: border-box;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
    resize: vertical;
  }
</style>
