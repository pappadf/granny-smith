<script lang="ts">
  import { onMount } from 'svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator, opfs, gsEval, whenModuleReady } from '@/bus';
  import { pickAndUploadAs } from '@/bus/upload';
  import type { MediaTypeId } from '@/lib/media';
  import { DEFAULT_CONFIG } from '@/lib/machine';
  import type { ImageCategory, OpfsEntry } from '@/bus/types';
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
    // The machine's own built-in video, when it is not a NuBus pseudo-card
    // (the PDM family's Ariel scanout).  Present means the display can be
    // driven EITHER by this port or by a NuBus card, and picking a card
    // leaves this port unconnected — exactly what plugging the monitor into
    // the card does on real hardware.  Absent (empty map) on every other
    // machine, so nothing else changes shape.
    builtin_video?: { id?: string; display_name?: string };
    // Per-card video slot shape (proposal §4.4) — the single source for both
    // the VROM requirement (per-card requires_vrom; see needsVrom) and the
    // video-mode list (each card's monitors × depths; see videoModes).
    // Stage 2 of the computed-card-compatibility proposal emits EVERY
    // declared socket here (all offering the same computed card list); the
    // dialog configures the first entry only (see configSlot).
    video_slots?: Array<{
      slot: string;
      fixed: boolean;
      default_card: string;
      cards: Array<{
        id: string;
        display_name?: string;
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
    // PCI sockets, same shape idea as video_slots but for the PCI bus: each
    // declared socket carries the cards the registry says fit it, and each
    // card carries the UI grouping hint `class` ('display', 'other', ...)
    // plus its own requires_prom.  A machine with no PCI bus omits this.
    //
    // The 9500 is the first machine whose display can ONLY come from here:
    // its builtin_video and video_slots are both empty, so without a
    // display-class PCI card there is nothing to draw on.
    pci_slots?: Array<{
      slot: number;
      label?: string;
      // Soldered down rather than a socket — not a user choice.
      fixed: boolean;
      // ...and a fixed slot that is only a STAND-IN: it exists solely
      // because no socket has supplied a card of its class yet, because
      // the real machine has nothing there.  The Power Macintosh 9500 is
      // the case this exists for: its Control/Chaos entry is emulator
      // scaffolding, not hardware, and calling it "on-board video" would
      // state the opposite of what the machine is.
      fallback?: boolean;
      default_card?: string;
      cards: Array<{
        id: string;
        display_name?: string;
        class?: string;
        requires_prom?: boolean;
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

  // One identified VROM in OPFS: which card it provides (probed via
  // vrom.identify) so the dialog can speak in cards, not filenames. Any
  // human-readable label comes from the card id via machine.profile
  // (cardOptions) — the on-disk name is a content hash and never shown.
  interface VromEntry {
    path: string;
    cardId: string; // nubus card-kind id this blob provides
    compatible: string[]; // card ids this vROM can drive (usually [cardId])
  }

  // One identified PCI expansion ROM in OPFS: which card it provides
  // (probed via prom.identify).  The sibling of VromEntry, kept separate for
  // the same reason the two core modules are separate — a vROM and a PROM
  // are different objects with different identity rules, and a card asks for
  // one or the other, never "a ROM".
  interface PromEntry {
    path: string;
    cardId: string; // PCI card-kind id this blob provides
    compatible: string[];
  }

  // The pseudo card-id standing for "the machine's own built-in video port".
  // Never a registry card id, so it can share the `cardId` state without
  // colliding; the boot document turns it into "no video_card, monitor
  // connected", and any real card into "that card, built-in port
  // unconnected".
  const BUILTIN_VIDEO_ID = 'builtin';

  // Local form state.
  let modelId = $state('');
  let cardId = $state(''); // selected NuBus video card-kind id
  let ram = $state(DEFAULT_CONFIG.ram);
  let romPath = $state('');
  let floppies = $state<string[]>([]);
  let hd = $state(NONE_SENTINEL);
  let cd = $state(NONE_SENTINEL);
  let videoMode = $state('');

  // Discovery state.
  let scanning = $state(true);
  let allRoms = $state<RomEntry[]>([]);
  // VROMs in OPFS, each identified to the card it provides.
  let allVroms = $state<VromEntry[]>([]);
  // PCI expansion ROMs in OPFS, likewise.
  let allProms = $state<PromEntry[]>([]);
  // model id -> profile, populated lazily via gsEval('machine.profile').
  let profiles = $state<Record<string, MachineProfile>>({});
  // model id -> ROMs that boot this model.
  let romsByModel = $derived.by(() => {
    const out: Record<string, RomEntry[]> = {};
    for (const r of allRoms) {
      // A repeated id in `compatible` would list the same ROM twice under one
      // model — the ROM picker keys its options by path, so keep it unique.
      for (const id of new Set(r.compatible)) {
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
  // --- Video card selection (card-driven; the vROM is auto-resolved). ------
  // The dialog speaks in *cards* (Apple Macintosh Display Card 24AC), not vROM
  // filenames. The available cards + their requires_vrom / monitors come from
  // machine.profile (the core owns this); each uploaded vROM is probed to the
  // card it provides (vrom.identify → card_id), so we only offer cards whose
  // vROM is actually present, and set machine.nubus.video_card at boot.

  // card-id -> the OPFS VROM files that provide it.
  let vromsByCardId = $derived.by(() => {
    const out: Record<string, VromEntry[]> = {};
    for (const v of allVroms) (out[v.cardId] ??= []).push(v);
    return out;
  });
  // The slot this dialog configures: the FIRST video_slots entry.  Machines
  // now declare every socket (stage 2 of the computed-card-compatibility
  // proposal), all offering the same computed card list — the single picker
  // drives the first one via the machine.nubus.video_card first-socket
  // alias; a per-socket UI is future work.  On builtin-first machines
  // (SE/30, IIci, IIsi) the first entry is the fixed built-in video, which
  // keeps their no-picker/vROM-row behavior exactly as before.
  let configSlot = $derived((currentProfile?.video_slots ?? [])[0]);
  let slotCards = $derived(configSlot?.cards ?? []);
  // The slot's default card id (the C-side default pick).
  let defaultCardId = $derived(configSlot?.default_card ?? '');
  // A card is offerable iff it needs no vROM (builtin) or its vROM is present.
  let availableCards = $derived(
    slotCards.filter((c) => !c.requires_vrom || (vromsByCardId[c.id]?.length ?? 0) > 0),
  );
  let cardOptions = $derived(
    availableCards.map((c) => ({ id: c.id, label: c.display_name ?? c.id })),
  );
  // The machine's own built-in video, offered beside the NuBus cards when
  // the profile advertises one.  BUILTIN_VIDEO_ID is a card id no registry
  // card can use, so it round-trips through the same `cardId` state.
  let builtinVideo = $derived(currentProfile?.builtin_video);
  let hasBuiltinVideo = $derived(!!builtinVideo?.display_name);

  // --- Display-class PCI cards --------------------------------------------
  // card-id -> the OPFS PROM files that provide it.
  let promsByCardId = $derived.by(() => {
    const out: Record<string, PromEntry[]> = {};
    for (const p of allProms) (out[p.cardId] ??= []).push(p);
    return out;
  });
  // Every distinct display-class card offered by any PCI socket.  Sockets
  // are deduplicated by card id: the machine declares six of them and they
  // all offer the same computed list, so the picker would otherwise show
  // the same card six times.
  let pciDisplayCards = $derived.by(() => {
    const out: NonNullable<MachineProfile['pci_slots']>[number]['cards'] = [];
    for (const slot of currentProfile?.pci_slots ?? []) {
      // SOCKETS only. A fixed slot's card is soldered down and can never be
      // staged into a socket — the core's own pci_card_fits_socket refuses
      // it — so offering it here would put a choice in the picker that the
      // boot path is guaranteed to reject.
      if (slot.fixed) continue;
      for (const c of slot.cards ?? []) {
        if (c.class !== 'display' || out.some((seen) => seen.id === c.id)) continue;
        out.push(c);
      }
    }
    return out;
  });

  // A soldered-down display-class PCI card IS this machine's built-in
  // video, and is named like it — unless it is a stand-in, which is not
  // the machine's hardware and must not be presented as though it were.
  let pciBuiltinDisplay = $derived(
    (currentProfile?.pci_slots ?? [])
      .filter((slot) => slot.fixed && !slot.fallback)
      .flatMap((slot) => slot.cards ?? [])
      .find((c) => c.class === 'display'),
  );
  // A PCI card is offerable iff it needs no expansion ROM or one is present.
  // Unlike a NuBus vROM this is not a soft preference: the core refuses the
  // boot outright (requires_prom + strict resolution), so offering the card
  // without its ROM would just produce a rejected boot.
  let availablePciCards = $derived(
    pciDisplayCards.filter((c) => !c.requires_prom || (promsByCardId[c.id]?.length ?? 0) > 0),
  );
  let pciCardOptions = $derived(
    availablePciCards.map((c) => ({ id: c.id, label: c.display_name ?? c.id })),
  );
  // What the display picker offers, in the order a machine presents itself:
  // its own built-in port first, then anything soldered to the PCI bus that
  // amounts to built-in video, then every installable NuBus card, then every
  // installable display-class PCI card.
  //
  // This follows the NuBus-only machines rather than inventing a shape.  A
  // IIx / IIcx / IIfx has builtin_video {} and a non-fixed video slot: the
  // dialog shows a "Display Card" picker and, when no vROM is present, says
  // the card needs one.  A IIci has a FIXED video slot holding its soldered
  // RBV video and offers no choice.  The PCI machines are the same two
  // cases: a 7500/8500's Control is soldered (fixed), a 9500's sockets are
  // sockets — so the 9500 behaves exactly like a IIfx, with a .prom in the
  // place of a .vrom.
  let displayOptions = $derived([
    ...(hasBuiltinVideo
      ? [{ id: BUILTIN_VIDEO_ID, label: builtinVideo?.display_name ?? 'Built-in video' }]
      : []),
    ...(pciBuiltinDisplay
      ? [
          {
            id: pciBuiltinDisplay.id,
            label: pciBuiltinDisplay.display_name ?? pciBuiltinDisplay.id,
          },
        ]
      : []),
    ...cardOptions,
    ...pciCardOptions,
  ]);
  let builtinSelected = $derived(cardId === BUILTIN_VIDEO_ID);
  // Whether the machine already has a screen without the user installing
  // anything.  A stand-in fallback deliberately does NOT count: the 9500 has
  // no on-board video, and the emulator's Control/Chaos stand-in exists so a
  // cardless boot has somewhere to draw, not so the dialog can claim the
  // machine has video it never shipped with.
  let hasSolderedDisplay = $derived(hasBuiltinVideo || !!pciBuiltinDisplay);
  // Is the current pick a PCI card rather than a NuBus one?  The two travel
  // in different boot-document fields (pci_card= vs video_card=), so this
  // decides which one is filled in.
  let selectedPciCard = $derived(availablePciCards.find((c) => c.id === cardId));
  let pciSelected = $derived(!!selectedPciCard);
  // The expansion ROM handed to the core for the selected PCI card.  As with
  // the vROM, an explicit pick is preferred over letting the offer registry
  // content-match, so the user sees the file they uploaded actually used.
  let resolvedProm = $derived(
    selectedPciCard?.requires_prom ? (promsByCardId[cardId]?.[0] ?? null) : null,
  );
  // Only surface the picker when there's a real choice; a fixed/builtin
  // single card (e.g. SE/30 onboard video) needs no dropdown.  A machine
  // whose only display source is one expansion card is still a choice worth
  // showing — it is the only place the screen's provenance is stated — so
  // the single PCI option counts.
  let needsCardPicker = $derived(displayOptions.length > 1 || pciSelected);
  let selectedCard = $derived(availableCards.find((c) => c.id === cardId));
  // VROM row/handling is driven by the *selected card* (the SE/30-vs-IIci
  // asymmetry): a card declares requires_vrom, not the machine.
  let needsVrom = $derived(selectedCard?.requires_vrom === true);
  // The vROM file handed to the core for the selected card (an explicit
  // machine.vrom.load — the preferred offer); it also gates "is this card
  // installable". Without it the card factory falls back to whatever the
  // platform offered from the OPFS store (content-matched).
  let resolvedVrom = $derived(needsVrom ? (vromsByCardId[cardId]?.[0] ?? null) : null);
  // Model expects a video card but none is installable (every candidate card
  // needs a vROM and none is present). Drives the "upload a Video ROM" hint.
  // ...one sentence for both buses, because it is one situation: the machine
  // can take a display card, none is installable, and it has nothing
  // soldered to fall back on.  Which ROM to ask for is the only difference,
  // and that is decided by which bus had the candidates.
  let videoUnavailable = $derived(
    !hasSolderedDisplay &&
      displayOptions.length === 0 &&
      (slotCards.length > 0 || pciDisplayCards.length > 0),
  );
  // A PCI expansion ROM (.prom) and a NuBus video ROM (.vrom) are different
  // files from different places; naming the wrong one sends the user hunting
  // for something that would not help.
  let missingRomKind = $derived(
    slotCards.length > 0 && availableCards.length === 0 ? 'Video ROM' : 'PCI expansion ROM',
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
  // Video-mode list for the *selected card*: its monitors × supported depths.
  // Ids/labels match what the C side emits ("<monitor>_<depth>bpp"), so the
  // boot-time `machine.nubus.video_mode` seed is unchanged.
  let videoModes = $derived.by(() => {
    const out: Array<{ id: string; label: string }> = [];
    for (const m of (selectedCard ?? selectedPciCard)?.monitors ?? []) {
      for (const d of m.depths ?? []) {
        out.push({
          id: `${m.id}_${d}bpp`,
          label: `${m.name ?? m.id} · ${m.width}×${m.height} · ${d} bpp`,
        });
      }
    }
    return out;
  });

  let fdOptions = $state<string[]>([NONE_SENTINEL]);
  let hdOptions = $state<string[]>([NONE_SENTINEL]);
  let cdOptions = $state<string[]>([NONE_SENTINEL]);
  // Selected filename -> the OPFS path it actually came from. The dropdowns
  // speak in filenames, but a category's listing is not always one directory:
  // scanImages('fd') folds in the legacy /opfs/images/fdhd/ (see
  // BrowserOpfs.scanImages), so `/opfs/images/<cat>/<name>` is not a safe way
  // to reconstruct the path at submit time. Filled by refreshOpfs.
  let fdPaths = $state<Record<string, string>>({});
  let hdPaths = $state<Record<string, string>>({});
  let cdPaths = $state<Record<string, string>>({});

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
    // rom.identify returns a native object (V_MAP) — no inner JSON.parse.
    const r = await gsEval('machine.rom.identify', [path]);
    if (!r || typeof r !== 'object' || 'error' in r) return null;
    const parsed = r as {
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
  }

  // Probe one VROM file to the card it provides, mirroring identifyRom. The
  // core (vrom.identify) owns the vROM→card mapping; the UI carries none.
  async function identifyVrom(path: string): Promise<VromEntry | null> {
    // vrom.identify returns a native object (V_MAP) — no inner JSON.parse.
    const r = await gsEval('machine.vrom.identify', [path]);
    if (!r || typeof r !== 'object' || 'error' in r) return null;
    const parsed = r as {
      recognised?: boolean;
      card_id?: string;
      compatible?: string[];
    };
    if (!parsed.recognised || !parsed.card_id) return null;
    return {
      path,
      cardId: parsed.card_id,
      compatible: Array.isArray(parsed.compatible) ? parsed.compatible : [parsed.card_id],
    };
  }

  // Probe one PCI expansion ROM to the card it provides.  The sibling of
  // identifyVrom, against the sibling core registry.
  async function identifyProm(path: string): Promise<PromEntry | null> {
    // prom.identify returns a native object (V_MAP) — no inner JSON.parse.
    const r = await gsEval('machine.prom.identify', [path]);
    if (!r || typeof r !== 'object' || 'error' in r) return null;
    const parsed = r as {
      recognised?: boolean;
      card_id?: string;
      compatible?: string[];
    };
    if (!parsed.recognised || !parsed.card_id) return null;
    return {
      path,
      cardId: parsed.card_id,
      compatible: Array.isArray(parsed.compatible) ? parsed.compatible : [parsed.card_id],
    };
  }

  async function resolveProfile(id: string): Promise<MachineProfile> {
    if (profiles[id]) return profiles[id];
    // machine.profile returns a native nested object — no inner JSON.parse.
    const r = await gsEval('machine.profile', [id]);
    let parsed: MachineProfile = {};
    if (r && typeof r === 'object' && !('error' in r)) parsed = r as MachineProfile;
    profiles = { ...profiles, [id]: parsed };
    return parsed;
  }

  // Collapse a category listing to the unique filenames the dropdown offers,
  // plus the path each name resolves to. A listing can carry the same name
  // twice — scanImages('fd') concatenates /opfs/images/fd/ with the legacy
  // /opfs/images/fdhd/ — and the first (canonical) entry wins. Deduping here
  // is what keeps the option list free of repeats; the dropdowns identify an
  // option by its text, so a repeat is both ambiguous to the user and (until
  // the {#each} keys below were changed) fatal to the render.
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
      const [roms, vroms, proms, fds, hds, cds] = await Promise.all([
        opfs.scanRoms().catch(() => []),
        opfs.scanImages('vrom').catch(() => []),
        opfs.scanImages('prom').catch(() => []),
        opfs.scanImages('fd').catch(() => []),
        opfs.scanImages('hd').catch(() => []),
        opfs.scanImages('cd').catch(() => []),
      ]);

      // Identify every ROM in parallel. Drop the unrecognised ones.
      const identified = (await Promise.all(roms.map((r) => identifyRom(r.path)))).filter(
        (e): e is RomEntry => e !== null,
      );
      allRoms = identified;

      // Identify every VROM to the card it provides (drop unrecognised). The
      // card picker is then built from machine.profile filtered to these.
      allVroms = (await Promise.all(vroms.map((v) => identifyVrom(v.path)))).filter(
        (e): e is VromEntry => e !== null,
      );

      // ...and every PCI expansion ROM, which is what makes a display-class
      // PCI card offerable at all.
      allProms = (await Promise.all(proms.map((p) => identifyProm(p.path)))).filter(
        (e): e is PromEntry => e !== null,
      );

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

      const fd = mediaOptions(fds);
      const hdm = mediaOptions(hds);
      const cdm = mediaOptions(cds);
      fdPaths = fd.paths;
      hdPaths = hdm.paths;
      cdPaths = cdm.paths;
      fdOptions = [NONE_SENTINEL, ...fd.names, UPLOAD_SENTINEL, CREATE_SENTINEL];
      hdOptions = [NONE_SENTINEL, ...hdm.names, UPLOAD_SENTINEL, CREATE_SENTINEL];
      cdOptions = [NONE_SENTINEL, ...cdm.names, UPLOAD_SENTINEL];
    } finally {
      // Never leave the dialog pinned on "Scanning ROMs…": a rejected scan
      // has to surface as an empty inventory the user can act on, not as a
      // spinner that outlives the page.
      scanning = false;
    }
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

  // Keep cardId valid for the current model: prefer the slot's default card,
  // else the first installable one. The picker may be hidden (single card),
  // so this is what submit relies on.
  //
  // This works off displayOptions — the UNION of built-in video, NuBus
  // cards and display-class PCI cards — not just the NuBus list. On a
  // machine whose only display comes from a PCI socket the union is the
  // one-element list holding that card, and if this effect ignored it the
  // dialog would silently boot with no card at all.
  $effect(() => {
    const list = displayOptions;
    const builtin = hasBuiltinVideo;
    if (builtin && cardId === BUILTIN_VIDEO_ID) return; // a valid pick
    if (!list.length) {
      // Built-in video is the fallback when no card is installable, and the
      // default on machines that have it: a stock machine ships no card.
      cardId = builtin ? BUILTIN_VIDEO_ID : '';
    } else if (!list.find((c) => c.id === cardId)) {
      cardId = builtin
        ? BUILTIN_VIDEO_ID
        : (list.find((c) => c.id === defaultCardId)?.id ?? list[0].id);
    }
  });

  // Keep videoMode valid for the selected card (resets on model or card change).
  $effect(() => {
    const list = videoModes;
    if (!list.find((m) => m.id === videoMode)) {
      videoMode = list[0]?.id ?? '';
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
    // cardId / videoMode follow the card-selection effects above.
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
    // The chosen card auto-resolves its vROM (probed by card id); '(auto)'
    // means no explicit pick — the card factory content-matches among the
    // files the platform offered from the OPFS store.
    const vromPath = resolvedVrom ? resolvedVrom.path : '(auto)';
    // Same contract for a PCI card's expansion ROM: an explicit pick when we
    // have one, otherwise let the core's offer registry content-match.
    const promPath = resolvedProm ? resolvedProm.path : '(auto)';
    // Resolve each pick back to the path it was scanned from (fdPaths etc.);
    // the category directory is only the fallback, since an fd listing can
    // also carry files from the legacy fdhd directory.
    const floppyPaths = floppies.map((f) =>
      f === NONE_SENTINEL || !f ? '' : (fdPaths[f] ?? `/opfs/images/fd/${f}`),
    );
    const hdPath = hd === NONE_SENTINEL ? NONE_SENTINEL : (hdPaths[hd] ?? `/opfs/images/hd/${hd}`);
    const cdPath = cd === NONE_SENTINEL ? NONE_SENTINEL : (cdPaths[cd] ?? `/opfs/images/cd/${cd}`);
    // A fixed builtin video slot (IIci / IIsi) hard-wires its card and has
    // no C-side video-mode catalog — the boot document carries neither
    // field for it (boot validation rejects unknown mode ids).
    const fixedVideo = configSlot?.fixed === true;
    // Card configurability and mode-catalog presence are INDEPENDENT: the
    // SE/30's builtin slot is now card-configurable (generic vs real vROM)
    // yet still has a single fixed 1-bpp mode with no C-side catalog.  So
    // gate video_mode on there being an actual choice (more than one mode —
    // the same condition that shows the picker), not on `fixedVideo`; else
    // the lone auto-selected `se30_internal_1bpp` id is sent and boot
    // validation rejects it as unknown.
    const hasVideoModeChoice = videoModes.length > 1;
    await initEmulator({
      model: modelId,
      modelName,
      rom: selected.path,
      vrom: vromPath,
      // The selected NuBus video card — the boot document's video_card=, so
      // the right card boots instead of the slot default (the 24AC-vs-8•24 bug).
      // Only a NuBus pick travels here.  `selectedCard` is looked up in the
      // NuBus list, so built-in video, a socket PCI card and a soldered PCI
      // card all miss it — which is the point: sending any of those as
      // video_card would make the core hunt for a NuBus card that does not
      // exist.  (An earlier version tested for those three cases one by one
      // and missed the soldered PCI card, which then went out as
      // video_card=tnt_control.)
      videoCard: fixedVideo || !selectedCard ? undefined : cardId || undefined,
      // A display-class PCI card travels in its own field: the boot document
      // seats it into the first free socket, and the machine's BUILTIN
      // fallback video (the 9500's Control/Chaos stand-in) retires because a
      // socket supplied a display card.
      pciCard: pciSelected ? cardId : undefined,
      prom: pciSelected ? promPath : undefined,
      // Which port the monitor is plugged into.  Choosing a NuBus card on a
      // machine that also has built-in video leaves the built-in port
      // unconnected, so the ROM turns built-in video off and the card is the
      // only screen — the hardware behaviour, and the only way the card's
      // own accelerator ever gets used.
      monitor: hasBuiltinVideo && !builtinSelected ? 'none' : undefined,
      // Seed the selected video mode (matches web-legacy's bootFromConfig).
      // Without it the card never seeds its slot-PRAM/video defaults and A/UX
      // hangs enabling its device drivers on real hardware.
      videoMode: fixedVideo || !hasVideoModeChoice ? undefined : videoMode || undefined,
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
      {#if needsCardPicker}
        <div class="form-row">
          <label for="cfg-card">{hasBuiltinVideo ? 'Display' : 'Display Card'}</label>
          <select id="cfg-card" bind:value={cardId}>
            {#each displayOptions as c (c.id)}
              <option value={c.id}>{c.label}</option>
            {/each}
          </select>
        </div>
        {#if hasBuiltinVideo && !builtinSelected}
          <div class="form-row">
            <span class="form-label"></span>
            <div class="form-help">
              The monitor is plugged into the card, so the built-in video port is left unconnected
              and the card becomes the only screen.
            </div>
          </div>
        {/if}
        {#if pciSelected && !hasSolderedDisplay}
          <div class="form-row">
            <span class="form-label"></span>
            <div class="form-help">
              This model has no built-in video, so the card in the expansion slot is the screen.
            </div>
          </div>
        {/if}
      {/if}
      {#if videoUnavailable}
        <div class="form-row">
          <span class="form-label">Display Card</span>
          <div class="form-help">
            This model's display card needs a {missingRomKind}. Drag-and-drop one (or add it via the
            Images panel) to enable video.
          </div>
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
          {#each ramOptions as opt, i (i)}
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
            {#each fdOptions as opt, oi (oi)}
              <option>{opt}</option>
            {/each}
          </select>
        </div>
      {/each}
      <div class="form-row">
        <label for="cfg-hd">{hdSlotLabel}</label>
        <select id="cfg-hd" value={hd} onchange={onHdChange}>
          {#each hdOptions as opt, i (i)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      {#if hasCdrom}
        <div class="form-row">
          <label for="cfg-cd">SCSI CD-ROM</label>
          <select id="cfg-cd" value={cd} onchange={onCdChange}>
            {#each cdOptions as opt, i (i)}
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
