-- Converted from test.script by scripts/script-to-lua.py
-- The AV MACE register map (see config.mk): register n at $50F1C000 + n*$10.

local mace = 0x50F1C000

-- CHIPID[7:0] / [15:8] at 16 / 17: the Curio's Am79C940, $0940
assert(machine.memory.peek.b(mace + 0x100) == 0x40, F"CHIPID low byte at register 16 reads ${machine.memory.peek.b(mace + 0x100):02x}")
assert(machine.memory.peek.b(mace + 0x110) == 0x09, F"CHIPID high byte at register 17 reads ${machine.memory.peek.b(mace + 0x110):02x}")
-- MPC (24) and the reserved register 25 are not the chip ID
assert(machine.memory.peek.b(mace + 0x180) == 0x00, F"MPC (register 24) reads ${machine.memory.peek.b(mace + 0x180):02x}")
assert(machine.memory.peek.b(mace + 0x190) == 0x00, F"reserved register 25 reads ${machine.memory.peek.b(mace + 0x190):02x}")

-- The configuration registers latch at their data-sheet slots, and the chip
-- ID ignores a write
machine.memory.poke.b(mace + 0xB0, 0x20)
machine.memory.poke.b(mace + 0xC0, 0x5C)
machine.memory.poke.b(mace + 0xE0, 0x01)
machine.memory.poke.b(mace + 0x100, 0x55)
assert(machine.memory.peek.b(mace + 0xB0) == 0x20, "BIUCC (register 11) did not latch")
-- FIFOCC: the watermarks latch; XMTFWU/RCVFWU (bits 3/2) load them and
-- clear themselves (Am79C940 data manual, FIFOCC, p. 66)
assert(machine.memory.peek.b(mace + 0xC0) == 0x50, F"FIFOCC (register 12) reads ${machine.memory.peek.b(mace + 0xC0):02x}, not the latched watermarks with the update bits cleared")
assert(machine.memory.peek.b(mace + 0xE0) == 0x01, "PLSCC (register 14) did not latch")
assert(machine.memory.peek.b(mace + 0x100) == 0x40, "CHIPID took a write")

echo("av-mace-regs: OK")
quit()
