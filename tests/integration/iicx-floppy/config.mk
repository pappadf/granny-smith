# Integration test configuration: IIcx System 7.0.1 Floppy Boot
# Boots the IIcx with the System 7.0.1 1.44 MB MFM floppy attached.
# Verifies the SWIM driver mounts the disk and the Universal ROM
# transfers to the System 7 boot blocks without crashing or surfacing
# unhandled-write errors.
#
# This is a "no-crash + booting" smoke test rather than a "reach the
# Finder desktop" check — the JMFB driver is minimum-viable per
# proposal §3.2.5, and the System 7 driver in `Apple-341-0868.vrom`
# may surface unmodelled register writes that block the desktop from
# fully painting.  When step-6 work fleshes out those writes, this
# test gains a `screen.match finder.png` assertion.

TEST_NAME := IIcx System 7.0.1 Floppy Boot
TEST_DESC := Boots IIcx from the System 7.0.1 floppy and verifies no crash

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
