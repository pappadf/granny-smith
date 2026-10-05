# Granny Smith documentation

This directory holds four classes of content, split by two questions.
**Would a document stay true, sentence for sentence, if Granny Smith did not
exist?** If yes, it is *reference* material and lives under
[`reference/`](reference/). If no, it is about Granny Smith — and a second
question applies: **is it about the implementation, or about using the
product?** Implementation lives under [`guide/`](guide) and
[`internals/`](internals) — the latter a mirror of `src/`; using the product
lives under [`user/`](user/).

| tree | class | holds |
|---|---|---|
| `guide/` | internals (C) | how to build, test and contribute |
| `internals/core/` | internals (C) | Granny Smith design, mirrors `src/core/` |
| `internals/machines/` | internals (C) | machine and chip models, mirrors `src/machines/` |
| `user/` | end-user (D) | how to use the emulator; no source checkout needed |
| `reference/hardware/` | reference (A) | cross-machine chips and buses |
| `reference/machines/` | reference (A/B) | one subtree per machine family: family doc, machine docs, device docs |
| `reference/protocols/` | reference (B) | wire protocols: AppleTalk, PAP, LaserWriter, AFP |
| `reference/formats/` | reference (B) | file and image formats: DiskCopy 4.2, NDIF, UDIF, Mac ROM, Mac PRAM |
| `reference/os/` | reference (B) | guest operating systems: A/UX, BeOS, Copland, MkLinux, Windows NT, Xenix |
| `articles/` | long-form | curated publication-quality write-ups |
| `notes/` | logs | dated investigation logs, frozen once written |
| `assets/` | — | images |

A reference doc describes **the hardware** — what the real chip, card or
machine does — never what Granny Smith does with it. No `src/` paths, no
object-model or shell surface, no checkpointing, no "we model this as…".
All of that belongs in `internals/core/` or `internals/machines/`, or in the
optional
"Granny Smith implementation notes" appendix most reference docs carry.
Conversely, internals docs cite reference docs instead of restating
hardware. Interleaved model docs (a hardware fact paired with the modelling
decision it forced) stay internals — that pairing is the value.

**`internals/` mirrors `src/`** — the full rule, with its exceptions
registry, lives in [internals/README.md](internals/README.md) and is
enforced by `scripts/check-doc-mirror.py`. In short: no `src/` file needs an
internals doc, but a doc that exists lives at the path mirrored from its
owning source — its directory is the `src/` directory that owns the
subject, its basename follows the owning source file.

## Machine documentation set: family, machine, device

Every emulated machine is documented by three levels of page, all in
`reference/`, all describing real hardware:

- **Family doc** (`reference/machines/<family>/<family>.md`) — what every
  motherboard in the family shares: the ASIC set, the shared memory map and
  address decode, the shared interrupt/bus/clock architecture, and the
  device roster linking every device to its page.
- **Machine doc** (`reference/machines/<family>/<machine>.md`) — what is
  unique to that machine: identity/gestalt, its deltas against the family
  doc (citing it by `§`), machine-specific wiring, expansion, boot.
- **Device doc** — `reference/machines/<family>/<device>.md` if the part is
  family-specific; `reference/hardware/[<subject>/]<device>.md` if it
  appears across families.

Rules that make the three levels add up:

1. **Say it once.** A fact lives at the highest level true of everything
   below it; everything else cites it by `§` (e.g. `glue.md §3`).
2. **Together they are complete.** Family + machines + devices are one
   specification, measured by the re-implementation test below as a set.
3. **Hardware only.** How the family is modelled stays in
   `docs/internals/machines/<family>/`, which cites these pages.
4. **Every device gets its own page**, even where a system doc's section
   currently covers it.
5. **A family of one** (e.g. `iifx/`) may carry the family role on its
   machine page, stated in its §1.

A PR that adds a machine, a family, or a device to `src/` also adds its
reference page here, at least as a stub, in the same PR. A device that
exists in `src/` with no landing page in `reference/` is not acceptable;
stubs are.

## Content rules

These apply to every document in this tree; R6 is specific to `reference/`.
`notes/` is exempt from the templates and, being frozen dated logs, from
R5/R6, but not from R1–R4. `user/` and `articles/` follow `articles/`: R1,
R2 and R5 in full; R3/R4 rarely bite (a References section is optional and
omitted when there is nothing to cite); R6 does not apply.

