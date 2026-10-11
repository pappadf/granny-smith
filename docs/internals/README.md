# Internals — implementation notes mirroring `src/`

This tree holds the implementation documentation: how Granny Smith models
and is built, as opposed to what the real hardware does ([`../reference/`](../reference/))
or how the product is used ([`../user/`](../user/)). Its subdirectories mirror
`src/` — `core/` for `src/core/`, `machines/` for `src/machines/`.

## The mirror rule

1. **Coverage is optional.** No `src/` file needs an internals doc — a page
   exists only when there is something to say about the implementation.
   (This is the exact inverse of `reference/`'s every-device-gets-a-stub
   duty, and the asymmetry is deliberate.)
2. **Placement is determined, not chosen.** If a doc exists, it lives at
   the path mirrored from its owning source: its directory is the `src/`
   directory that owns the subject, and its basename follows the owning
   source file (`sonic.md` for `sonic.c`, `tnt.md` for the `tnt.c` family
   substrate) or the subsystem/directory name when the doc owns many files
   (`scheduler.md` for `src/core/scheduler/`).
3. **Deviations are registered, never improvised.** Every doc that cannot
   follow the basename convention is a row in the Exceptions table below —
   the single registry of deliberate deviations, enforced by
   [`scripts/check-doc-mirror.py`](../../scripts/check-doc-mirror.py) in CI.
   Never prose in AGENTS.md explaining why a doc area covers several
   source directories.

`src/platform/` and `src/peeler/` are future candidates for the same
treatment (`platform/`, `peeler/` here) — their docs currently live in
`guide/web.md` and `guide/peeler.md`.

## Exceptions

| doc | owning source, and why the stem differs |
|---|---|
| `core/checkpointing.md` | `src/core/checkpoint.c` plus the per-subsystem `<subsystem>_checkpoint` contract it orchestrates |
| `core/cpu/cores.md` | the core registry across `src/core/cpu/cpu.c` and its `cpu_68000/68030/68040`, `ppc/` and `dsp3210/` models |
| `core/network/ppc_appleevents.md` | the program-linking layers `src/core/network/appletalk_ppc.c` and `appletalk_aevt.c` |
| `core/object/object-model.md` | the subsystem-wide model doc for `src/core/object/` (`object.c`, `value.c`, `expr.c`, `parse.c`, …) |
| `core/peripherals/mouse_control.md` | cross-cutting input automation: `src/core/peripherals/adb.c`, `mouse.c`, `mouse_class.c` and `src/core/host_input.c` |
| `core/peripherals/nubus_generic_vrom.md` | the generic vROM generator `src/core/peripherals/nubus/gsvrom_data.c` (`gsvrom.h`) |
| `core/peripherals/nubus/cards/display_card_8_24.md` | `src/core/peripherals/nubus/cards/jmfb.c` — the source keeps Apple's ASIC codename `jmfb` |
| `core/scheduler/timing.md` | the timing model inside `src/core/scheduler/scheduler.c` |
| `core/storage/target-filesystems.md` | the APM/HFS/UFS readers `src/core/storage/image_apm.c`, `image_hfs.c`, `image_ufs.c` |
| `core/storage/bare-volume-wrapper.md` | the bare-volume wrapper `src/core/storage/image_wrap.c` plus its `gsdisk/` driver (added by #204, placed here by the mirror rule) |
| `machines/lisa/fdc.md` | `src/machines/lisa/lisa_fdc.c` — the doc drops the `lisa_` prefix |
| `machines/lisa/mmu.md` | `src/core/memory/lisa_mmu.c` — the doc drops the `lisa_` prefix and stays with the Lisa family |
| `machines/lisa/profile.md` | `src/machines/lisa/lisa_profile.c` — the doc drops the `lisa_` prefix |
| `machines/pdm/video.md` | the PDM video path built on `src/machines/pdm/ariel.c` |
