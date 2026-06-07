# Integration test configuration: IIfx A/UX 3.0.1 HD Boot
# Mirrors se30-aux3-boot but exercises the IIfx-specific boot path
# (PMMU + FMC + OSS + SCC/SWIM IOPs) against the same A/UX 3 HD image.
#
# The IIfx now boots A/UX 3.0.1 all the way to the `login:` prompt
# (text getty).  This was unblocked by the IIfx bus-master SCSI DMA
# scatter-gather fix (src/core/peripherals/scsi.c scsi_signal_eop: a
# DMA-segment EOP keeps the bus in data_in so the driver re-arms each
# scatter-gather segment instead of completing per segment).  The test
# pins the `login:` screen so any regression in the boot path — or
# further progress past getty — is caught.  See test.script.

TEST_NAME := IIfx A/UX 3.0.1 HD Boot (reaches login)
TEST_DESC := Boot IIfx with 16 MB RAM from the A/UX 3.0.1 HD image; expect the A/UX text `login:` prompt.

# IIfx ROM (checksum 0x4147DD77).  JMFB declrom (Apple-341-0868.vrom) is
# auto-discovered from the same directory as the ROM file; the content is
# identical to tests/data/roms/341-0868.vrom.
TEST_ROM := roms/4147DD77-IIfx.rom

# Copy the HD image into TEST_TMPDIR so its .delta/.journal are wiped with
# the tempdir on exit; the source image is left untouched between runs.
# Keep the TEST_SETUP line single-line — the Makefile extractor only honors
# $(TEST_DATA) and $(TEST_TMPDIR) substitutions.
TEST_SETUP := cp "$(TEST_DATA)/aux/aux_3.0.1/hd160-with-aux-301.img" "$(TEST_TMPDIR)/hd.img"

# 16 MB RAM matches the SE/30 A/UX test (A/UX itself wants 16 MB; even
# though the kernel isn't running here, keep the budget identical so
# any future A/UX startup runs against the same footprint).
TEST_ARGS := model=iifx ram=16384 hd=$(TEST_TMPDIR)/hd.img
