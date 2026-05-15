<script lang="ts">
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator } from '@/bus';
  import { DEFAULT_CONFIG } from '@/lib/machine';

  // Local form state. Defaults match prototype's selected option per field.
  let model = $state(DEFAULT_CONFIG.model);
  let vrom = $state(DEFAULT_CONFIG.vrom);
  let ram = $state(DEFAULT_CONFIG.ram);
  let fd = $state(DEFAULT_CONFIG.fd);
  let hd = $state(DEFAULT_CONFIG.hd);
  let cd = $state(DEFAULT_CONFIG.cd);

  const UPLOAD_SENTINELS = new Set(['Upload image...', 'Create blank image...']);

  function onBack(e: Event) {
    e.preventDefault();
    setWelcomeSlide('home');
  }

  function intercept(value: string, current: string): string {
    // Selecting an "Upload image..." option toasts a not-implemented warning
    // and reverts the select to its previous value. The underlying upload
    // flow lands in Phase 3.
    if (UPLOAD_SENTINELS.has(value)) {
      showNotification('Coming in Phase 3 (real emulator)', 'warning');
      return current;
    }
    return value;
  }

  async function onSubmit(e: Event) {
    e.preventDefault();
    await initEmulator({ model, vrom, ram, fd, hd, cd });
    // Reset the slide so the next Shut Down → Welcome lands on Home.
    setWelcomeSlide('home');
  }
</script>

<div class="config-content">
  <a href="#back" class="back-link" onclick={onBack}>← Back</a>
  <h2 class="config-title">New Machine</h2>
  <form class="config-form" onsubmit={onSubmit}>
    <div class="form-row">
      <label for="cfg-model">Machine Model</label>
      <select id="cfg-model" bind:value={model}>
        <option>Macintosh Plus</option>
        <option>Macintosh SE</option>
        <option>Macintosh SE/30</option>
        <option>Macintosh IIci</option>
      </select>
    </div>
    <div class="form-row">
      <label for="cfg-vrom">Video ROM</label>
      <select id="cfg-vrom" bind:value={vrom}>
        <option>(auto)</option>
        <option>ROM_97221136</option>
        <option>ROM_SE30_VROM</option>
      </select>
    </div>
    <div class="form-row">
      <label for="cfg-ram">RAM</label>
      <select id="cfg-ram" bind:value={ram}>
        <option>1 MB</option>
        <option>2 MB</option>
        <option>4 MB</option>
        <option>8 MB</option>
        <option>16 MB</option>
      </select>
    </div>
    <div class="form-divider"></div>
    <div class="form-row">
      <label for="cfg-fd">Internal Floppy</label>
      <select
        id="cfg-fd"
        value={fd}
        onchange={(e) => (fd = intercept((e.target as HTMLSelectElement).value, fd))}
      >
        <option>(none)</option>
        <option>System_7.0_Install.dsk</option>
        <option>Disk_Tools.dsk</option>
        <option>Upload image...</option>
      </select>
    </div>
    <div class="form-row">
      <label for="cfg-hd">SCSI HD 0</label>
      <select
        id="cfg-hd"
        value={hd}
        onchange={(e) => (hd = intercept((e.target as HTMLSelectElement).value, hd))}
      >
        <option>(none)</option>
        <option>hd1.img</option>
        <option>hd2.img</option>
        <option>Upload image...</option>
        <option>Create blank image...</option>
      </select>
    </div>
    <div class="form-row">
      <label for="cfg-cd">SCSI CD-ROM</label>
      <select
        id="cfg-cd"
        value={cd}
        onchange={(e) => (cd = intercept((e.target as HTMLSelectElement).value, cd))}
      >
        <option>(none)</option>
        <option>system7.iso</option>
        <option>Upload image...</option>
      </select>
    </div>
    <div class="form-divider"></div>
    <div class="form-actions">
      <button type="submit" class="primary-button">Start Machine</button>
    </div>
  </form>
</div>

<style>
  .config-content {
    max-width: 560px;
    width: 100%;
    padding: 48px 32px 32px;
  }
  .back-link {
    display: inline-block;
    color: var(--gs-link);
    text-decoration: none;
    margin-bottom: 16px;
    font-size: 13px;
  }
  .back-link:hover {
    text-decoration: underline;
  }
  .config-title {
    font-size: 22px;
    font-weight: 200;
    color: var(--gs-fg-bright);
    margin: 0 0 20px 0;
  }
  .config-form {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .form-row {
    display: grid;
    grid-template-columns: 140px 1fr;
    align-items: center;
    gap: 12px;
  }
  .form-row label {
    color: var(--gs-fg);
    opacity: 0.9;
    font-size: 13px;
  }
  .form-row select {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 26px;
    padding: 0 6px;
    font-size: 13px;
    outline: none;
  }
  .form-row select:focus {
    border-color: var(--gs-focus);
  }
  .form-divider {
    height: 1px;
    background: var(--gs-border);
    margin: 6px 0;
  }
  .form-actions {
    display: flex;
    justify-content: flex-end;
    margin-top: 16px;
  }
  .primary-button {
    background: var(--gs-primary-bg);
    color: var(--gs-primary-fg);
    border: none;
    border-radius: 0;
    padding: 6px 14px;
    font-size: 13px;
    cursor: pointer;
    height: 30px;
  }
  .primary-button:hover {
    background: var(--gs-primary-hover);
  }
  .primary-button:active {
    background: var(--gs-primary-active);
  }
</style>
