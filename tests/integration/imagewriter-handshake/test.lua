-- Converted from test.script by scripts/script-to-lua.py
-- The script is the host: see config.mk.  On the Plus, SCC channel B's
-- control register is written at 0xBFFFF9 and read at 0x9FFFF8, its data
-- register written at 0xBFFFFD.  RR0 bit 2 is "transmit buffer empty", bit 5
-- the CTS input, set while the printer is busy (the Mac's HSKi is inverted).

machine.imagewriter.connection = "serial-b"
machine.imagewriter.buffer_model = "2k"
scheduler.run(30000000)
assert((machine.memory.peek.b(0x9FFFF8) & 0x20) == 0, "an idle printer is busy")

-- WR4: x16 clock, one stop bit, no parity; WR5: 8-bit characters, transmitter on
machine.memory.poke.b(0xBFFFF9, 4)
machine.memory.poke.b(0xBFFFF9, 0x44)
machine.memory.poke.b(0xBFFFF9, 5)
machine.memory.poke.b(0xBFFFF9, 0x6A)

-- Lines of 62 letters, CR LF
local sent = 0
local busy_seen = 0
local i = 0
while sent < 4000 and i < 400000 do
    local rr0 = machine.memory.peek.b(0x9FFFF8)
    if (rr0 & 0x20) == 0 and (rr0 & 0x04) ~= 0 then
        local b = 0x41 + sent % 26
        if sent % 64 == 62 then
            b = 0x0D
        end
        if sent % 64 == 63 then
            b = 0x0A
        end
        machine.memory.poke.b(0xBFFFFD, b)
        sent = sent + 1
    else
        if (rr0 & 0x20) ~= 0 then
            busy_seen = busy_seen + 1
        end
        scheduler.run(1000)
    end
    i = i + 1
end
echo(F"host: ${sent} bytes sent, CTS busy seen ${busy_seen} times; printer: ${machine.imagewriter.buffer_waits} waits, peak ${machine.imagewriter.buffer_peak}, ${machine.imagewriter.dropped} dropped, ${machine.imagewriter.buffered} still buffered")
assert(sent == 4000, "the host could not send everything")
assert(machine.imagewriter.buffer_waits > 0, "the 2 KB buffer never filled")
assert(busy_seen > 0, "the host never saw the printer busy")
assert(machine.imagewriter.buffer_peak <= 2048, "the buffer held more than 2 KB")
assert(machine.imagewriter.dropped == 0, "bytes were lost")

-- The printer catches up, then the job ends on idle
local n = 0
while machine.imagewriter.jobs < 1 and n < 300 do
    scheduler.run(1000000)
    n = n + 1
end
echo(F"printed: ${machine.imagewriter.bytes} bytes, ${machine.imagewriter.last_job_pages} page(s), ${machine.imagewriter.last_outcome}, crc ${machine.imagewriter.last_pdf_crc}")
assert(machine.imagewriter.jobs == 1, "the job did not end")
assert(machine.imagewriter.bytes == 4000, "not every byte was printed")
assert(machine.imagewriter.dropped == 0, "bytes were lost")
assert((machine.memory.peek.b(0x9FFFF8) & 0x20) == 0, "the printer is still busy")
assert(machine.imagewriter.last_pdf_crc == 0xaaff996e, "the PDF differs from the golden")

echo("PASS: imagewriter-handshake")
quit()