- **R1 — Standalone work. Never name or reference other emulators.** No <!-- lint-allow: other emulator -->
  comparative sections, no citing another emulator's source as evidence.
  Where behaviour was cross-checked against another implementation, state
  the verified fact and cite a primary source instead.
  *Exception — file-format compatibility:* where Granny Smith reads a disk
  image format that another emulator defined, so that existing images made
  with it attach as they are, the format is named after that emulator (the
  LisaEm ProFile image is the case today). Say that compatibility is the only <!-- lint-allow: LisaEm -->
  reason it is named, and mark each line with a `lint-allow` annotation.
- **R2 — Never name or reference leaked Apple source code.** It may inform <!-- lint-allow: leaked -->
  reverse engineering, but a doc must never point a reader at it, quote it,
  or use its internal identifiers as citations. Cite observable evidence:
  ROM disassembly addresses, guest-visible behaviour, official
  documentation. Apple *codenames* (PDM, TNT, Cyclone…) appear in published
  developer notes and are fine as names for ROMs, boards and projects.
  Officially *released* source is citable (e.g. the Lisa OS source released
  via the Computer History Museum).
- **R3 — Official sources are listed formally**, by full title and edition
  where known: hardware manuals, chip datasheets, Apple developer notes,
  *Inside Macintosh*, *Guide to the Macintosh Family Hardware*, *Designing
  Cards and Drivers*, ERS documents. Never session notes or private dossiers.
