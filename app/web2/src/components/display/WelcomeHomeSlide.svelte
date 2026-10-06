<script lang="ts">
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { pickAndUpload, pickAndLoadCheckpoint } from '@/bus/upload';
  import { initEmulator, whenModuleReady } from '@/bus';
  import { images } from '@/state/images.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { opfs } from '@/bus/opfs';
  import { recent, forgetRecentMachine } from '@/state/recent.svelte';
  import {
    findMissingFiles,
    formatRelativeTime,
    recentKey,
    type RecentMachine,
  } from '@/lib/recentMachines';
  import Icon from '@/components/common/Icon.svelte';
  import ActionRow from '../ui/ActionRow.svelte';
  import Badge from '../ui/Badge.svelte';
  import Card from '../ui/Card.svelte';
  import Hero from '../ui/Hero.svelte';
  import IconButton from '../ui/IconButton.svelte';
  import ListRow from '../ui/ListRow.svelte';

  // The Recent card: machines started in this browser (state/recent.svelte.ts),
  // each the exact initEmulator input -- the machine.boot document with its
  // model id and ROM path, and the media -- so a relaunch is the New Machine
  // boot again, without the dialog.  (An earlier Recent card read
  // /opfs/config/recent.json, which nothing in production wrote, and held
  // display names where machine.boot takes model ids.)
  let rows = $derived(recent.entries.map((e) => ({ key: recentKey(e.config), entry: e })));
  // recentKey -> the first file of that entry that is gone from OPFS.
  let missing = $state<Record<string, string>>({});
  let launching = $state(false);
  // The clock the relative times are read against, ticking once a minute.
  let now = $state(Date.now());

  $effect(() => {
    const t = setInterval(() => (now = Date.now()), 60_000);
    return () => clearInterval(t);
  });

  // Re-check the files whenever the list or the image catalog changes (an
  // upload, a rename, a delete): a missing file disables its row up front
  // instead of failing the boot halfway.
  $effect(() => {
    const entries = recent.entries;
    void images.revision;
    let stale = false;
    void findMissingFiles(entries, opfs.list).then((m) => {
      if (!stale) missing = m;
    });
    return () => {
      stale = true;
    };
  });

  async function launchRecent(entry: RecentMachine) {
    if (launching) return;
    launching = true;
    try {
      await whenModuleReady();
      await initEmulator(entry.config);
    } catch (e) {
      showNotification(`Boot failed: ${e instanceof Error ? e.message : String(e)}`, 'error');
    } finally {
      launching = false;
    }
  }

  function openConfigSlide() {
    setWelcomeSlide('configuration');
  }

  async function openUploadRom() {
    // No auto-boot: the user is on the Welcome screen specifically to
    // configure a machine. Auto-booting from this entry would strand
    // them mid-setup (no VROM, no disks). The Display drag-and-drop
    // path still auto-boots, which is where that shortcut belongs.
    await pickAndUpload('', { autoBootOnRom: false });
  }

  // A saved state from disk (Save State's download, or any checkpoint file):
  // loading it IS starting a machine, so it belongs on this card.  The ones
  // this browser keeps are in the Checkpoints panel.
  async function openCheckpoint() {
    await pickAndLoadCheckpoint();
  }
</script>

<div class="home-content">
  <Hero
    title="Granny Smith"
    subtitle="A classic Macintosh emulator in the browser."
    titleClass="welcome-title"
    subtitleClass="welcome-subtitle"
  />
  <Card class="card" heading="Start">
    <div class="card-rows">
      <ActionRow class="card-row" icon="mac" label="New Machine..." onclick={openConfigSlide} />
      <ActionRow
        class="card-row"
        icon="clock"
        label="Open Checkpoint..."
        onclick={openCheckpoint}
      />
      <ActionRow class="card-row" icon="upload" label="Load ROM..." onclick={openUploadRom} />
    </div>
  </Card>
  {#if rows.length}
    <Card class="card" heading="Recent">
      <div class="card-rows">
        {#each rows as { key, entry } (key)}
          {@const gone = missing[key]}
          <ListRow class="recent-row" hover={!gone}>
            <button
              type="button"
              class="recent-launch"
              disabled={!!gone || launching}
              title={gone ? `${entry.label} (missing: ${gone})` : entry.label}
              onclick={() => launchRecent(entry)}
            >
              <Icon name="mac" class="recent-icon" />
              <span class="recent-label">{entry.label}</span>
              {#if gone}
                <Badge class="recent-missing" intent="warning">missing: {gone}</Badge>
              {:else}
                <span class="recent-time">{formatRelativeTime(entry.lastUsed, now)}</span>
              {/if}
            </button>
            <IconButton
              icon="close"
              size="sm"
              tone="panel"
              rest="faded"
              label="Forget {entry.label}"
              title="Forget"
              onclick={() => forgetRecentMachine(key)}
            />
          </ListRow>
        {/each}
      </div>
    </Card>
  {/if}
</div>

<style>
  .home-content {
    max-width: 560px;
    width: 100%;
    padding: var(--gs-space-12) var(--gs-space-8) var(--gs-space-8);
  }
  .card-rows {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  /* A recent machine: the whole row launches it, the × forgets it. */
  .home-content :global(.recent-row) {
    padding-right: var(--gs-space-1);
    border-radius: var(--gs-action-row-radius);
  }
  .recent-launch {
    display: flex;
    flex: 1 1 auto;
    align-items: center;
    gap: var(--gs-space-2-5);
    min-width: 0;
    height: 100%;
    padding: 0;
    border: none;
    background: transparent;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .recent-launch:disabled {
    cursor: default;
    color: var(--gs-text-disabled);
  }
  .recent-launch :global(.recent-icon) {
    flex: none;
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
  }
  .recent-label {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .recent-time {
    flex: none;
    color: var(--gs-text-muted);
  }
</style>
