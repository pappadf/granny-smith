<script lang="ts">
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { pickAndUpload } from '@/bus/upload';
  import Icon from '../common/Icon.svelte';

  // (An "Open Checkpoint..." row only toasted that it was not written yet;
  // saved states live in the Checkpoints view.)
  // (A "Recent" card used to list /opfs/config/recent.json, which nothing in
  // production ever wrote — only test fixtures, which held display names
  // where machine.boot takes model ids, N-13.  It is gone until something
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
</style>