- **R4 — References live at the end, cited by number.** Each doc ends with a
  `## References` section, numbered; the text cites `[1]`, `[3] §4.2`. ROM
  disassembly evidence is a reference entry too ("SE/30 ROM disassembly,
  `$97221136`"), so provenance stays out of the prose.
- **R5 — one exact Markdown layout** (extends
  [guide/STYLE_GUIDE.md](guide/STYLE_GUIDE.md), the home of the base rules):
  exactly one `#` H1 (unnumbered); `##` sections numbered
  (`## 3. Programming model`) and `###` subsections compound-numbered
  (`### 3.2 Init sequence`); appendices lettered
  (`## Appendix A. Granny Smith implementation notes`); `## References` the
  unnumbered final section, after appendices; a hand-written TOC for docs
  over ~400 lines; register/field/packet maps as tables with semantics in
  prose beneath. Cross-references use section numbers — `§3.2`,
  `lisa.md §6.1`, `[3] §4.2` — which makes **section numbers a stable API**:
  renumbering is a breaking change and the editor fixes inbound
  §-references in the same PR.
- **R6 — reference documentation is exhaustive: the re-implementation
  test.** Could a competent engineer with *no access to Granny Smith's
  source code* write a working emulator of this machine from
  `reference/` alone? Everything needed for the answer to be yes belongs in
  the docs: register maps down to the bit, reset values, address decode and
  aliasing, timing and interrupt sequencing, bus and DMA behaviour, error
  paths, the initialisation sequences the ROM and OS actually perform, and
  the quirks real guest software depends on. No technical detail is left out
  as "too low-level" or "an implementation detail". Completeness is
  collective — applied to family + machine + device docs in combination.
  Four corollaries: **evidence direction** — a reference doc is built from
  primary material (manuals, datasheets, dossiers, disassemblies, observed
  guest behaviour), never from the emulator's source: the code is a
  *checklist of claims*, each of which is re-established from primary
  evidence, marked *observed*/*inferred*, or moved to Open questions, and
  because the tree is independent of the implementation, audits run
  code-against-docs, never the reverse; every reference doc carries an
  **"Open questions"**
  section listing what is *not* known (completeness must be falsifiable);
  the duty is about the hardware, not how the emulator copes; and when
  development establishes a new hardware fact, the reference doc is updated
  in the same PR as the code that relies on it.

Enforcement: R1/R2 have a denylist lint in CI
([`scripts/check-doc-rules.py`](../scripts/check-doc-rules.py)) that fails
on new violations; R5's cheap structure is linted on stubs today and rides
doc-by-doc as existing pages are normalized; R6 is a review-time bar.

## Document templates

All templates follow the R5 layout: one H1, numbered `## N.` sections,
lettered appendices, `## References` last, "Open questions" mandatory in
`reference/`.

**Chip/peripheral doc** (`reference/hardware/`, device docs):
1. Overview (what the part is, which machines carry it) ·
2. Register file (summary table, then per-register detail) ·
3. Behaviour (state machines, timing, interrupts) ·
4. Programming model (how the ROM/OS actually drives it, init sequences) ·
5. Quirks & errata · 6. Open questions · Appendix A. Granny Smith
implementation notes (optional) · References.

**Family doc**: 1. Overview & membership · 2. Board architecture common to
the family · 3. Memory map & address decode shared by the family ·
4. Device roster · 5. Interrupt, bus, and clock architecture ·
6. Per-machine index · 7. Open questions · Appendix A (optional) ·
References.

**Machine doc**: 1. Identity (gestalt / box flag / ROM version, spec
table) · 2. Deltas vs the family doc · 3. Per-subsystem wiring ·
4. Expansion · 5. Boot sequence summary · 6. Open questions · Appendix A
(optional) · References. A machine doc does not restate what the family doc
already says — but between the two, nothing is missing.

**Protocol doc**: 1. Scope & layering · 2. Wire formats · 3. State
machines · 4. Worked captures/examples · 5. Open questions · Appendix A
(optional) · References.

**Format doc**: 1. Identification (magic, sizes) · 2. Layout tables ·
3. Algorithms (checksums, with runnable pseudocode) · 4. Worked decode of a
real specimen · 5. Open questions · Appendix A (optional) · References.

**Subsystem design doc** (`internals/core/`): first paragraph names the owning
`src/` files; then 1. Responsibilities & design · 2. Key types & files ·
3. Behaviour/algorithms · 4. Object-model / shell surface ·
5. Checkpointing · 6. Testing · 7. Known debts · 8. See also. Hardware
facts are cited from `reference/`, not restated.

**Machine-family docs** (`internals/machines/`) come in three shapes — family
substrate, machine leaf, chip model — see the existing pages under
[internals/machines/](internals/machines) for the pattern.

**User guide** (`user/`): task-oriented, numbered steps over prose,
imperative mood; must stay true with no source checkout present — the
moment a page explains implementation, that part moves to `internals/` or
`guide/`.

**Articles** (`articles/`) are freeform long-form; **notes** are dated,
frozen once written.

## Placeholder stubs

Pages in the coverage map that are planned but not yet written are checked
in as stubs so the tree shows the full coverage map, and every promotion has
a pre-agreed landing spot. A stub
carries:

- the banner `> 🚧 **Placeholder** — this page is planned but not yet
  written.` as the first body line, plus the machine-readable marker
  `<!-- gs-doc-status: stub -->` that the lint keys off;
- the R5 skeleton of the applicable template above, with a
  `> **TODO**` marker under each heading;
- a one-to-two-sentence factual scope statement of what the part or
  machine is.

Stubs are exempt from R6 (they claim nothing) but pass R1/R2/R5. The stub
marker is removed only in the PR that fills the page — which is where the
R6 audit happens; if the bar is not genuinely met yet, the page stays a
partial stub and says so.

## Contributing a document

1. Read the rules (R1–R6) and the template for the doc type; read the
   stub if one exists, and the named exemplar of that type — for a chip
   page, [swim.md](reference/hardware/swim.md); for a family doc,
   [glue/](reference/machines/glue/); for a format,
   [diskcopy42.md](reference/formats/diskcopy42.md). Match it.
2. Placement follows the classification rule above, the family/machine/
   device set, family-first (everything about one family in its
   `reference/machines/<family>/` subtree) and subject grouping
   (`hardware/nubus/`, `hardware/scsi/`, `hardware/pci/`, …). Never invent
   a new directory without flagging it.
3. Write about the hardware, never about Granny Smith: the emulator's
   source layout, object model, shell commands and modelling shortcuts
   belong in `internals/`, or in the optional implementation-notes
   appendix — never in the body.
4. **Do not guess. Ever.** Anything not evidenced goes in "Open
   questions", not in the body as fact. Inventing plausible register
   semantics from naming conventions is the classic failure mode.
5. Every non-obvious claim is traceable: cite `[n]` (R4), or mark it
   *observed* (say where/how) or *inferred — unverified*. Fact and
   inference are never blended in one sentence.
6. Sourcing rules: no other emulators (R1), no leaked source (R2), formal <!-- lint-allow: other emulator, leaked -->
   titles only (R3).
7. Reference docs carry the R6 duty: build from the primary material
   first — manuals, datasheets, RE dossiers, disassemblies, observed guest
   behaviour — then sweep the owning `src/` files' comments, the C-side
   model doc and test expectations *as a claim checklist*: each hardware
   fact found there is re-established from primary evidence, marked
   *observed*/*inferred*, or moved to Open questions; the code is never the
   authority. Ask before
   calling the page done: *what would still be missing for someone
   re-implementing this from the page alone?* The answer goes in the body
   or in Open questions.
8. Altitude split: hardware truth → the reference doc; modelling
   decisions, scope cuts, object-model surface, tests → the C-side model
   doc, which cites the reference doc instead of restating it.
9. Reference voice: present tense, no session narrative, no changelog.
10. Finish means: R5 layout exact, links pass
    [`scripts/check-links.py`](../scripts/check-links.py), the lint passes,
    the stub marker removed (only if the R6 bar is genuinely met), and the
    section index updated.
