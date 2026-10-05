<script lang="ts">
  // The configuration dialog: a renderer over the machine-description tree
  // (bus/profile.ts) that edits a configuration document (lib/machineConfig.ts)
  // and boots it.  Every label, every choice and every default is the core's
  // (catalog.profile); nothing here knows a machine, a bus or a card.  The
  // only state the dialog adds is the images: which file goes into which
  // floppy drive, hard disk and CD-ROM drive.
  import { onMount, tick, untrack } from 'svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator, opfs, gsEval, whenModuleReady } from '@/bus';
  import { pickAndUploadAs } from '@/bus/upload';
  import {
    getProfile,
    clearProfileCache,
    type BlankDisk,
    type Card,
    type ConfigDocument,
    type ConfigOption,
    type MachineProfile,
  } from '@/bus/profile';
  import { identifyRom, type MediaTypeId, type RomIdentity } from '@/lib/media';
  import * as mc from '@/lib/machineConfig';
  import type { ImageCategory, MediaImage, OpfsEntry } from '@/bus/types';
  import { images } from '@/state/images.svelte';
  import CreateImageDialog from './CreateImageDialog.svelte';
  import Button from '../ui/Button.svelte';
  import Field from '../ui/Field.svelte';
  import FormGrid from '../ui/FormGrid.svelte';
  import IconButton from '../ui/IconButton.svelte';
  import Link from '../ui/Link.svelte';
  import SectionHeading from '../ui/SectionHeading.svelte';
  import Select from '../ui/Select.svelte';
  import Separator from '../ui/Separator.svelte';

  const UPLOAD_SENTINEL = 'Upload image...';
  const CREATE_SENTINEL = 'Create blank image...';
  const NO_DISK = '(no disk)';
  const NO_DISC = '(no disc)';
  const CHOOSE_IMAGE = 'Choose image…';

  // --- Model and ROM -------------------------------------------------------
  let modelId = $state('');
  let romPath = $state('');
  let scanning = $state(true);
  let startError = $state<string | null>(null); // the emulator failed to start
  let allRoms = $state<RomIdentity[]>([]);
  // model id -> its tree, from catalog.profile.
  let profiles = $state<Record<string, MachineProfile>>({});

  // model id -> the distinct ROMs (by content id) that boot it.  Two files
  // of one ROM are one choice: the first file found stands for it.
  let romsByModel = $derived.by(() => {
    const out: Record<string, RomIdentity[]> = {};
    for (const r of allRoms) {
      for (const id of new Set(r.compatible)) {
        const list = (out[id] ??= []);
        if (!list.some((e) => e.id === r.id)) list.push(r);
      }
    }
    return out;
  });
  // One Model entry per model/ROM pair, so every ROM a model can run is
  // directly choosable: "<model> (<variant>)" when several ROMs boot it.
  let modelOptions = $derived(
    Object.entries(romsByModel).flatMap(([id, roms]) =>
      roms.map((r) => {
        const model = profiles[id]?.name ?? id;
        return {
          key: roms.length > 1 ? `${id}/${r.id}` : id,
          model: id,
          rom: r,
          label: r.variant
            ? `${model} (${r.variant})`
            : roms.length > 1
              ? `${model} (${r.id})`
              : model,
        };
      }),
    ),
  );
  let romsForCurrentModel = $derived(modelId ? (romsByModel[modelId] ?? []) : []);
  let currentRomId = $derived(romsForCurrentModel.find((r) => r.path === romPath)?.id ?? '');
  let profile = $derived(modelId ? profiles[modelId] : undefined);

  // --- The document ----------------------------------------------------------
  // The configuration being edited: the model's defaults on open and on every
  // model change (no remembered state), and on "Reset to defaults".
  let doc = $state<ConfigDocument | null>(null);
  let docFor = $state('');
  // The defaults `doc` was seeded from, serialised: an untouched document
  // follows the model's defaults when they change under it (a card ROM
  // uploaded while the dialog is open seats the factory card).
  let docSeed = '';
  // Images, by floppy position ("fd0") and by storage position ("scsi:0").
  let floppyImages = $state<Record<string, string>>({});
  let mediaImages = $state<Record<string, string>>({});

  function resetToDefaults(): void {
    if (!profile) return;
    doc = mc.defaultDocument(profile);
    docFor = profile.id;
    docSeed = JSON.stringify(doc);
    floppyImages = {};
    mediaImages = {};
    addingDeviceOn = null;
    addingCard = false;
  }

  $effect(() => {
    if (!profile) return;
    if (docFor !== profile.id) {
      resetToDefaults();
      return;
    }
    // Same model, fresh profile: re-seed only a document nobody has edited
    // (the images chosen for its positions stay).
    const fresh = mc.defaultDocument(profile);
    const seed = JSON.stringify(fresh);
    if (seed !== docSeed && untrack(() => JSON.stringify(doc)) === docSeed) {
      doc = fresh;
      docSeed = seed;
    }
  });

  // --- Image inventories -------------------------------------------------------
  let fdNames = $state<string[]>([]);
  let hdNames = $state<string[]>([]);
  let cdNames = $state<string[]>([]);
  // Filename -> the OPFS path it was scanned from: a category's listing is
  // not always one directory (scanImages('fd') folds in the legacy fdhd/).
  let fdPaths = $state<Record<string, string>>({});
  let hdPaths = $state<Record<string, string>>({});
  let cdPaths = $state<Record<string, string>>({});

  // Collapse a listing to unique filenames, the first (canonical) entry
  // winning, plus the path each resolves to.
  function mediaOptions(entries: OpfsEntry[]): { names: string[]; paths: Record<string, string> } {
    const names: string[] = [];
    const paths: Record<string, string> = {};
    for (const e of entries) {
      if (e.name in paths) continue;
      paths[e.name] = e.path;
      names.push(e.name);
    }
    return { names, paths };
  }

  async function refreshOpfs() {
    scanning = true;
    try {
      const [roms, fds, hds, cds] = await Promise.all([
        opfs.scanRoms().catch(() => []),
        opfs.scanImages('fd').catch(() => []),
        opfs.scanImages('hd').catch(() => []),
        opfs.scanImages('cd').catch(() => []),
      ]);
      const identified = (await Promise.all(roms.map((r) => identifyRom(gsEval, r.path)))).filter(
        (e): e is RomIdentity => e !== null,
      );
      // A card's availability follows the card ROMs stored, which may just
      // have changed: ask the core again.
      clearProfileCache();
      const ids = [...new Set(identified.flatMap((r) => r.compatible))];
      const fetched = await Promise.all(ids.map((id) => getProfile(id)));
      const next: Record<string, MachineProfile> = {};
      ids.forEach((id, i) => {
        const p = fetched[i];
        if (p) next[id] = p;
      });
      // Both at once, so a model is never listed before its name is known.
      allRoms = identified;
      profiles = next;
      if (!modelId || !ids.includes(modelId)) modelId = ids[0] ?? '';

      const fd = mediaOptions(fds);
      const hd = mediaOptions(hds);
      const cd = mediaOptions(cds);
      [fdNames, fdPaths] = [fd.names, fd.paths];
      [hdNames, hdPaths] = [hd.names, hd.paths];
      [cdNames, cdPaths] = [cd.names, cd.paths];
    } finally {
      // Never leave the dialog pinned on "Scanning ROMs…".
      scanning = false;
    }
  }

  // Keep romPath one of the current model's ROMs.
  $effect(() => {
    const list = romsForCurrentModel;
    if (!list.length) romPath = '';
    else if (!list.find((r) => r.path === romPath)) romPath = list[0].path;
  });

  onMount(() => {
    void (async () => {
      try {
        await whenModuleReady();
      } catch (e) {
        startError = e instanceof Error ? e.message : String(e);
        scanning = false;
        return;
      }
      await refreshOpfs();
    })();
  });

  // Re-scan whenever the image catalog changes elsewhere (uploads, renames,
  // deletes): the slides stay mounted, so onMount fires once per page load.
  let lastSeenRevision = -1;
  $effect(() => {
    const rev = images.revision;
    if (rev === lastSeenRevision) return;
    if (lastSeenRevision !== -1) void refreshOpfs();
    lastSeenRevision = rev;
  });

  // --- Derived views of the tree ---------------------------------------------
  let machineOptions = $derived(profile ? mc.visibleOptions(profile.options) : []);
  let devices = $derived(profile && doc ? mc.displayDevices(profile, doc) : []);
  let connectedId = $derived(doc ? mc.connectedDevice(doc) : null);
  let connected = $derived(devices.find((d) => d.id === connectedId));
  let monitorId = $derived(connected && doc ? (doc.displays[connected.id]?.monitor ?? '') : '');
  let monitorChoices = $derived(
    (connected?.monitors ?? [])
      .filter((m) => m !== mc.NO_MONITOR)
      .map((m) => ({ id: m, label: profile?.monitors.find((x) => x.id === m)?.label ?? m })),
  );
  let modeChoices = $derived(connected ? (connected.modes[monitorId] ?? []) : []);
  let modeValue = $derived(connected && doc ? (doc.displays[connected.id]?.mode ?? '') : '');
  let startups = $derived(profile && doc ? mc.startupChoices(profile, doc) : []);
  let warnings = $derived(profile && doc ? mc.configWarnings(profile, doc) : []);
  // A card entry's display status, for its row.
  function cardStatus(slot: string): string {
    return slot === connectedId ? 'connected' : 'no monitor';
  }

  // --- Edits -------------------------------------------------------------------
  function edit(fn: (p: MachineProfile, d: ConfigDocument) => ConfigDocument): void {
    if (profile && doc) doc = fn(profile, doc);
  }
  function optionRowValue(o: ConfigOption): string {
    return doc ? mc.optionValue(doc, o) : o.default;
  }

  // Storage: the inline "Add device" form, per bus.
  let addingDeviceOn = $state<string | null>(null);
  let addType = $state('');
  let addUnit = $state<number>(-1);
  function openAddDevice(bus: string): void {
    if (!profile || !doc) return;
    const b = mc.storageBus(profile, bus);
    addingDeviceOn = bus;
    addType = b?.accepts[0]?.id ?? 'hd';
    addUnit = mc.defaultUnit(profile, doc, bus, addType) ?? -1;
  }
  $effect(() => {
    // A type change moves the proposed unit to that type's preferred one.
    if (!addingDeviceOn || !profile || !doc) return;
    const unit = mc.defaultUnit(profile, doc, addingDeviceOn, addType);
    addUnit = unit ?? -1;
  });
  function confirmAddDevice(): void {
    if (!addingDeviceOn || addUnit < 0) return;
    const bus = addingDeviceOn;
    edit((p, d) => mc.addDevice(p, d, bus, addType, addUnit));
    addingDeviceOn = null;
  }
  // `map` without `key`.
  function without(map: Record<string, string>, key: string): Record<string, string> {
    const next = { ...map };
    delete next[key];
    return next;
  }
  function removeDeviceAt(index: number): void {
    if (!doc) return;
    const gone = doc.storage[index];
    edit((p, d) => mc.removeDevice(p, d, index));
    if (gone) mediaImages = without(mediaImages, mc.positionKey(gone));
  }
  function moveDevice(index: number, unit: number): void {
    if (!doc) return;
    const d = doc.storage[index];
    if (!d) return;
    const oldKey = mc.positionKey(d);
    const image = mediaImages[oldKey];
    edit((_, cur) => mc.setDeviceUnit(cur, index, unit));
    const rest = without(mediaImages, oldKey);
    mediaImages = image ? { ...rest, [mc.positionKey({ bus: d.bus, unit })]: image } : rest;
  }
  function startupValue(): string {
    return doc?.startup ? mc.positionKey(doc.startup) : '';
  }
  function setStartupFrom(value: string): void {
    const [bus, unit] = value.split(':');
    edit((_, d) => mc.setStartup(d, value ? { bus, unit: Number(unit) } : null));
  }

  // Cards: the inline "Add card" form.
  let addingCard = $state(false);
  let addCardId = $state('');
  let addSlot = $state('');
  // Every card of the tree, the ones that cannot be added now disabled with
  // the reason (an unavailable ROM, no free slot it fits).
  let cardChoices = $derived(
    profile && doc
      ? profile.cards.map((c) => {
          const slots = mc.freeSlotsFor(profile!, doc!, c.id);
          const reason =
            c.status === 'unavailable'
              ? (c.reason ?? romNeeded(c))
              : slots.length === 0
                ? 'No free slot it fits'
                : '';
          return { card: c, slots, reason };
        })
      : [],
  );
  let addSlotChoices = $derived(cardChoices.find((c) => c.card.id === addCardId)?.slots ?? []);
  function openAddCard(): void {
    const first = cardChoices.find((c) => !c.reason);
    addCardId = first?.card.id ?? '';
    addSlot = first?.slots[0]?.id ?? '';
    addingCard = true;
  }
  $effect(() => {
    if (addingCard && !addSlotChoices.some((s) => s.id === addSlot))
      addSlot = addSlotChoices[0]?.id ?? '';
  });
  function confirmAddCard(): void {
    if (!addCardId || !addSlot) return;
    const [id, slot] = [addCardId, addSlot];
    edit((p, d) => mc.addCard(p, d, id, slot));
    addingCard = false;
  }
  function romNeeded(c: Card): string {
    return c.rom?.kind === 'prom'
      ? 'Needs the card’s expansion ROM (.prom)'
      : 'Needs the card’s declaration ROM (.vrom)';
  }
  async function uploadCardRom(c: Card): Promise<void> {
    const kind: MediaTypeId = c.rom?.kind === 'prom' ? 'prom' : 'vrom';
    if (await pickAndUploadAs(kind)) await refreshOpfs();
  }

  // --- Image pickers -------------------------------------------------------------
  let createOpen = $state(false);
  let createKind = $state<'hd' | 'fd'>('hd');
  let createDisks = $state<BlankDisk[]>([]);
  let createTarget = $state(''); // "fd0" or "scsi:0"

  // The new value of an image picker: the pick, or for "Upload image…" the
  // uploaded file -- or, when that was cancelled, the previous value (and
  // the <select> is written back, since the state does not change).
  async function pickImage(
    select: HTMLSelectElement,
    previous: string,
    category: ImageCategory,
  ): Promise<string> {
    const value = select.value;
    if (value !== UPLOAD_SENTINEL) return value;
    const mediaId: MediaTypeId = category === 'cd' ? 'cdrom' : (category as MediaTypeId);
    const persisted = await pickAndUploadAs(mediaId);
    await refreshOpfs();
    await tick();
    const paths = category === 'fd' ? fdPaths : category === 'hd' ? hdPaths : cdPaths;
    const name = persisted ? Object.keys(paths).find((n) => paths[n] === persisted) : undefined;
    const next = name ?? previous;
    select.value = next;
    return next;
  }

  async function onFloppyImage(e: Event, id: string): Promise<void> {
    const select = e.target as HTMLSelectElement;
    if (select.value === CREATE_SENTINEL) {
      select.value = floppyImages[id] ?? NO_DISK;
      createKind = 'fd';
      createTarget = id;
      createOpen = true;
      return;
    }
    const v = await pickImage(select, floppyImages[id] ?? NO_DISK, 'fd');
    floppyImages = { ...floppyImages, [id]: v === NO_DISK ? '' : v };
  }

  async function onDeviceImage(e: Event, key: string, type: string, disks: BlankDisk[]) {
    const select = e.target as HTMLSelectElement;
    const empty = type === 'cd' ? NO_DISC : CHOOSE_IMAGE;
    if (select.value === CREATE_SENTINEL) {
      select.value = mediaImages[key] || empty;
      createKind = 'hd';
      createDisks = disks;
      createTarget = key;
      createOpen = true;
      return;
    }
    const v = await pickImage(select, mediaImages[key] || empty, type === 'cd' ? 'cd' : 'hd');
    mediaImages = { ...mediaImages, [key]: v === empty ? '' : v };
  }

  async function onImageCreated(name: string) {
    createOpen = false;
    await refreshOpfs();
    if (createKind === 'fd') floppyImages = { ...floppyImages, [createTarget]: name };
    else mediaImages = { ...mediaImages, [createTarget]: name };
  }

  // --- Start ---------------------------------------------------------------------
  function onBack(e: Event) {
    e.preventDefault();
    setWelcomeSlide('home');
  }

  async function onSubmit(e: Event) {
    e.preventDefault();
    if (!modelId || !romsForCurrentModel.length || !profile || !doc) {
      showNotification('Upload a ROM first via drag-and-drop or the Upload ROM button', 'warning');
      return;
    }
    const selected = romsForCurrentModel.find((r) => r.path === romPath) ?? romsForCurrentModel[0];
    // A hard disk with no image is dropped, never a reason not to start.
    const { doc: booted, dropped } = mc.dropImagelessDisks(profile, doc, mediaImages);
    for (const d of dropped)
      showNotification(
        `Hard disk at ${mc.storageLabel(profile, d)} has no image and was not attached`,
        'warning',
      );
    const floppies: Record<string, string> = {};
    for (const [id, name] of Object.entries(floppyImages)) {
      if (name && (booted.floppies[id] ?? 'none') !== 'none')
        floppies[id] = fdPaths[name] ?? `/opfs/images/fd/${name}`;
    }
    const media: MediaImage[] = [];
    for (const d of booted.storage) {
      const name = mediaImages[mc.positionKey(d)];
      if (!name) continue;
      const paths = d.type === 'cd' ? cdPaths : hdPaths;
      const dir = d.type === 'cd' ? 'cd' : 'hd';
      media.push({ ...d, path: paths[name] ?? `/opfs/images/${dir}/${name}` });
    }
    await initEmulator({
      model: modelId,
      rom: selected.path,
      config: booted,
      floppies,
      media,
    });
    setWelcomeSlide('home');
  }

  let canStart = $derived(!scanning && !!modelId && romsForCurrentModel.length > 0 && !!doc);
