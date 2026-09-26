<script lang="ts">
  import { onMount, tick } from 'svelte';
  import { setWelcomeSlide } from '@/state/layout.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { initEmulator, opfs, gsEval, whenModuleReady } from '@/bus';
  import { pickAndUploadAs } from '@/bus/upload';
  import { getProfile, type MachineProfile } from '@/bus/profile';
  import {
    identifyRom,
    identifyCardRom,
    type MediaTypeId,
    type RomIdentity,
    type CardRomIdentity,
  } from '@/lib/media';
  import { formatRamKb } from '@/lib/machine';
  import type { ImageCategory, OpfsEntry } from '@/bus/types';
  import { images } from '@/state/images.svelte';
  import CreateImageDialog from './CreateImageDialog.svelte';

  const UPLOAD_SENTINEL = 'Upload image...';
  const CREATE_SENTINEL = 'Create blank image...';
  const NONE_SENTINEL = '(none)';

  // The model's configuration shape is bus/profile.ts's MachineProfile.
  //
  // One identified ROM in OPFS: the model ids it lights up (rom.identify).
  type RomEntry = RomIdentity;
  // One identified VROM in OPFS: which card it provides (vrom.identify), so
  // the dialog speaks in cards, not filenames — the on-disk name is a content
  // hash and never shown.
  type VromEntry = CardRomIdentity;
  // One identified PCI expansion ROM (prom.identify).  The sibling of
  // VromEntry: a vROM and a PROM are different objects with different
  // identity rules, and a card asks for one or the other, never "a ROM".
  type PromEntry = CardRomIdentity;

  // The pseudo card-id standing for "the machine's own built-in video port".
  // Never a registry card id, so it can share the `cardId` state without
  // colliding; the boot document turns it into "no video_card, monitor
  // connected", and any real card into "that card, built-in port
  // unconnected".
  const BUILTIN_VIDEO_ID = 'builtin';

  // Local form state.
  let modelId = $state('');
  let cardId = $state(''); // selected NuBus video card-kind id
  // RAM in KB, one of the profile's ram_options; 0 = the model's own default.
  let ramKb = $state(0);
  let romPath = $state('');
  let floppies = $state<string[]>([]);
  let hd = $state(NONE_SENTINEL);
  let cd = $state(NONE_SENTINEL);
  let videoMode = $state('');
  // Chosen PCI card options, keyed by option key ("vram" -> "4m"). Cleared
  // whenever the selected card changes, since the keys are the card's.
  let pciOptions = $state<Record<string, string>>({});

  // Discovery state.
  let scanning = $state(true);
  let startError = $state<string | null>(null); // the emulator failed to start
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
  // The options the selected PCI card declares. Only a socket card can take
  // them: a soldered one is not staged, so there is nothing to attach an
  // option to.  A display pick and an expansion pick are mutually
  // exclusive holders of the wildcard socket, so exactly one of them
  // supplies the option list (activePciCard, defined with the expansion
  // picker below).
  // Every PCI SOCKET the machine declares, for the Expansion Slots list.
  // Sockets only — a fixed slot is soldered-down hardware, not a slot the
  // user populates.
  let pciSockets = $derived((currentProfile?.pci_slots ?? []).filter((sl) => !sl.fixed));
  // Every distinct NON-display card the sockets offer (the Voodoo2's
  // class is "3d": a pass-through card that must never be the machine's
  // display, so the Display picker rightly never lists it).  Deduplicated
  // across sockets exactly like pciDisplayCards.
  let pciExpansionCards = $derived.by(() => {
    const out: NonNullable<MachineProfile['pci_slots']>[number]['cards'] = [];
    for (const slot of currentProfile?.pci_slots ?? []) {
      if (slot.fixed) continue;
      for (const c of slot.cards ?? []) {
        if (c.class === 'display' || out.some((seen) => seen.id === c.id)) continue;
        if (c.requires_prom && (promsByCardId[c.id]?.length ?? 0) === 0) continue;
        out.push(c);
      }
    }
    return out;
  });
  // The one expansion pick this dialog can express: machine.boot's
  // pci_card= reaches the FIRST socket, the same wildcard field a
  // display-class PCI pick travels in — so the two picks are mutually
  // exclusive, and the Display picker's choice wins the socket.
  let expansionCardId = $state('');
  let selectedExpansionCard = $derived(pciExpansionCards.find((c) => c.id === expansionCardId));
  let expansionSelected = $derived(!pciSelected && !!selectedExpansionCard);
  // Whichever card actually occupies the wildcard socket.
  let activePciCard = $derived(
    selectedPciCard ?? (expansionSelected ? selectedExpansionCard : undefined),
  );
  let pciCardOptions_ = $derived(activePciCard?.options ?? []);
  // machine.boot takes one wildcard card for the FIRST socket, so that is
  // the only one this dialog can fill; the rest are shown as empty. A
  // per-socket picker needs a per-slot boot field that does not exist yet.
  let firstSocketLabel = $derived(pciSockets[0]?.label ?? '');
  // "key=value,key=value" for machine.boot, omitting anything left at the
  // card's own default so the boot record does not claim a choice the user
  // did not make.
  let pciOptionSpec = $derived(
    pciCardOptions_
      .map((o) => [o.key, pciOptions[o.key] ?? o.default_value ?? ''] as const)
      .filter(([, v], i) => v && v !== (pciCardOptions_[i].default_value ?? ''))
      .map(([k, v]) => `${k}=${v}`)
      .join(','),
  );

  // The expansion ROM handed to the core for the selected PCI card.  As with
  // the vROM, an explicit pick is preferred over letting the offer registry
  // content-match, so the user sees the file they uploaded actually used.
  let resolvedProm = $derived(
    activePciCard?.requires_prom ? (promsByCardId[activePciCard.id]?.[0] ?? null) : null,
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
  // The model's hard-disk bays, as the core derives them (profile.hd_bays):
  // the boot bay first, each on whatever bus it is — SCSI, a Network
  // Server's second channel, the Lisa's ProFile.  The dialog picks an index;
  // the core attaches there (machine.attach_hd), so nothing here knows a bus.
  let hdSlots = $derived(currentProfile?.hd_bays ?? []);
  let hdSlotLabel = $derived(hdSlots[0]?.label ?? 'Hard disk');
  // Which bay, by index; 0 (the boot bay) until the user picks another.
  // Reset whenever the model changes, since the bays are its.
  let hdBay = $state(0);
  $effect(() => {
    void modelId;
    hdBay = 0;
  });
  // Only machines whose profile advertises a CD-ROM (has_cdrom) show the CD row.
  let hasCdrom = $derived(currentProfile?.has_cdrom === true);
  // RAM choices in KB, labelled for display (the value stays a number, F-01).
  let ramOptions = $derived(currentProfile?.ram_options ?? []);
  let floppySlots = $derived(currentProfile?.floppy_slots ?? []);
  // Video-mode list for the *selected card*: its monitors × supported depths.
  // Ids/labels match what the C side emits ("<monitor>_<depth>bpp"), so the
  // boot-time `machine.nubus.video_mode` seed is unchanged.
  //
  // NuBus cards ONLY.  A PCI display card's monitor list is real, but the
  // boot document has no field that carries it: video_mode is validated
  // against the NuBus catalog (nubus_video_mode_known), so sending a PCI
  // card's mode id fails the boot outright — "unknown video-mode id
  // '14in_rgb_8bpp'".  The card does take a `monitor` staged option, but
  // nothing reaches pci_staged_option_set from a boot document yet, so
  // there is no honest way to offer the choice.  Until there is, the row
  // stays hidden for a PCI pick and the card senses its default monitor.
  let videoModes = $derived.by(() => {
    const out: Array<{ id: string; label: string }> = [];
    for (const m of selectedCard?.monitors ?? []) {
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

  async function resolveProfile(id: string): Promise<void> {
    if (profiles[id]) return;
    const p = await getProfile(id);
    if (p) profiles = { ...profiles, [id]: p };
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
      const identified = (await Promise.all(roms.map((r) => identifyRom(gsEval, r.path)))).filter(
        (e): e is RomEntry => e !== null,
      );
      allRoms = identified;

      // Identify every VROM to the card it provides (drop unrecognised). The
      // card picker is then built from machine.profile filtered to these.
      allVroms = (
        await Promise.all(vroms.map((v) => identifyCardRom(gsEval, 'vrom', v.path)))
      ).filter((e): e is VromEntry => e !== null);

      // ...and every PCI expansion ROM, which is what makes a display-class
      // PCI card offerable at all.
      allProms = (
        await Promise.all(proms.map((p) => identifyCardRom(gsEval, 'prom', p.path)))
      ).filter((e): e is PromEntry => e !== null);

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

  // An expansion pick survives only while its card is still offered (a
  // model change rebuilds the socket list).
  $effect(() => {
    if (expansionCardId && !pciExpansionCards.find((c) => c.id === expansionCardId))
      expansionCardId = '';
  });

  // The option keys belong to the selected card, so a card change starts
  // them over rather than carrying a stale key into a different card.
  $effect(() => {
    const keys = pciCardOptions_.map((o) => o.key).join('|');
    void keys;
    pciOptions = {};
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
    ramKb = currentProfile.ram_default || (ramOptions[0] ?? 0);
    floppies = new Array<string>(floppySlots.length).fill(NONE_SENTINEL);
    // Media picked for another model are not this one's (and this one may
    // have no CD bay at all): the CD used to stay selected, hidden, and be
    // attached anyway (N-03).
    hd = NONE_SENTINEL;
    cd = NONE_SENTINEL;
    // cardId / videoMode follow the card-selection effects above.
  });

  onMount(() => {
    void (async () => {
      try {
        await whenModuleReady();
      } catch (e) {
        // No emulator: nothing can be scanned or booted — say so instead of
        // leaving the dialog on "Scanning ROMs…" forever.
        startError = e instanceof Error ? e.message : String(e);
        scanning = false;
        return;
      }
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

  // The slot's new value for a dropdown pick: the pick itself, or for
  // "Upload image…" the uploaded image -- or, when the upload was cancelled
  // or rejected, the previous value.  In that last case the state does not
  // change, so Svelte leaves the DOM showing the sentinel: write the
  // <select>'s value back ourselves.  (A cancel used to wipe the slot, and a
  // successful upload left it at (none), N-53.)
  async function interceptIfUpload(
    select: HTMLSelectElement,
    previous: string,
    category: ImageCategory,
  ): Promise<string> {
    const value = select.value;
    if (value !== UPLOAD_SENTINEL) return value;
    // Map the dropdown's category (uses 'cd' as the ImageCategory key)
    // to the upload pipeline's MediaTypeId ('cdrom') and pick strictly:
    // a file uploaded into the floppy slot must validate AS a floppy
    // or it's rejected. Prevents accidentally classifying an HD image
    // as a floppy via the auto-detect order.
    const mediaId: MediaTypeId = category === 'cd' ? 'cdrom' : (category as MediaTypeId);
    const persisted = await pickAndUploadAs(mediaId);
    await refreshOpfs();
    await tick(); // the new option is in the DOM before the value names it
    const paths = category === 'fd' ? fdPaths : category === 'hd' ? hdPaths : cdPaths;
    const name = persisted ? Object.keys(paths).find((n) => paths[n] === persisted) : undefined;
    const next = name ?? previous;
    select.value = next;
    return next;
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
    const select = e.target as HTMLSelectElement;
    const result = await interceptIfUpload(select, floppies[slotIndex] ?? NONE_SENTINEL, 'fd');
    const next = floppies.slice();
    next[slotIndex] = result;
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
    hd = await interceptIfUpload(e.target as HTMLSelectElement, hd, 'hd');
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
    cd = await interceptIfUpload(e.target as HTMLSelectElement, cd, 'cd');
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
      // ...or a non-display socket card (the Voodoo2) from the Expansion
      // Slots picker — the SAME wildcard field, so the two picks are
      // mutually exclusive and the display pick wins the socket.
      pciCard: pciSelected ? cardId : expansionSelected ? expansionCardId : undefined,
      prom: pciSelected ? promPath : undefined,
      pciOption: (pciSelected || expansionSelected) && pciOptionSpec ? pciOptionSpec : undefined,
      // Which port the monitor is plugged into.  Choosing a NuBus card on a
      // machine that also has built-in video leaves the built-in port
      // unconnected, so the ROM turns built-in video off and the card is the
      // only screen — the hardware behaviour, and the only way the card's
      // own accelerator ever gets used.
      monitor: hasBuiltinVideo && !builtinSelected ? 'none' : undefined,
      // Seed the selected video mode (matches web-legacy's bootFromConfig).
      // Without it the card never seeds its slot-PRAM/video defaults and A/UX
      // hangs enabling its device drivers on real hardware.
      // Same rule as videoCard: only a NuBus pick has a video_mode the core
      // will accept, so gate on selectedCard rather than on the negations.
      videoMode:
        fixedVideo || !selectedCard || !hasVideoModeChoice ? undefined : videoMode || undefined,
      ramKb: ramKb || undefined,
      floppies: floppyPaths,
      hd: hdPath,
      hdBay,
      // Only a model with a CD bay gets a CD.
      cd: hasCdrom ? cdPath : NONE_SENTINEL,
    });
    setWelcomeSlide('home');
  }

  let canStart = $derived(!scanning && !!modelId && romsForCurrentModel.length > 0);
</script>

<div class="config-content">
  <a href="#back" class="back-link" onclick={onBack}>← Back</a>
  <h2 class="config-title">New Machine</h2>
  <form class="config-form" onsubmit={onSubmit}>
    {#if startError}
      <div class="form-row">
        <span class="form-label">Machine Model</span>
        <div class="form-help">The emulator did not start: {startError}</div>
      </div>
    {:else if scanning}
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
      {#if pciSockets.length > 0}
        <div class="form-divider"></div>
        <div class="form-row">
          <span class="form-label">Expansion Slots</span>
          <div class="slot-list">
            {#each pciSockets as sl (sl.slot)}
              <div class="slot-row">
                <span class="slot-name">{sl.label ?? `Slot ${sl.slot}`}</span>
                {#if sl.label === firstSocketLabel && !pciSelected && pciExpansionCards.length > 0}
                  <!-- The one per-socket pick the boot document can carry:
                       the wildcard pci_card= reaches the first socket.  Only
                       non-display cards appear here; a display-class card is
                       picked in the Display row and would occupy this same
                       socket, which is why the two are mutually exclusive. -->
                  <select
                    id="cfg-expansion-card"
                    value={expansionCardId}
                    onchange={(e) => (expansionCardId = (e.target as HTMLSelectElement).value)}
                  >
                    <option value="">(empty)</option>
                    {#each pciExpansionCards as c (c.id)}
                      <option value={c.id}>{c.display_name ?? c.id}</option>
                    {/each}
                  </select>
                {:else}
                  <span class="slot-card">
                    {sl.label === firstSocketLabel && pciSelected
                      ? (selectedPciCard?.display_name ?? cardId)
                      : '(empty)'}
                  </span>
                {/if}
              </div>
            {/each}
          </div>
        </div>
        {#if pciSockets.length > 1 && pciSelected}
          <div class="form-row">
            <span class="form-label"></span>
            <div class="form-help">
              A card is installed in the first socket. Filling the others needs a per-socket pick,
              which the boot document does not carry yet.
            </div>
          </div>
        {/if}
        {#each pciCardOptions_ as opt (opt.key)}
          <div class="form-row">
            <label for="cfg-pciopt-{opt.key}">{opt.label ?? opt.key}</label>
            <select
              id="cfg-pciopt-{opt.key}"
              value={pciOptions[opt.key] ?? opt.default_value ?? ''}
              onchange={(e) =>
                (pciOptions = { ...pciOptions, [opt.key]: (e.target as HTMLSelectElement).value })}
            >
              {#each opt.values ?? [] as v (v.id)}
                <option value={v.id}>{v.label ?? v.id}</option>
              {/each}
            </select>
          </div>
        {/each}
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
        <select id="cfg-ram" bind:value={ramKb}>
          {#each ramOptions as kb (kb)}
            <option value={kb}>{formatRamKb(kb)}</option>
          {:else}
            <option value={0}>Model default</option>
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
        <label for="cfg-hd">{hdSlots.length > 1 ? 'Hard disk' : hdSlotLabel}</label>
        <select id="cfg-hd" value={hd} onchange={onHdChange}>
          {#each hdOptions as opt, i (i)}
            <option>{opt}</option>
          {/each}
        </select>
      </div>
      {#if hdSlots.length > 1}
        <!-- Which bay it sits in: the firmware's default boot bay is preselected. -->
        <div class="form-row">
          <label for="cfg-hd-bay">Bay</label>
          <select id="cfg-hd-bay" bind:value={hdBay}>
            {#each hdSlots as slot, i (i)}
              <option value={i}>{slot.label}</option>
            {/each}
          </select>
        </div>
      {/if}
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
  bus={hdSlots[0]?.bus === 'profile' ? 'profile' : 'scsi'}
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
  .slot-list {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .slot-row {
    display: flex;
    gap: 8px;
    align-items: baseline;
  }
  .slot-name {
    min-width: 3em;
    opacity: 0.7;
  }
  .slot-card {
    opacity: 0.9;
  }

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
