<script lang="ts">
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { pickAndUpload, pickAndLoadCheckpoint } from '@/bus/upload';
  import ActionRow from '../ui/ActionRow.svelte';
  import Card from '../ui/Card.svelte';
  import Hero from '../ui/Hero.svelte';

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
      <ActionRow class="card-row" icon="upload" label="Upload ROM..." onclick={openUploadRom} />
    </div>
  </Card>
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
</style>
