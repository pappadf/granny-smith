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

### ROM Images

Located in `tests/data/roms/`:

| File         | Size    | Description            |
|--------------|---------|------------------------|
| `Plus_v3.rom`| 128 KB  | Macintosh Plus ROM v3  |
| `IIcx.rom`   | 256 KB  | Macintosh IIcx ROM     |

**How to obtain:**
- Extract from a physical Macintosh using ROM extraction tools

### System Software

Located in `tests/data/systems/`:

| File                  | Description         |
|-----------------------|---------------------|
| `System_2_0_1.dsk`    | System 2.0.1        |
| `System_3_0_0.dsk`    | System 3.0          |
| `System_3_2_0.dsk`    | System 3.2          |
| `System_3_3_0.dsk`    | System 3.3          |
| `System_4_0_0.dsk`    | System 4.0          |
| `System_4_1_0.dsk`    | System 4.1          |
| `System_4_2_0.dsk`    | System 4.2          |
| `System_4_3_0.dsk`    | System 4.3          |
| `System_6_0_0.dsk`    | System 6.0          |
| `System_6_0_3.dsk`    | System 6.0.3        |
| `System_6_0_5.dsk`    | System 6.0.5        |
| `System_6_0_8.dsk`    | System 6.0.8        |
| `System_7_1_0.dsk`    | System 7.1          |

**Disk image format:**
- 800 KB raw disk images (no headers)
- File size should be exactly 819,200 bytes for 800K floppies
- For hard disk images, use raw block format

### Application Test Data

Located in `tests/data/apps/`:

| File                          | Description                   |
|-------------------------------|-------------------------------|
| `MacTest_Disk.image_.sit_.hqx`| MacTest diagnostic application |

---

## Directory Structure

Your `tests/data/` directory should look like this:

```
tests/data/
├── README.md           (this can remain - explains the directory)
├── roms/
│   ├── Plus_v3.rom     (128 KB - Macintosh Plus ROM)
│   └── IIcx.rom        (256 KB - optional)
├── systems/
│   ├── System_6_0_8.dsk   (819,200 bytes - primary test system)
│   ├── System_7_1_0.dsk   (optional)
│   └── ... (other system disks)
└── apps/
    └── ... (test applications)
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
   │   └── Plus_v3.rom
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

The most commonly needed file is `tests/data/roms/Plus_v3.rom` and `tests/data/systems/System_6_0_8.dsk`.
