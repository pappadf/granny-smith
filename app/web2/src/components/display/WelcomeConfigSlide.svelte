<script lang="ts">
  import { onMount } from 'svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator, opfs, gsEval, whenModuleReady } from '@/bus';
  import { pickAndUpload } from '@/bus/upload';
  import { DEFAULT_CONFIG } from '@/lib/machine';
  import type { ImageCategory } from '@/bus/types';

  const UPLOAD_SENTINEL = 'Upload image...';
  const NONE_SENTINEL = '(none)';

  // One identified ROM in OPFS. `compatible` is the list of model ids that
  // the C side reports this ROM lights up (rom.identify); `name` is the
  // human label baked into the image.
  interface RomEntry {
    path: string;
    name: string;
    checksum: string;
    compatible: string[];
  }

  // Local form state.
  let modelId = $state('');
  let vrom = $state(DEFAULT_CONFIG.vrom);
  let ram = $state(DEFAULT_CONFIG.ram);
  let romPath = $state('');
  let fd = $state(NONE_SENTINEL);
  let hd = $state(NONE_SENTINEL);
  let cd = $state(NONE_SENTINEL);

  // Discovery state.
  let scanning = $state(true);
  let allRoms = $state<RomEntry[]>([]);
  // model id -> display name, populated lazily via machine.profile.
  let modelNames = $state<Record<string, string>>({});
  // model id -> ROMs that boot this model.
  let romsByModel = $derived.by(() => {
    const out: Record<string, RomEntry[]> = {};
    for (const r of allRoms) {
      for (const id of r.compatible) {
        (out[id] ??= []).push(r);
      }
    }
    return out;
  });
  let modelOptions = $derived(
    Object.keys(romsByModel).map((id) => ({ id, label: modelNames[id] ?? id })),
  );
  let romsForCurrentModel = $derived(modelId ? (romsByModel[modelId] ?? []) : []);
  let needsRomPicker = $derived(romsForCurrentModel.length > 1);
  let modelName = $derived(modelId ? (modelNames[modelId] ?? modelId) : '');

  let vromOptions = $state<string[]>(['(auto)']);
  let fdOptions = $state<string[]>([NONE_SENTINEL]);
  let hdOptions = $state<string[]>([NONE_SENTINEL]);
  let cdOptions = $state<string[]>([NONE_SENTINEL]);

  async function identifyRom(path: string): Promise<RomEntry | null> {
    const r = await gsEval('rom.identify', [path]);
    if (typeof r !== 'string') return null;
    try {
      const parsed = JSON.parse(r) as {
        recognised?: boolean;
        compatible?: string[];
        checksum?: string;
        name?: string;
      };
      if (!parsed.recognised || !Array.isArray(parsed.compatible)) return null;
      return {
        path,
        name: parsed.name ?? path.split('/').pop() ?? path,
        checksum: parsed.checksum ?? '',
        compatible: parsed.compatible,
      };
    } catch {
      return null;
    }
  }

  async function resolveModelName(id: string): Promise<string> {
    if (modelNames[id]) return modelNames[id];
    const r = await gsEval('machine.profile', [id]);
    if (typeof r === 'string') {
      try {
        const parsed = JSON.parse(r) as { name?: string };
        if (parsed.name) {
          modelNames = { ...modelNames, [id]: parsed.name };
          return parsed.name;
        }
      } catch {
        /* fall through */
      }
    }
    modelNames = { ...modelNames, [id]: id };
    return id;
  }

  async function refreshOpfs() {
    scanning = true;
    const [roms, vroms, fds, hds, cds] = await Promise.all([
      opfs.scanRoms().catch(() => []),
      opfs.scanImages('vrom').catch(() => []),
      opfs.scanImages('fd').catch(() => []),
      opfs.scanImages('hd').catch(() => []),
      opfs.scanImages('cd').catch(() => []),
    ]);

    // Identify every ROM in parallel. Drop the unrecognised ones.
    const identified = (await Promise.all(roms.map((r) => identifyRom(r.path)))).filter(
      (e): e is RomEntry => e !== null,
    );
    allRoms = identified;

    // Look up display names for every model surfaced by these ROMs.
    const seenIds: string[] = [];
    for (const r of identified) {
      for (const id of r.compatible) {
        if (!seenIds.includes(id)) seenIds.push(id);
      }
    }
    await Promise.all(seenIds.map(resolveModelName));

    // Default the model selection to the first compatible model we found.
    if (!modelId || !seenIds.includes(modelId)) {
      modelId = seenIds[0] ?? '';
    }

    vromOptions = ['(auto)', ...vroms.map((v) => v.name)];
    fdOptions = [NONE_SENTINEL, ...fds.map((f) => f.name), UPLOAD_SENTINEL];
    hdOptions = [NONE_SENTINEL, ...hds.map((h) => h.name), UPLOAD_SENTINEL];
    cdOptions = [NONE_SENTINEL, ...cds.map((c) => c.name), UPLOAD_SENTINEL];
    scanning = false;
  }

  // Keep romPath sync'd with the current model. When the dropdown is hidden
  // (single ROM match) we still need romPath set so submit can find it.
  $effect(() => {
    const list = romsForCurrentModel;
    if (!list.length) {
      romPath = '';
    } else if (!list.find((r) => r.path === romPath)) {
      romPath = list[0].path;
    }
  });

  onMount(() => {
    void (async () => {
      await whenModuleReady();
      await refreshOpfs();
    })();
  });

  function onBack(e: Event) {
    e.preventDefault();
    setWelcomeSlide('home');
  }

  async function interceptIfUpload(value: string, category: ImageCategory): Promise<string | null> {
    if (value !== UPLOAD_SENTINEL) return value;
    await pickAndUpload();
    await refreshOpfs();
    void category;
    return null;
  }

  async function onFdChange(e: Event) {
    const v = (e.target as HTMLSelectElement).value;
    const result = await interceptIfUpload(v, 'fd');
    fd = result ?? NONE_SENTINEL;
  }
  async function onHdChange(e: Event) {
    const v = (e.target as HTMLSelectElement).value;
    const result = await interceptIfUpload(v, 'hd');
    hd = result ?? NONE_SENTINEL;
  }
  async function onCdChange(e: Event) {
    const v = (e.target as HTMLSelectElement).value;
    const result = await interceptIfUpload(v, 'cd');
    cd = result ?? NONE_SENTINEL;
  }

  async function onSubmit(e: Event) {
    e.preventDefault();
    if (!modelId || !romsForCurrentModel.length) {
      showNotification('Upload a ROM first via drag-and-drop or the Upload ROM button', 'warning');
      return;
    }
    const selected = romsForCurrentModel.find((r) => r.path === romPath) ?? romsForCurrentModel[0];
    const vromPath = vrom === '(auto)' ? '(auto)' : `/opfs/images/vrom/${vrom}`;
    const fdPath = fd === NONE_SENTINEL ? NONE_SENTINEL : `/opfs/images/fd/${fd}`;
    const hdPath = hd === NONE_SENTINEL ? NONE_SENTINEL : `/opfs/images/hd/${hd}`;
    const cdPath = cd === NONE_SENTINEL ? NONE_SENTINEL : `/opfs/images/cd/${cd}`;
    await initEmulator({
      model: modelId,
      modelName,
      rom: selected.path,
      vrom: vromPath,
      ram,
      fd: fdPath,
      hd: hdPath,
      cd: cdPath,
    });
    setWelcomeSlide('home');
  }

  let canStart = $derived(!scanning && !!modelId && romsForCurrentModel.length > 0);