</script>

{#snippet optionSelect(o: ConfigOption, id: string, value: string, set: (v: string) => void)}
  <Select {id} {value} onchange={(e) => set((e.target as HTMLSelectElement).value)}>
    {#each o.values as v (v.id)}
      <option value={v.id}>{v.label}</option>
    {/each}
  </Select>
{/snippet}

<div class="config-content">
  <div class="back-row">
    <Link href="#back" class="back-link" icon="arrow-left" onclick={onBack}>Back</Link>
  </div>
  <h2 class="config-title">New Machine</h2>
  <FormGrid class="config-form" onsubmit={onSubmit}>
    {#if startError}
      <Field class="form-row" label="Model" help={`The emulator did not start: ${startError}`} />
    {:else if scanning && modelOptions.length === 0}
      <Field class="form-row" label="Model" help="Scanning ROMs…" />
    {:else if modelOptions.length === 0}
      <Field
        class="form-row"
        label="Model"
        help="No ROMs in storage. Drag-and-drop a ROM file or use the Upload ROM button on the Home slide."
      />
    {:else}
      <SectionHeading class="config-section">Machine</SectionHeading>
      <Field class="form-row" label="Model" for="cfg-model">
        <!-- A choice is a model AND the ROM it boots: selecting one sets both. -->
        <Select
          id="cfg-model"
          bind:value={
            () =>
              modelOptions.find((o) => o.model === modelId && o.rom.id === currentRomId)?.key ?? '',
            (key) => {
              const opt = modelOptions.find((o) => o.key === key);
              if (opt) {
                modelId = opt.model;
                romPath = opt.rom.path;
              }
            }
          }
        >
          {#each modelOptions as opt (opt.key)}
            <option value={opt.key} data-model={opt.model}>{opt.label}</option>
          {/each}
        </Select>
      </Field>
      {#if profile && doc}
        {#each machineOptions as o (o.id)}
          <Field class="form-row" label={o.label} for={`cfg-opt-${o.id}`} help={o.detail}>
            {@render optionSelect(o, `cfg-opt-${o.id}`, optionRowValue(o), (v) =>
              edit((_, d) => mc.setOption(d, o.id, v)),
            )}
          </Field>
        {/each}

        {#if devices.length > 0}
          <SectionHeading class="config-section" rule>Monitor</SectionHeading>
          {#if devices.length > 1}
            <Field class="form-row" label="Connected to" for="cfg-display">
              <Select
                id="cfg-display"
                value={connectedId ?? ''}
                onchange={(e) =>
                  edit((p, d) => mc.connectTo(p, d, (e.target as HTMLSelectElement).value))}
              >
                {#each devices as dev (dev.id)}
                  <option value={dev.id}>{dev.label}</option>
                {/each}
              </Select>
            </Field>
          {/if}
          {#if connected && monitorChoices.length > 1}
            <Field class="form-row" label="Monitor" for="cfg-monitor">
              <Select
                id="cfg-monitor"
                value={monitorId}
                onchange={(e) =>
                  edit((_, d) =>
                    mc.setMonitor(d, connected!.id, (e.target as HTMLSelectElement).value),
                  )}
              >
                {#each monitorChoices as m (m.id)}
                  <option value={m.id}>{m.label}</option>
                {/each}
              </Select>
            </Field>
          {:else if connected && monitorChoices.length === 1}
            <Field class="form-row" label="Monitor" help={monitorChoices[0].label} />
          {/if}
          {#if connected && modeChoices.length > 1}
            <Field class="form-row" label="Video mode" for="cfg-video-mode">
              <Select
                id="cfg-video-mode"
                value={modeValue}
                onchange={(e) =>
                  edit((_, d) =>
                    mc.setMode(d, connected!.id, (e.target as HTMLSelectElement).value),
                  )}
              >
                <option value="">Default (chosen by the Mac)</option>
                {#each modeChoices as m (m.id)}
                  <option value={m.id}>{m.label}</option>
                {/each}
              </Select>
            </Field>
          {/if}
          {#if connected && connected.id !== mc.BUILTIN}
            {@const entry = doc.cards.find((c) => c.slot === connected!.id)}
            {#each mc.visibleOptions(connected.options) as o (o.id)}
              <Field class="form-row" label={o.label} for={`cfg-display-opt-${o.id}`}>
                {@render optionSelect(
                  o,
                  `cfg-display-opt-${o.id}`,
                  entry?.options[o.id] ?? o.default,
                  (v) => edit((_, d) => mc.setCardOption(d, connected!.id, o.id, v)),
                )}
              </Field>
            {/each}
          {/if}
        {/if}

        {#if profile.slots.length > 0}
          <div class="section-head">
            <SectionHeading class="config-section" rule>Expansion cards</SectionHeading>
            {#if !addingCard && cardChoices.some((c) => !c.reason)}
              <Button size="sm" icon="plus" onclick={openAddCard} data-testid="cfg-add-card"
                >Add card</Button
              >
            {/if}
          </div>
          {#each doc.cards as entry (entry.slot)}
            {@const c = mc.card(profile, entry.card)}
            <div class="item-row" data-slot={entry.slot} data-card={entry.card}>
              <span class="item-name">{c?.label ?? entry.card}</span>
              <span class="item-pos">{mc.slotLabel(profile, entry.slot)}</span>
              {#if c && entry.slot !== connectedId}
                {#each mc.visibleOptions(c.options) as o (o.id)}
                  {@render optionSelect(
                    o,
                    `cfg-card-opt-${entry.slot}-${o.id}`,
                    entry.options[o.id] ?? o.default,
                    (v) => edit((_, d) => mc.setCardOption(d, entry.slot, o.id, v)),
                  )}
                {/each}
              {/if}
              {#if c?.class === 'display'}
                <span class="item-status">{cardStatus(entry.slot)}</span>
              {/if}
              <IconButton
                icon="close"
                size="sm"
                tone="panel"
                label={`Remove ${c?.label ?? entry.card}`}
                onclick={() => edit((p, d) => mc.removeCard(p, d, entry.slot))}
              />
            </div>
            {#if c?.status === 'substitute'}
              <p class="item-note">
                Using the emulator’s substitute ROM — upload the card’s ROM to use Apple’s.
                <Link href="#upload" onclick={(e: Event) => (e.preventDefault(), uploadCardRom(c))}
                  >Upload ROM…</Link
                >
              </p>
            {/if}
          {/each}
          {#if addingCard}
            <div class="add-form">
              <Select id="cfg-add-card" bind:value={addCardId} aria-label="Card">
                {#each cardChoices as ch (ch.card.id)}
                  <option value={ch.card.id} disabled={!!ch.reason}
                    >{ch.card.label}{ch.reason ? ` — ${ch.reason}` : ''}</option
                  >
                {/each}
              </Select>
              <Select id="cfg-add-card-slot" bind:value={addSlot} aria-label="Slot">
                {#each addSlotChoices as s (s.id)}
                  <option value={s.id}>{s.label}</option>
                {/each}
              </Select>
              <Button size="sm" variant="primary" onclick={confirmAddCard} disabled={!addSlot}
                >Add</Button
              >
              <Button size="sm" variant="ghost" onclick={() => (addingCard = false)}>Cancel</Button>
            </div>
          {/if}
          {#each cardChoices.filter((ch) => ch.card.status === 'unavailable') as ch (ch.card.id)}
            <p class="item-note">
              {ch.card.label}: {ch.reason}.
              <Link
                href="#upload"
                onclick={(e: Event) => (e.preventDefault(), uploadCardRom(ch.card))}
                >Upload ROM…</Link
              >
            </p>
          {/each}
        {/if}

        {#each warnings as w (w)}
          <p class="item-note warning" role="status">{w}</p>
        {/each}

        {#if profile.floppies.length > 0}
          <SectionHeading class="config-section" rule>Floppy drives</SectionHeading>
          {#each profile.floppies as pos, i (pos.id)}
            {@const type = doc.floppies[pos.id] ?? pos.default}
            <Field class="form-row" label={pos.label} for={`cfg-fd${i}`}>
              <div class="pair">
                {#if pos.types.length > 1}
                  <Select
                    id={`cfg-fd-type-${pos.id}`}
                    aria-label={`${pos.label} type`}
                    value={type}
                    onchange={(e) =>
                      edit((_, d) =>
                        mc.setFloppy(d, pos.id, (e.target as HTMLSelectElement).value),
                      )}
                  >
                    {#each pos.types as t (t.id)}
                      <option value={t.id}>{t.label}</option>
                    {/each}
                  </Select>
                {/if}
                {#if type !== 'none'}
                  <Select
                    id={`cfg-fd${i}`}
                    value={floppyImages[pos.id] || NO_DISK}
                    onchange={(e) => onFloppyImage(e, pos.id)}
                  >
                    <option>{NO_DISK}</option>
                    {#each fdNames as n (n)}
                      <option>{n}</option>
                    {/each}
                    <option>{UPLOAD_SENTINEL}</option>
                    <option>{CREATE_SENTINEL}</option>
                  </Select>
                {/if}
              </div>
            </Field>
          {/each}
        {/if}

        {#if profile.storage.length > 0}
          <SectionHeading class="config-section" rule>Storage</SectionHeading>
          {#each profile.storage as bus (bus.id)}
            {@const onBus = doc.storage
              .map((d, index) => ({ d, index }))
              .filter((x) => x.d.bus === bus.id)}
            <div class="section-head bus-head" data-bus={bus.id}>
              <span class="bus-label">{bus.label}</span>
              {#if addingDeviceOn !== bus.id && mc.freeUnits(profile, doc, bus.id).length > 0}
                <Button
                  size="sm"
                  icon="plus"
                  onclick={() => openAddDevice(bus.id)}
                  data-testid={`cfg-add-device-${bus.id}`}>Add device</Button
                >
              {/if}
            </div>
            {#each onBus as { d, index } (mc.positionKey(d))}
              {@const key = mc.positionKey(d)}
              {@const empty = d.type === 'cd' ? NO_DISC : CHOOSE_IMAGE}
              <div class="item-row device-row" data-device-type={d.type} data-position={key}>
                <span class="item-name">{mc.deviceTypeLabel(profile, d.bus, d.type)}</span>
                <Select
                  id={`cfg-unit-${bus.id}-${d.unit}`}
                  class="unit-select"
                  aria-label="Position"
                  value={d.unit}
                  onchange={(e) => moveDevice(index, Number((e.target as HTMLSelectElement).value))}
                >
                  {#each mc.freeUnits(profile, doc, bus.id, index) as u (u.unit)}
                    <option value={u.unit}>{u.short ?? u.label}</option>
                  {/each}
                </Select>
                <Select
                  id={`cfg-media-${bus.id}-${d.unit}`}
                  class="media-select"
                  title={mediaImages[key] || undefined}
                  aria-label={`${mc.deviceTypeLabel(profile, d.bus, d.type)} image`}
                  value={mediaImages[key] || empty}
                  onchange={(e) => onDeviceImage(e, key, d.type, bus.blank_disks)}
                >
                  <option>{empty}</option>
                  {#each d.type === 'cd' ? cdNames : hdNames as n (n)}
                    <option>{n}</option>
                  {/each}
                  <option>{UPLOAD_SENTINEL}</option>
                  {#if d.type !== 'cd'}<option>{CREATE_SENTINEL}</option>{/if}
                </Select>
                <IconButton
                  icon="close"
                  size="sm"
                  tone="panel"
                  label={`Remove ${mc.deviceTypeLabel(profile, d.bus, d.type)}`}
                  onclick={() => removeDeviceAt(index)}
                />
              </div>
            {/each}
            {#if addingDeviceOn === bus.id}
              <div class="add-form">
                {#if bus.accepts.length > 1}
                  <Select id={`cfg-add-type-${bus.id}`} bind:value={addType} aria-label="Device">
                    {#each bus.accepts as a (a.id)}
                      <option value={a.id}>{a.label}</option>
                    {/each}
                  </Select>
                {:else}
                  <span class="item-name">{bus.accepts[0]?.label}</span>
                {/if}
                <Select id={`cfg-add-unit-${bus.id}`} bind:value={addUnit} aria-label="Position">
                  {#each mc.freeUnits(profile, doc, bus.id) as u (u.unit)}
                    <option value={u.unit}>{u.short ?? u.label}</option>
                  {/each}
                </Select>
                <Button
                  size="sm"
                  variant="primary"
                  onclick={confirmAddDevice}
                  disabled={addUnit < 0}>Add</Button
                >
                <Button size="sm" variant="ghost" onclick={() => (addingDeviceOn = null)}
                  >Cancel</Button
                >
              </div>
            {/if}
          {/each}
          {#if profile.storage.some((b) => b.startup)}
            <Field class="form-row" label="Start up from" for="cfg-startup">
              <Select
                id="cfg-startup"
                value={startupValue()}
                onchange={(e) => setStartupFrom((e.target as HTMLSelectElement).value)}
              >
                {#each startups as s (mc.positionKey(s))}
                  <option value={mc.positionKey(s)}>{s.label}</option>
                {/each}
                <option value="">No default (search all drives)</option>
              </Select>
            </Field>
          {/if}
        {/if}
      {/if}
    {/if}
    <Separator orientation="horizontal" class="form-divider" />
    <div class="form-actions">
      {#if profile && doc}
        <Button size="lg" variant="ghost" onclick={resetToDefaults} data-testid="cfg-reset"
          >Reset to defaults</Button
        >
      {/if}
      <Button type="submit" size="lg" variant="primary" class="primary-button" disabled={!canStart}
        >Start</Button
      >
    </div>
  </FormGrid>
</div>

<CreateImageDialog
  open={createOpen}
  kind={createKind}
  disks={createDisks}
  onClose={() => (createOpen = false)}
  onCreated={onImageCreated}
/>

<style>
  .config-content {
    max-width: 640px;
    width: 100%;
    padding: var(--gs-space-12) var(--gs-space-8) var(--gs-space-8);
  }
  .config-title {
    font-size: var(--gs-font-size-3xl);
    font-weight: var(--gs-font-weight-light);
    color: var(--gs-text-strong);
    margin: 0 0 var(--gs-space-5) 0;
  }
  .back-row {
    margin-bottom: var(--gs-space-4);
  }
  .section-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: var(--gs-space-2);
  }
  .bus-label {
    color: var(--gs-text-muted);
  }
  .item-row,
  .add-form,
  .pair {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--gs-space-2);
  }
  .item-name {
    min-width: 8em;
  }
  /* One device to a line: the type, a narrow unit menu, then the image menu
     taking the rest of the row at a fixed width whatever the image's name. */
  .device-row {
    flex-wrap: nowrap;
    gap: var(--gs-space-3);
  }
  /* The device's name in the form's label column, so its unit menu lines up
     with the controls of the rows around it (Start up from, Model…). */
  .device-row .item-name {
    flex: none;
    width: var(--gs-form-label-width);
    min-width: 0;
  }
  .device-row :global(.unit-select) {
    flex: none;
  }
  .device-row :global(.media-select) {
    flex: 1 1 0;
    min-width: 0;
  }
  .item-pos,
  .item-status {
    color: var(--gs-text-muted);
  }
  .item-note {
    margin: 0;
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-sm);
  }
  .form-actions {
    display: flex;
    justify-content: flex-end;
    gap: var(--gs-space-2);
    margin-top: var(--gs-space-4);
  }
</style>
