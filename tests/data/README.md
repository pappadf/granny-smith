# Test Data

This directory contains proprietary test data required to run the full test suite.

## ⚠️ Important Notice

**The contents of this directory (except this README) are NOT included in the public repository.**

The test data (ROM images, disk images, system software) required to run the full emulator test suite is copyrighted material that cannot be redistributed.

## For Maintainers

If you have access to the private test data repository, run:

```bash
./scripts/fetch-test-data.sh
```

This will clone/update the test data from the private `pappadf/gs-test-data` repository.

**Requirements:**
- `GS_TEST_DATA_TOKEN` environment variable set to a GitHub PAT with read access
- In GitHub Codespaces: The token is automatically available if configured in your Codespaces secrets
- In CI: The token is automatically available via repository secrets

## For Contributors

If you want to run the full test suite locally, you have two options:

### Option 1: Run Limited Tests

The CPU unit tests do not require proprietary test data:

```bash
make -C tests/unit run
```

These tests use the open source [ProcessorTests](https://github.com/SingleStepTests/ProcessorTests) from `third-party/single-step-tests/`.

### Option 2: Provide Your Own Test Data

You can legally obtain the required software and create your own test data. See [docs/TEST_DATA.md](../../docs/TEST_DATA.md) for the required file structure and specifications.

## Expected Structure

When properly configured, this directory should contain:

```
tests/data/
├── README.md              ← You are here (this file is in the public repo)
├── roms/
│   ├── Plus_v3.rom        # Macintosh Plus ROM (128 KB)
│   └── IIcx.rom           # Macintosh IIcx ROM (optional)
├── systems/
│   ├── System_6_0_8.dsk   # System 6.0.8 boot disk (primary test target)
│   └── ...                # Other system versions
└── apps/
    └── ...                # Application disk images
```

## More Information

See [docs/TEST_DATA.md](../../docs/TEST_DATA.md) for complete details on:
- Required files and their specifications
- How to obtain test data legally
- Setting up the private repository (for maintainers)
- Troubleshooting
