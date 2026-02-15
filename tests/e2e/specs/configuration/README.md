# Configuration (URL media) Test

This suite validates a minimal configuration flow: booting the emulator when `rom` and `fd0` media
are supplied via URL parameters. Media assets are served from the shared `tests/e2e/media` root (no
per‑suite copies).

## Test logic
1. Intercept emulator fetches for `Plus_v3.rom` and `System_4_1_0.dsk` via a synthetic `/tests-media/` URL space.
2. Navigate to `/index.html?rom=...&fd0=...` so the app auto loads the media and runs.
3. Wait for the ROM-required overlay to disappear (indicates successful load + run sequence).
4. Capture the processed full screen (1‑bit, 512x342) using `captureProcessedFull()`.
5. Compare the entire canonical image against `baseline-full.png` stored in this directory. If that file does not exist and `UPDATE_SNAPSHOTS=1` is set, it will be created.

## Updating the snapshot
Run (from repo root):

```bash
UPDATE_SNAPSHOTS=1 npx playwright test tests/e2e/configuration
```

After a successful boot the file `baseline-full.png` will be written in this folder. Subsequent runs
(without the env var) compare against it. A mismatch (or absence of the baseline) causes the test to fail.

## Notes
* Only a *single* scenario is covered for now (URL param boot). Additional tests can be layered in later.
* Screen capture utilities were refactored: `captureProcessedFull()` (full canonical image) underpins region extraction.
* The test uses a 60s watchdog; failure to match the baseline inside that window fails the test.
