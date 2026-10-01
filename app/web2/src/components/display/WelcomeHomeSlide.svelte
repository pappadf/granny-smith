<script lang="ts">
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { pickAndUpload, pickAndLoadCheckpoint } from '@/bus/upload';
  import Icon from '../common/Icon.svelte';

  // (A "Recent" card used to list /opfs/config/recent.json, which nothing in
  // production ever wrote — only test fixtures, which held display names
  // where machine.boot takes model ids.  It is gone until something
  // records a machine worth relaunching.)

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
  <h1 class="welcome-title">Granny Smith</h1>
  <p class="welcome-subtitle">A classic Macintosh emulator in the browser.</p>
  <section class="card">
    <h3 class="card-heading">Start</h3>
    <div class="card-rows">
      <button class="card-row" onclick={openConfigSlide}>
        <Icon name="mac" />
        <span>New Machine...</span>
      </button>
      <button class="card-row" onclick={openCheckpoint}>
        <Icon name="clock" />
        <span>Open Checkpoint...</span>
      </button>
      <button class="card-row" onclick={openUploadRom}>
        <Icon name="upload" />
        <span>Upload ROM...</span>
      </button>
    </div>
  </section>
</div>

<style>
  .home-content {
    max-width: 560px;
    width: 100%;
    padding: var(--gs-space-12) var(--gs-space-8) var(--gs-space-8);
  }
  .welcome-title {
    font-size: var(--gs-font-size-4xl);
    font-weight: var(--gs-font-weight-light);
    color: var(--gs-text-strong);
    margin: 0 0 var(--gs-space-2) 0;
  }
  .welcome-subtitle {
    color: var(--gs-text-muted);
    margin: 0 0 var(--gs-space-7) 0;
    font-size: var(--gs-font-size-md);
  }
  .card {
    background: var(--gs-card-bg);
    border: var(--gs-border-width) solid var(--gs-border-card);
    border-radius: var(--gs-radius-lg);
    padding: var(--gs-space-3-5) var(--gs-space-4);
    margin-bottom: var(--gs-space-4);
  }
  .card-heading {
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
    text-transform: var(--gs-caps-transform);
    letter-spacing: var(--gs-caps-tracking);
    color: var(--gs-text-muted);
    margin: 0 0 var(--gs-space-2) 0;
  }
  .card-rows {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  .card-row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2-5);
    padding: var(--gs-space-1-5) var(--gs-space-2);
    background: transparent;
    border: none;
    border-radius: var(--gs-radius-sm);
    color: var(--gs-text-link);
    cursor: pointer;
    text-align: left;
    font-size: var(--gs-font-size-base);
  }
  .card-row:hover {
    background: var(--gs-row-hover);
  }
  .card-row :global(.icon) {
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
    color: var(--gs-text);
  }
</style>
