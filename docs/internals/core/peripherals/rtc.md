# RTC — the clock and parameter-RAM model (`rtc.c`)

`src/core/peripherals/rtc.c` models the Macintosh real-time clock: a
32-bit seconds counter and 256 bytes of battery-backed parameter RAM
(20 bytes on the pre-SE chip), reached either through the three-wire
serial interface on VIA1 port B (the classic chip on the Plus and the II
family) or as plain storage behind Egret or Cuda, which carry the same
clock and PRAM in their own firmware. The chip itself — its command set,
the write-protect register and the PRAM layout — is documented in the
hardware reference, [rtc.md](../../../reference/hardware/rtc.md); this page
covers how the emulator seeds, keeps and resets it.

## The two rules for the clock

The clock is a **simulated-time counter**. It advances once per second of
emulated time (the `one_second` scheduler event), never by host time.

1. **Created → the host wall clock.** `rtc_init` seeds `seconds` from
   `time(NULL)` plus the 1904 epoch offset. A machine nobody gives a time
   boots at the real current time, which is what the web UI wants.
2. **Set → it sticks.** `machine.rtc.time = N` writes the live counter.
   A test that wants a deterministic clock writes it **on the line after
   the boot**: `machine.boot` leaves the guest at zero instructions
   executed, so the write lands before the ROM has read anything and is
   exactly equivalent to seeding at construction.

```
machine.boot model="se30" ram=8192 rom="${$ROM}"
machine.rtc.time = 3000000000        # the device, directly
scheduler.run ...
```

There is **no staged seed**. An earlier design held a pre-boot
`rtc.time` in a module-scope static until the next `rtc_init` adopted it;
only the PDM family ever consumed it, so a pre-boot pin was silently
discarded everywhere else, and — because it was never cleared — a pin set
by one test row would have carried over to every machine built after it in
the same process. Device state is written to the device, never kept in a
special place across a construction. A pin written *before* a
`machine.boot` therefore does nothing to the new machine, which reads the
wall clock (`tests/integration/rtc-seed`, the negative control).

Mode3Clock guests (Copland on the PDM family) read the clock through the
Cuda tick and render it, so their goldens depend on the pin. One caveat is
the guest's, not ours: a pin *earlier* than the file dates on an installed
volume makes Copland's File Manager reject the catalog at volume mount, so
`pm7100-copland-boot` pins a time after its installation.

## Across the reset levels

| Operation | Clock | PRAM | Serial interface |
|---|---|---|---|
| `machine.reset` (level 2) | keeps counting | kept | idle (see below) |
| `machine.restart` (level 3) | keeps counting | kept | idle |
| `machine.rebuild`, `machine.boot` (level 4) | a new chip: the wall clock | the family's defaults | idle |
| `checkpoint.load` | the saved value | the saved bytes | the saved state |

The RTC is battery-backed and is not on any board's `/RESET` net, so no
reset or power cycle touches the counter or the PRAM: they survive for the
hardware's own reason, because nothing rebuilds the chip. What a reset does
reach is the **serial interface**: when VIA1 is reset it stops driving
port B, the RTC's `/CE` line floats to its pull-up, and the chip abandons
any transfer the guest left half-done. `rtc_deselect` models that edge
(`system_reset_common_devices` calls it right after `via_reset`): the
command and shift state go back to waiting for a command byte, the data
line is released, and the chip's idea of the clock line goes back to low.
That last part matters — `rtc_input` acts only on a rising clock edge, so a
chip still remembering the guest's last HIGH swallows the ROM's first
clock pulse after the reset, frames every later bit one place off, and the
ROM's own RTC test fails.

## The PRAM store

PRAM is initialised at construction from the family's `pram_defaults_t`
(`src/machines/runtime/pram_defaults.c`): the XPRAM validity token, the
Start Manager table, the measured cold MMFlags. The Open Firmware machines
(TNT, the beige G3) are the exception: Mac OS keeps its PRAM in their 8 KB
NVRAM, not in Cuda, so the same defaults are applied to the NVRAM's PRAM
partition instead (`of_nvram.c`) and the RTC's own PRAM starts zero. `rtc_pram_reset` returns
the store to exactly that state, ignoring write-protect, for a hardware
reset of the store: the Network Server's fail-safe red button
(`machine.board.reset_button()`), which resets parameter RAM but not
NVRAM.

The object surface is `machine.rtc` (`time`, `read_only`) and
`machine.rtc.pram` (`peek`, `poke`, `dump`, `snapshot`, `restore`,
`validate`, `boot_device`). A `poke` honours the guest's write-protect bit
the way the chip's command stream does; `rtc.pram.restore` on a running
machine is refused while the guest has protected the store.

## References

1. Apple Computer, *Guide to the Macintosh Family Hardware*, 2nd ed.
   (1990), chapter 3 ("The Real-Time Clock") and Table 14-2.
2. Apple Computer, *Network Server Hardware Developer Notes*, §2.7
   ("NVRAM").
