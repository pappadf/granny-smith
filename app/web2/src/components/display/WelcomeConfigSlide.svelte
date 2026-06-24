<script lang="ts">
  import { onMount } from 'svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator, opfs, gsEval, whenModuleReady } from '@/bus';
  import { pickAndUploadAs } from '@/bus/upload';
  import type { MediaTypeId } from '@/lib/media';
  import { DEFAULT_CONFIG } from '@/lib/machine';
  import type { ImageCategory } from '@/bus/types';
  import { images } from '@/state/images.svelte';
  import CreateImageDialog from './CreateImageDialog.svelte';

  const UPLOAD_SENTINEL = 'Upload image...';
  const CREATE_SENTINEL = 'Create blank image...';
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

  // Static configuration shape per model, returned by `machine.profile(id)`.
  // The slide reads `name` for the dropdown label, `video_slots` to decide
  // whether to show the Video ROM row (per-card requires_vrom) and to build
  // the video-mode list, `ram_options` / `ram_default` to build the RAM
  // dropdown, and `floppy_slots` for the floppy rows.
  interface MachineProfile {
    name?: string;
    ram_options?: number[]; // KB
    ram_default?: number; // KB
    floppy_slots?: Array<{ label?: string; kind?: string }>;
    scsi_slots?: Array<{ label?: string; id?: number }>;
    // How the hard disk attaches: 'scsi' (default) or 'profile' (Lisa/XL
    // parallel-port ProFile). Drives the HD row label and the attach call.
    hd_bus?: string;
    has_cdrom?: boolean; // documented UX gate: show the SCSI CD-ROM row iff true
    // Derived capability probe (proposal §4.4): the typed facts the UI reads
    // instead of guessing from the model name.
    capabilities?: {
      cpu?: { model?: number; address_bits?: number; fpu?: boolean };
      mmu?: { present?: boolean; kind?: string };
      nubus?: boolean;
    };
    // Per-card video slot shape (proposal §4.4) — the single source for both
    // the VROM requirement (per-card requires_vrom; see needsVrom) and the
    // video-mode list (each card's monitors × depths; see videoModes).
    video_slots?: Array<{
      slot: string;
      fixed: boolean;
      default_card: string;
      cards: Array<{
        id: string;
        requires_vrom: boolean;
        monitors?: Array<{
          id: string;
          name?: string;
          width?: number;
          height?: number;
          depths?: number[];
        }>;
      }>;
    }>;
  }

  // Local form state.
  let modelId = $state('');
  let vrom = $state(DEFAULT_CONFIG.vrom);
  let ram = $state(DEFAULT_CONFIG.ram);
  let romPath = $state('');
  let floppies = $state<string[]>([]);
  let hd = $state(NONE_SENTINEL);
  let cd = $state(NONE_SENTINEL);
  let videoMode = $state('');

  // Discovery state.
  let scanning = $state(true);
  let allRoms = $state<RomEntry[]>([]);
  // model id -> profile, populated lazily via gsEval('machine.profile').
  let profiles = $state<Record<string, MachineProfile>>({});
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
    Object.keys(romsByModel).map((id) => ({ id, label: profiles[id]?.name ?? id })),
  );
  let romsForCurrentModel = $derived(modelId ? (romsByModel[modelId] ?? []) : []);
  let needsRomPicker = $derived(romsForCurrentModel.length > 1);
  let currentProfile = $derived(modelId ? profiles[modelId] : undefined);
  let modelName = $derived(currentProfile?.name ?? modelId);
  // VROM row visibility is driven entirely by the *selected card* (the
  // SE/30-vs-IIci asymmetry): a card declares requires_vrom, not the machine.
  let needsVrom = $derived(
    (currentProfile?.video_slots ?? []).some((s) => {
      const card = s.cards?.find((c) => c.id === s.default_card) ?? s.cards?.[0];
      return card?.requires_vrom === true;
    }),
  );
  // HD row label: the Lisa/XL parallel-port ProFile (hd_bus === 'profile') is
  // not on the SCSI bus, so its label comes from the bus, not scsi_slots (which
  // is empty for those machines). SCSI machines keep their profile slot label.
  let hdSlotLabel = $derived(
    currentProfile?.hd_bus === 'profile'
      ? 'ProFile'
      : (currentProfile?.scsi_slots?.[0]?.label ?? 'SCSI HD 0'),
  );
  // Only machines whose profile advertises a CD-ROM (has_cdrom) show the CD row.
  let hasCdrom = $derived(currentProfile?.has_cdrom === true);
  let ramOptions = $derived.by(() => {
    const opts = currentProfile?.ram_options ?? [];
    if (opts.length) return opts.map(formatRamKb);
    return ['1 MB', '2 MB', '4 MB', '8 MB', '16 MB'];
  });
  let floppySlots = $derived(currentProfile?.floppy_slots ?? []);
  // Video-mode list derived from video_slots: each selectable card's
  // monitors × supported depths.  Ids/labels match what the C side used to
  // emit as the flat video_modes array ("<monitor>_<depth>bpp"), so the
  // boot-time `nubus.video_mode` seed is unchanged.
  let videoModes = $derived.by(() => {
    const out: Array<{ id: string; label: string }> = [];
    for (const s of currentProfile?.video_slots ?? []) {
      for (const c of s.cards ?? []) {
        for (const m of c.monitors ?? []) {
          for (const d of m.depths ?? []) {
            out.push({
              id: `${m.id}_${d}bpp`,
              label: `${m.name ?? m.id} · ${m.width}×${m.height} · ${d} bpp`,
            });
          }
        }
      }
    }
    return out;
  });

  let vromOptions = $state<string[]>(['(auto)']);
  let fdOptions = $state<string[]>([NONE_SENTINEL]);
  let hdOptions = $state<string[]>([NONE_SENTINEL]);
  let cdOptions = $state<string[]>([NONE_SENTINEL]);

  // Create-blank-image dialog state.
  let createOpen = $state(false);
  let createKind = $state<'hd' | 'fd'>('hd');
  let createFdSlot = $state(0);

  function formatRamKb(kb: number): string {
    if (kb >= 1024 && kb % 1024 === 0) return `${kb / 1024} MB`;
    if (kb >= 1024) return `${(kb / 1024).toFixed(1)} MB`;
    return `${kb} KB`;
  }

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

  async function resolveProfile(id: string): Promise<MachineProfile> {
    if (profiles[id]) return profiles[id];
    const r = await gsEval('machine.profile', [id]);
    let parsed: MachineProfile = {};
    if (typeof r === 'string') {
      try {
        parsed = JSON.parse(r) as MachineProfile;
      } catch {
        /* fall through with empty profile */
      }
    }
    profiles = { ...profiles, [id]: parsed };
    return parsed;
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
    await Promise.all(seenIds.map(resolveProfile));

    // Default the model selection to the first compatible model we found.
    if (!modelId || !seenIds.includes(modelId)) {
      modelId = seenIds[0] ?? '';
    }

    vromOptions = ['(auto)', ...vroms.map((v) => v.name)];
    fdOptions = [NONE_SENTINEL, ...fds.map((f) => f.name), UPLOAD_SENTINEL, CREATE_SENTINEL];
    hdOptions = [NONE_SENTINEL, ...hds.map((h) => h.name), UPLOAD_SENTINEL, CREATE_SENTINEL];
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

  // When the *selected model* changes, reset RAM to the new model's
  // ram_default (matches the legacy dialog — every model change rebuilds
  // the RAM dropdown around the profile's recommended value) and resize
  // the floppy-selection array to match the new slot count.
  let appliedFor = $state('');
  $effect(() => {
    if (!currentProfile || modelId === appliedFor) return;
    appliedFor = modelId;
    const dflt = currentProfile.ram_default;
    ram = dflt ? formatRamKb(dflt) : (ramOptions[0] ?? DEFAULT_CONFIG.ram);
    floppies = new Array<string>(floppySlots.length).fill(NONE_SENTINEL);
    videoMode = videoModes[0]?.id ?? '';
  });

  onMount(() => {
    void (async () => {
      await whenModuleReady();
      await refreshOpfs();
    })();
  });

  // Re-scan OPFS whenever the image catalog changes elsewhere — uploads
  // via the Welcome "Upload ROM..." button on the Home slide, uploads /
  // renames / deletes from the Images panel, etc. The slides in this
  // view are kept mounted (just CSS-hidden), so onMount only fires once
  // per page load; without this effect the dropdowns would stay stale
  // and the user would have to reload to see new images.
  let lastSeenRevision = -1;
  $effect(() => {
    const rev = images.revision;
    if (rev === lastSeenRevision) return;
    if (lastSeenRevision !== -1) void refreshOpfs();
    lastSeenRevision = rev;
  });

  function onBack(e: Event) {
    e.preventDefault();
    setWelcomeSlide('home');
  }

  async function interceptIfUpload(value: string, category: ImageCategory): Promise<string | null> {
    if (value !== UPLOAD_SENTINEL) return value;
    // Map the dropdown's category (uses 'cd' as the ImageCategory key)
    // to the upload pipeline's MediaTypeId ('cdrom') and pick strictly:
    // a file uploaded into the floppy slot must validate AS a floppy
    // or it's rejected. Prevents accidentally classifying an HD image
    // as a floppy via the auto-detect order.
    const mediaId: MediaTypeId = category === 'cd' ? 'cdrom' : (category as MediaTypeId);
    await pickAndUploadAs(mediaId);
    await refreshOpfs();
    return null;
  }

  async function onFdChange(e: Event, slotIndex: number) {
    const v = (e.target as HTMLSelectElement).value;
    if (v === CREATE_SENTINEL) {
      // Revert the dropdown off the sentinel, then open the create dialog.
      const reverted = floppies.slice();
      reverted[slotIndex] = NONE_SENTINEL;
      floppies = reverted;
      createKind = 'fd';
      createFdSlot = slotIndex;
      createOpen = true;
      return;
    }
    const result = await interceptIfUpload(v, 'fd');
    const next = floppies.slice();
    next[slotIndex] = result ?? NONE_SENTINEL;
    floppies = next;
  }
  async function onHdChange(e: Event) {
    const v = (e.target as HTMLSelectElement).value;
    if (v === CREATE_SENTINEL) {
      hd = NONE_SENTINEL;
      createKind = 'hd';
      createOpen = true;
      return;
    }
    const result = await interceptIfUpload(v, 'hd');
    hd = result ?? NONE_SENTINEL;
  }

  // A blank image was created in /opfs/images/{hd,fd}/. Re-scan so the
  // dropdown lists it, then select it.
  async function onImageCreated(name: string) {
    createOpen = false;
    await refreshOpfs();
    if (createKind === 'hd') {
      hd = name;
    } else {
      const next = floppies.slice();
      next[createFdSlot] = name;
      floppies = next;
    }
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
    const vromPath = !needsVrom || vrom === '(auto)' ? '(auto)' : `/opfs/images/vrom/${vrom}`;
    const floppyPaths = floppies.map((f) =>
      f === NONE_SENTINEL || !f ? '' : `/opfs/images/fd/${f}`,
    );
    const hdPath = hd === NONE_SENTINEL ? NONE_SENTINEL : `/opfs/images/hd/${hd}`;
    const cdPath = cd === NONE_SENTINEL ? NONE_SENTINEL : `/opfs/images/cd/${cd}`;
    await initEmulator({
      model: modelId,
      modelName,
      rom: selected.path,
      vrom: vromPath,
      // Seed the selected JMFB video mode (matches web-legacy's bootFromConfig).
      // Without it the JMFB never seeds its slot-PRAM/video defaults and A/UX
      // hangs enabling its device drivers on real hardware.
      videoMode: videoMode || undefined,
      ram,
      floppies: floppyPaths,
      hd: hdPath,
      hdBus: currentProfile?.hd_bus === 'profile' ? 'profile' : 'scsi',
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
      {#if needsVrom}
        <div class="form-row">
          <label for="cfg-vrom">Video ROM</label>
          <select id="cfg-vrom" bind:value={vrom}>
            {#each vromOptions as v (v)}
              <option>{v}</option>
            {/each}
          </select>
        </div>
      {/if}
      {#if videoModes.length > 1}
        <div class="form-row">
          <label for="cfg-video-mode">Video Mode</label>
          <select id="cfg-video-mode" bind:value={videoMode}>
            {#each videoModes as m (m.id)}
              <option value={m.id}>{m.label ?? m.id}</option>
            {/each}
          </select>
        </div>
      {/if}
      <div class="form-row">
        <label for="cfg-ram">RAM</label>
        <select id="cfg-ram" bind:value={ram}>
          {#each ramOptions as opt (opt)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      <div class="form-divider"></div>
      {#each floppySlots as slot, i (i)}
        <div class="form-row">
          <label for={`cfg-fd${i}`}>{slot.label ?? `Floppy ${i}`}</label>
          <select
            id={`cfg-fd${i}`}
            value={floppies[i] ?? NONE_SENTINEL}
            onchange={(e) => onFdChange(e, i)}
          >
            {#each fdOptions as opt (opt)}
              <option>{opt}</option>
            {/each}
          </select>
        </div>
      {/each}
      <div class="form-row">
        <label for="cfg-hd">{hdSlotLabel}</label>
        <select id="cfg-hd" value={hd} onchange={onHdChange}>
          {#each hdOptions as opt (opt)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      {#if hasCdrom}
        <div class="form-row">
          <label for="cfg-cd">SCSI CD-ROM</label>
          <select id="cfg-cd" value={cd} onchange={onCdChange}>
            {#each cdOptions as opt (opt)}
              <option>{opt}</option>
            {/each}
          </select>
        </div>
      {/if}
    {/if}
    <div class="form-divider"></div>
    <div class="form-actions">
      <button type="submit" class="primary-button" disabled={!canStart}>Start Machine</button>
    </div>
  </form>
</div>

<CreateImageDialog
  open={createOpen}
  kind={createKind}
  bus={currentProfile?.hd_bus === 'profile' ? 'profile' : 'scsi'}
  onClose={() => (createOpen = false)}
  onCreated={onImageCreated}
/>

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
