<script lang="ts">
  import { showNotification } from '@/state/toasts.svelte';
  import Icon from '../common/Icon.svelte';

  // Mock Recent entries. In Phase 2 these come from OPFS
  // (/opfs/config/recent.json) and the Recent card is hidden when empty.
  const recents = [
    { model: 'Macintosh SE/30', ram: '8 MB', media: 'HD0: System 7.1, FD0: Install Disk' },
    { model: 'Macintosh Plus', ram: '4 MB', media: 'HD0: hd1.img' },
  ];

  function notImplemented() {
    showNotification('Coming in Phase 2 (Configuration slide)', 'warning');
  }
</script>

<div class="welcome-slide active">
  <div class="welcome-content">
    <h1 class="welcome-title">Granny Smith</h1>
    <p class="welcome-subtitle">A classic Macintosh emulator in the browser.</p>
    <section class="card">
      <h3 class="card-heading">Start</h3>
      <div class="card-rows">
        <button class="card-row" onclick={notImplemented}>
          <Icon name="mac" />
          <span>New Machine...</span>
        </button>
        <button class="card-row" onclick={notImplemented}>
          <Icon name="clock" />
          <span>Open Checkpoint...</span>
        </button>
        <button class="card-row" onclick={notImplemented}>
          <Icon name="upload" />
          <span>Upload ROM...</span>
        </button>
      </div>
    </section>
    {#if recents.length > 0}
      <section class="card">
        <h3 class="card-heading">Recent</h3>
        <div class="card-rows">
          {#each recents as r (r.model + r.media)}
            <button class="card-row recent" onclick={notImplemented}>
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
</div>

<style>
  .welcome-slide {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: flex-start;
    justify-content: center;
    overflow: auto;
    /* Phase 1 ships only the active Home slide so the inactive-state
       transitions specced in Phase 2 are deferred. */
    opacity: 1;
    pointer-events: auto;
  }
  .welcome-content {
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