</script>

<div class="config-content">
  <a href="#back" class="back-link" onclick={onBack}>← Back</a>
  <h2 class="config-title">New Machine</h2>
  <form class="config-form" onsubmit={onSubmit}>
    {#if scanning}
      <div class="form-row">
        <span class="form-label">Machine Model</span>
        <div class="form-help">Scanning ROMs…</div>
      </div>
    {:else if modelOptions.length === 0}
      <div class="form-row">
        <span class="form-label">Machine Model</span>
        <div class="form-help">
          No ROMs in storage. Drag-and-drop a ROM file or use the Upload ROM button on the Home
          slide.
        </div>
      </div>
    {:else}
      <div class="form-row">
        <label for="cfg-model">Machine Model</label>
        <select id="cfg-model" bind:value={modelId}>
          {#each modelOptions as opt (opt.id)}
            <option value={opt.id}>{opt.label}</option>
          {/each}
        </select>
      </div>
      {#if needsRomPicker}
        <div class="form-row">
          <label for="cfg-rom">ROM Image</label>
          <select id="cfg-rom" bind:value={romPath}>
            {#each romsForCurrentModel as r (r.path)}
              <option value={r.path}>{r.name}</option>
            {/each}
          </select>
        </div>
      {/if}
      <div class="form-row">
        <label for="cfg-vrom">Video ROM</label>
        <select id="cfg-vrom" bind:value={vrom}>
          {#each vromOptions as v (v)}
            <option>{v}</option>
          {/each}
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
        <select id="cfg-fd" value={fd} onchange={onFdChange}>
          {#each fdOptions as opt (opt)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      <div class="form-row">
        <label for="cfg-hd">SCSI HD 0</label>
        <select id="cfg-hd" value={hd} onchange={onHdChange}>
          {#each hdOptions as opt (opt)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      <div class="form-row">
        <label for="cfg-cd">SCSI CD-ROM</label>
        <select id="cfg-cd" value={cd} onchange={onCdChange}>
          {#each cdOptions as opt (opt)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
    {/if}
    <div class="form-divider"></div>
    <div class="form-actions">
      <button type="submit" class="primary-button" disabled={!canStart}>Start Machine</button>
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
  .form-row label,
  .form-row .form-label {
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
  .form-help {
    color: var(--gs-fg-muted);
    font-size: 12px;
    line-height: 1.4;
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
  .primary-button:hover:not(:disabled) {
    background: var(--gs-primary-hover);
  }
  .primary-button:active:not(:disabled) {
    background: var(--gs-primary-active);
  }
  .primary-button:disabled {
    background: #777;
    cursor: default;
    opacity: 0.5;
  }
</style>
