<script lang="ts">
  import { onMount } from 'svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { initEmulator, opfs } from '@/bus';
  import { pickAndUpload } from '@/bus/upload';
  import type { RecentEntry } from '@/bus/types';
  import Icon from '../common/Icon.svelte';

  // Recent list — loaded from OPFS. MockOpfs (tests) returns prototype
  // fixtures; BrowserOpfs (production) reads /opfs/config/recent.json
  // once it exists.
  let recents = $state<RecentEntry[]>([]);
  onMount(async () => {
    const loaded = await opfs.readJson<RecentEntry[]>('/opfs/config/recent.json');
    if (loaded) recents = loaded;
  });

  function openConfigSlide() {
    setWelcomeSlide('configuration');
  }

  async function openUploadRom() {
    await pickAndUpload();
  }

  function openCheckpointPicker() {
    showNotification('Open Checkpoint... arrives in Phase 5 (Checkpoints view)', 'warning');
  }

  async function launchRecent(r: RecentEntry) {
    await initEmulator({
      model: r.model,
      ram: r.ram,
      vrom: '(auto)',
      floppies: [],
      hd: '',
      cd: '',
    });
  }
</script>

<div class="home-content">
  <h1 class="welcome-title">Granny Smith</h1>
  <p class="welcome-subtitle">A classic Macintosh emulator in the browser.</p>
  <section class="card">
    <h3 class="card-heading">Start</h3>
    <div class="card-rows">
      <button class="card-row" onclick={openConfigSlide}>
        <Icon name="mac" />
        <span>New Machine...</span>
      </button>
      <button class="card-row" onclick={openCheckpointPicker}>
        <Icon name="clock" />
        <span>Open Checkpoint...</span>
      </button>
      <button class="card-row" onclick={openUploadRom}>
        <Icon name="upload" />
        <span>Upload ROM...</span>
      </button>
    </div>
  </section>
  {#if recents.length > 0}
    <section class="card">
      <h3 class="card-heading">Recent</h3>
      <div class="card-rows">
        {#each recents as r (r.model + r.media + r.lastUsedAt)}
          <button class="card-row recent" onclick={() => launchRecent(r)}>
            <Icon name="mac" />
            <span class="recent-text">
              <span class="recent-main">{r.model} — {r.ram}</span>
              <span class="recent-sub">{r.media}</span>
            </span>
          </button>
        {/each}
      </div>
    </section>
  {/if}
</div>

<style>
  .home-content {
    max-width: 560px;
    width: 100%;
    padding: 48px 32px 32px;
  }
  .welcome-title {
    font-size: 28px;
    font-weight: 200;
    color: var(--gs-fg-bright);
    margin: 0 0 8px 0;
  }
  .welcome-subtitle {
    color: var(--gs-fg);
    opacity: 0.7;
    margin: 0 0 28px 0;
    font-size: 14px;
  }
  .card {
    background: var(--gs-card-bg);
    border: 1px solid var(--gs-card-border);
    border-radius: 6px;
    padding: 14px 16px;
    margin-bottom: 16px;
  }
  .card-heading {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--gs-fg);
    opacity: 0.8;
    margin: 0 0 8px 0;
  }
  .card-rows {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .card-row {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 6px 8px;
    background: transparent;
    border: none;
    border-radius: 3px;
    color: var(--gs-link);
    cursor: pointer;
    text-align: left;
    font-size: 13px;
  }
  .card-row:hover {
    background: var(--gs-list-hover);
  }
  .card-row :global(.icon) {
    width: 16px;
    height: 16px;
    color: var(--gs-fg);
  }
  .recent-text {
    display: flex;
    flex-direction: column;
  }
  .recent-main {
    color: var(--gs-fg-bright);
  }
  .recent-sub {
    color: var(--gs-fg);
    opacity: 0.6;
    font-size: 12px;
    margin-top: 2px;
  }
</style>
