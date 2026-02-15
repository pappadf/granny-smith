# Checkpoint Save/Load v2 Integration Test

This is an improved version of the checkpoint integration test with the following key differences:

## Differences from `checkpoint` test

1. **Static Scripts**: Uses pre-written `step1.script` and `step2.script` files that you can edit directly, rather than dynamically generating them.

2. **Persistent Results**: Test artifacts are saved to `tests/integration/test-results/checkpoint2/` instead of `/tmp`, making it easier to inspect results after test completion.

3. **Debuggability**: All intermediate files (screenshots, checkpoints, scripts) are preserved for debugging.

## Test Structure

- `config.mk` - Test configuration
- `run.sh` - Custom test runner that orchestrates the two-step test
- `step1.script` - Template for step 1 (boot from floppy, save checkpoint)
- `step2.script` - Template for step 2 (boot from HD, load checkpoint, verify)
- `desktop.png` - Reference screenshot for validation
- `test.script` - Marker file for test auto-discovery

## Test Artifacts (in test-results/checkpoint2/)

After running the test, you'll find:
- `checkpoint.gs` - Saved machine state checkpoint
- `screenshot-step1.png` - Screen state after step 1
- `screenshot-step2.png` - Screen state after step 2 (should match desktop.png)
- `step1_run.script` - Generated script with actual paths (step 1)
- `step2_run.script` - Generated script with actual paths (step 2)
- `hd1.img` - Unzipped SCSI hard disk image

## Running the Test

```bash
# Run just this test
make -C tests/integration test-checkpoint2

# Run all integration tests
make integration-test
```

## How It Works

1. **Step 1**: Boots from floppy disk, runs 50M instructions, saves checkpoint
2. **Step 2**: Boots from SCSI HD, runs 100M instructions (different state), then loads the checkpoint from step 1, runs 50M more instructions, and verifies the screen matches the expected desktop

This validates that checkpoint save/restore correctly captures and restores emulator state across different boot configurations.
