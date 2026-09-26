# Integration test: LaserWriter print over AppleTalk from System 7.1
#
# The sibling of appletalk-print, which prints from System 6.0.8 on a Plus
# with LaserWriter 7.0.  This row prints the same way — Chooser, then the
# Finder's "Print Window…" — from System 7.1 on a IIcx with LaserWriter
# 7.1.2, because the two driver versions send materially different
# PostScript and only this one exercises the paths below.
#
# It exists because a field report ("IIcx + System 7.1: offending command,
# and the PDF will not open") turned out to be two defects that System
# 6.0.8 never reached:
#
#   * The driver probes for `cexec` — a printer extension that runs native
#     code on the printer's own processor — inside `{eexec}stopped`, and
#     needs the `undefined` error to discard the code string it pushed.
#     EfterScript defined `cexec` as `exec`, so the string stayed on the
#     stack and the `and` after the probe failed with `typecheck`: no page.
#   * With that fixed, the driver still downloaded a LaserWriter-only path,
#     because our `product` began with "LaserWriter" and the AppleDict
#     prologue keys that path off exactly that prefix (`LW`, computed with
#     `anchorsearch`).  The product no longer claims to be one.
#
# Assertions are the object model's own values, as in appletalk-print.
#
# GATED ON THE INTERPRETER, like appletalk-print: a PLATEN=0 binary has no
# bridge, so the row logs a skip and passes.  To run it for real:
#
#   make -C tests/integration test-appletalk-print-71 PLATEN=1
#
# The PDF lands in the results directory (--print-dir).

TEST_NAME := LaserWriter Print from System 7.1 (platen)
TEST_DESC := IIcx + System 7.1 + LaserWriter 7.1.2 -> Chooser -> Print Window -> a PDF via the platen interpreter; idle afterward.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The script attaches the base image directly, as the other IIcx rows do:
# printing writes nothing to the guest volume, and writes that do happen go
# to the image's delta beside it rather than into the base.

# The harness creates the IIcx with 8 MB; the script re-boots with the 24AC
# card selected (video_card can't be passed as an arg) and attaches the HD.
TEST_ARGS := model=iicx ram=8192 --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
