# Test Data Requirements

This document describes the proprietary test data required to run the full Granny Smith test suite, and how contributors can obtain or provide their own copies.

## Overview

Granny Smith is a Macintosh emulator. To fully test the emulation, we need:

1. **ROM Images** - The Macintosh firmware
2. **System Software** - Apple's operating system disk images
3. **Application Software** - Test applications (optional)

These files are copyrighted and cannot be redistributed with this open source project.

## Test Coverage Without Proprietary Data

Even without proprietary test data, you can still:

- ✅ Build the emulator (`make`)
- ✅ Run CPU instruction unit tests (`make -C tests/unit run`)

The CPU tests use the open source [ProcessorTests](https://github.com/SingleStepTests/ProcessorTests) which are included in `third-party/single-step-tests/`.

---

## Required Files

Everything below lives under `tests/data/`, which is `.gitignored` and
synced from the private `gs-test-data` repository by
`scripts/fetch-test-data.sh`. The listing reflects what the integration
suite actually loads — `scripts/test-matrix.py --tests` prints the
per-test media mapping if you need to know which test needs what.

### ROM images (`tests/data/roms/`)

Machine ROMs. The filename carries the ROM's checksum, and `rom-naming`
(unit tier) enforces that grammar, so a mismatched dump is caught rather
than silently booted.

| File | Machines |
|---|---|
| `plus-v3-4d1f8172.rom` | Macintosh Plus |
| `iix-iicx-se30-97221136.rom` | Universal: SE/30, IIx, IIcx |
| `iici-368cadfe.rom` | IIci |
| `iisi-36b7fb6c.rom` | IIsi |
| `iifx-4147dd77.rom` | IIfx |
| `q700-q900-420dbff3.rom` | Quadra 700 and 900 |
| `q950-3dc27823.rom` | Quadra 950 |
| `lisa2-revh-098917b2.rom` | Lisa 2 (rev H) |
| `macxl-3a-094c82f0.rom` | Macintosh XL |

NuBus **declaration ROMs** (`*.vrom`) live beside them — the 8•24 (JMFB),
8•24 GC, 24AC and the SE/30 built-in video. Cards can also run on the
runtime-generated generic GS vROM instead; `iicx-gsvrom` covers that path.

### Prepared hard-disk images (`tests/data/systems/`)

These are the workhorses: full installs that boot on any supported
machine, so a test can pick its host freely. Naming grammar (§6.1 of
proposal-integration-test-rework):
`system_<ver>_<size>_<trait>[_<trait>…].img`.

| File | Contents |
|---|---|
| `system_6_0_8_20mb_8_24gc.img` | System 6.0.8, 20 MB ST225N geometry, 8•24 GC support + AppleShare + 32-Bit QD |
| `system_7_1_20mb_24ac_cd_32bit.img` | System 7.1, 20 MB, 24AC drivers, 32-bit enabled |
| `system_7_1_20mb_24ac_cd_32bit_gc.img` | as above plus 8•24 GC support |
| `system_7_5_0_77mb_mode32_24ac.img` | System 7.5.0, 77 MB, MODE32 + 24AC drivers |
| `system_7_6_170mb_24ac.img` | Mac OS 7.6, 170 MB HD160SC, minimum system + 24AC drivers |

The 7.5 and 7.6 images are consumed by rows that **skip gracefully** when
they are absent, so a checkout without them still runs green — the
coverage check reports those cells as media-gated rather than failing
(`python3 scripts/test-matrix.py --check --tier=matrix <log>`).

`hd1.zip` is **no longer used**: it was verified to be the same System
6.0.8 at the same geometry as `system_6_0_8_20mb_8_24gc.img`, whose
System Folder is a strict superset, so every consumer was re-pointed and
the suite's last `TEST_SETUP` unzip went away with it.

### System floppies (`tests/data/systems/`)

Single-disk system images (`System_<ver>.dsk`, 400K/800K raw — exactly
409,600 or 819,200 bytes) plus the multi-disk installer sets
`SSW-2.0-400K/`, `SSW-3.2-400K/`, `SSW-4.2-800K/`, `SSW-6.08-800K/`,
`SSW-7.0-800K/`, `SSW-7.1-1.4M/`, `SSW-7.5-1.4M/`, `SSW-7.6-1.4M/`.

⚠️ **Three media labels are known to lie**, so do not trust a filename as
a system version (each is documented in §6.2/§7 of the rework proposal):

| File | Claims | Actually boots |
|---|---|---|
| `System_7_1_0.dsk` | System 7.1 | a 6.0.7-class System (7.1 does not fit on 800K) |
| `SSW-7.0-800K/` boot disk | System 7.0 | a minimal 6.0.7 that *installs* 7.0 |
| `System_6_0_8.dsk` | System 6.0.8 | reports Finder 6.1.5 / **System 6.0.5** in its own About box |

Deliberately unused as a result: `System_7_1_0.dsk` and the SSW-7.0 set.
Real 7.1 comes from the 1.4 M `SSW-7.1-1.4M/Disk Tools.img` or the
prepared 7.1 HD images.

### Other media

| Path | Contents |
|---|---|
| `tests/data/apps/` | MacTest diagnostics, Marathon, MusicWorks, Norton System Info |
| `tests/data/Lisa/` | Lisa Office System 3.1, Xenix 3.0, MacWorks XL 3.0 (floppies + installed ProFile images) |
| `tests/data/aux/aux_3.0.1/` | A/UX 3.0.1 retail ISO and an installed 160 MB HD image |
| `tests/data/cdroms/` | CD-ROM images |

---

## Directory Structure

```
tests/data/
├── roms/          machine ROMs (*.rom) + NuBus declaration ROMs (*.vrom)
├── systems/       prepared HD images, single system floppies, SSW-* sets
├── apps/          application/diagnostic media
├── Lisa/          Lisa Office System, Xenix, MacWorks XL
├── aux/           A/UX 3.0.1 installer ISO + installed HD image
└── cdroms/        CD-ROM images
```

---

## For Maintainers

### Setting Up the Private Repository

The test data is stored in a private GitHub repository. To set this up:

1. **Create the private repository:**
   - Repository name: `gs-test-data`
   - Visibility: **Private**
   - Owner: Same as the main repository

2. **Structure the repository:**
   ```
   gs-test-data/
   ├── roms/
   │   └── plus-v3-4d1f8172.rom
   ├── systems/
   │   ├── System_6_0_8.dsk
   │   └── ...
   └── apps/
       └── ...
   ```

3. **Create a Fine-Grained Personal Access Token:**
   - Go to: GitHub → Settings → Developer settings → Personal access tokens → Fine-grained tokens
   - Token name: `gs-test-data-read`
   - Expiration: Choose appropriate (max 1 year, set calendar reminder to rotate)
   - Repository access: Select **Only select repositories** → `gs-test-data`
   - Permissions: Repository permissions → Contents: **Read-only**
   - Generate and copy the token immediately

4. **Configure the public repository secret:**
   - Go to your public repository → Settings → Secrets and variables → Actions
   - Create new repository secret:
     - Name: `GS_TEST_DATA_TOKEN`
     - Value: (paste the token)

5. **Configure Codespaces secrets (for your personal Codespaces):**
   - Go to: GitHub → Settings (your user settings) → Codespaces → Secrets
   - Add new secret:
     - Name: `GS_TEST_DATA_TOKEN`
     - Value: (paste the token)
     - Repository access: Select your public repository

### Token Rotation

Fine-grained tokens expire. Set a calendar reminder to rotate before expiration:

1. Generate new token (same permissions)
2. Update the repository secret
3. Update your Codespaces secret
4. Delete the old token

---

## Security Considerations

The `fetch-test-data.sh` script is designed to prevent token leakage:

- Token is never passed as a command-line argument
- Token is never printed to stdout/stderr
- Git credential helper is used to pass token securely
- `GIT_TERMINAL_PROMPT=0` prevents interactive prompts

**Never:**
- Commit tokens to any repository
- Print tokens in CI logs
- Share tokens in issues or PRs

---

## Troubleshooting

### "Test data not available" error

This means the `tests/data/` directory doesn't have the required files.

**If you're a maintainer:**
```bash
# Check if your token is set (shows length, not value)
if [ -n "$GS_TEST_DATA_TOKEN" ]; then 
  echo "Token is set (length: ${#GS_TEST_DATA_TOKEN})"
else 
  echo "Token is NOT set"
fi

# Try fetching manually
./scripts/fetch-test-data.sh --status
./scripts/fetch-test-data.sh
```

**If you're a contributor:**
You need to provide your own test data or run only the CPU unit tests.

### "Permission denied" when fetching

Your token may have expired or lack the required permissions. Generate a new token with `contents:read` access to the private repository.

### Tests fail with "missing file" errors

Check that all required files are present:
```bash
ls -la tests/data/roms/
ls -la tests/data/systems/
```

The most commonly needed file is `tests/data/roms/plus-v3-4d1f8172.rom` and `tests/data/systems/System_6_0_8.dsk`.
