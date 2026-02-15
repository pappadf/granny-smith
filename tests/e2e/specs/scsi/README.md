## SCSI (URL media) Tests

This suite exercises SCSI hard‑disk behavior (and mixed floppy + SCSI scenarios) using URL parameters
to supply media. All media files are served from the shared `tests/e2e/media` root (no per‑suite copies).

### Tests Overview

1. **Test 1 (Boot from hd0)**  
  Boots with only a ROM (`roms/Plus_v3.rom`) and a SCSI hard disk zip (`systems/hd1.zip`, mounted as `hd0`).  
  Verifies the fully processed 1‑bit 512×342 screen matches baseline `baseline-test-1A.png` (created / updated automatically when `UPDATE_SNAPSHOTS=1`).

2. **Test 2 (Boot from fd0, test & reformat hd0)**  
  Boots with ROM + floppy (`systems/System_6_0_5.dsk` as `fd0`) + hard disk (`hd1.zip` as `hd0`). Then performs a sequence of UI operations inside "Apple HD SC Setup" and captures multiple screen states:
  - `baseline-test-2A.png` Finder after initial boot (60s timeout).
  - `baseline-test-2B.png` After running the HD test (120s timeout for slower operation).
  - `baseline-test-2C.png` After reformat / initialize completes (120s timeout).
  - `baseline-test-2D.png` Final Finder view showing the mounted, reformatted volume (120s timeout).

### Image Processing Pipeline
All comparisons use `matchScreen()` which:
1. Captures `#screen`.
2. Crops off first row/col if the top‑left pixel is not pure black/white.
3. Validates dimensions are integer multiples of 512×342; downscales by sampling the top‑left pixel of each source cell.
4. Converts to 1‑bit (threshold at luminance 128) producing a canonical 512×342 grayscale (0 or 255) PNG.
5. Optionally extracts a region (these tests use the full screen by default).  
6. Compares the region bytes directly to the baseline PNG.

### Baseline Naming & Defaults
`matchScreen(page, 'name')` automatically looks for (or writes) `name.png` in the same directory as the invoking test file (falling back to Playwright snapshot path if present). Call options are minimal:

Defaults (current):
* `timeoutMs`: 120_000 (tests override to 60_000 for faster early states or keep default for longer waits)
* `pollMs`: 5000 (boot‑matrix suite overrides to 500 for faster small‑region polling; SCSI uses default)
* `waitBeforeUpdateMs`: 60_000 (delay before writing a new baseline when updating)
* Full screen region (512×342) if `region` omitted

Only non‑default values are specified in the test code (e.g. `timeoutMs: 60_000` for faster fail on first Finder match).

### Updating Baselines
To update (recreate) all SCSI baselines:

```bash
UPDATE_SNAPSHOTS=1 npx playwright test tests/e2e/scsi
```

To update only Test 2 baselines:

```bash
UPDATE_SNAPSHOTS=1 npx playwright test tests/e2e/scsi/scsi.spec.ts -g "SCSI Test 2"
```

Individual baseline file removal + rerun with `UPDATE_SNAPSHOTS=1` also works.

### Artifacts & Logging
Each test attaches:
* Processed full screen & region images (under derived attachment names) for the last attempt.
* Xterm dump (`final` / `final-hd`).
* Console + network logs via `initLogging()`.

### Notes
* Relies on `bootWithMedia()` helper to build the navigation URL with appropriate `rom`, `fd0`, and `hd0` parameters.
* Hard disk archive `hd1.zip` is served unchanged; the emulator handles mounting the contained image.
* Polling interval (5s) reduces noise while still catching state transitions; longer operations use larger `timeoutMs` instead of tighter polling.
* All image comparisons are exact byte matches after the deterministic 1‑bit processing pipeline.
