# Floppy — the IWM/SWIM controller model (`floppy.c`)

`src/core/peripherals/floppy.c` (with `floppy_gcr.c` and `floppy_swim.c`)
models the Macintosh floppy controller in its IWM-only form (Mac Plus) and
the SWIM's dual IWM/ISM form (SE/30 and the II family), together with the
drives behind it. The controller-driven SWIM III and New Age chips have
their own pages ([swim3.md](swim3.md), [new_age.md](new_age.md)). This page
records the model's timing choices that deliberately depart from the drive
specification.

## Step settle time

After each step command the drive's `/STEP` sense line reads 0 (step in
progress) until the head has settled; a scheduler event ends the settle
period (`STEP_SETTLE_TIME_NS` in `floppy_internal.h`).

On real hardware the settle is about 12 ms. The model uses **10 µs**. The
emulator runs the CPU against an averaged cycles-per-instruction figure, and
the ROM waits for the settle with a fixed-iteration polling loop (about 2,800
instructions on the Plus, about 3,600 on the SE/30) rather than a timer. At
low CPI values that loop expires long before 12 ms of emulated time have
passed, and the ROM reports a seek failure. 10 µs is long enough for
MacTest's post-step check to still see `/STEP` = 0 (a step in progress), and
short enough to settle inside the ROM's loop at any CPI.

## Motor speed settle time

Stepping across a speed-zone boundary on a GCR disk changes the target
spindle speed; `/READY` is deasserted for `SPEED_SETTLE_TIME_NS` (5 ms) while
the motor reaches it. Motor spin-up from rest takes `MOTOR_SPINUP_TIME_NS`
(400 ms).

## Inserting a disk

`floppy_insert` accepts only images the image layer classifies as floppy
media — 400K and 800K GCR, 720K and 1440K MFM — and refuses anything else,
whatever the caller validated before it.
