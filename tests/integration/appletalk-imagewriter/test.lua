-- Converted from test.script by scripts/script-to-lua.py
-- The AppleTalk ImageWriter: Chooser, Print Directory over PAP, paper out,
-- and the print after an abandoned job.
-- See config.mk.  Screen coordinates are the Plus's 512x342 desktop.

local mac = dofile("../lib/mac.lua")

local function click(x, y)
    machine.adb.mouse.move(x, y)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.wait_mouse_taken(100000000)
end

-- File -> Print Directory..., then OK (Faster, the dialog's default)
local function print_directory()
    machine.adb.mouse.move(53, 10)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    scheduler.run(4000000)
    machine.adb.mouse.move(100, 203)
    scheduler.run(4000000)
    machine.adb.mouse.click(false)
    scheduler.run(60000000)
    click(448, 62)
end

-- The AppleTalk ImageWriter is on the network with no setup, beside the
-- LaserWriter; the machine's serial printer is a separate one (none here)
assert(appletalk.imagewriter.connection == "localtalk", "the AppleTalk ImageWriter is not on LocalTalk")
assert(appletalk.imagewriter.model == "imagewriter2", "the AppleTalk ImageWriter is not an ImageWriter II")
assert(machine.imagewriter.connection == "none", "the serial printer was plugged in unasked")
assert(appletalk.imagewriter.localtalk_name == "Virtual ImageWriter", "the card's name")

machine.rtc.time = "2026-04-26T21:30:36"
scheduler.run(30000000)

-- Apple menu -> Chooser
machine.adb.mouse.move(25, 10)
scheduler.run(2000000)
machine.adb.mouse.click(true)
scheduler.run(4000000)
machine.adb.mouse.move(25, 123)
scheduler.run(4000000)
machine.adb.mouse.click(false)
scheduler.run(60000000)

-- AppleTalk ImageWriter; "requires AppleTalk" -> OK (makes AppleTalk active)
click(165, 85)
scheduler.run(30000000)
click(187, 206)
scheduler.run(80000000)

-- The card is in the list: select it, close the Chooser
click(300, 88)
scheduler.run(30000000)
click(72, 47)
scheduler.run(40000000)

-- Open the disk's window
machine.adb.mouse.move(470, 52)
scheduler.run(2000000)
machine.adb.mouse.click(true)
machine.adb.mouse.click(false)
mac.wait_mouse_taken(100000000)
scheduler.run(80000)
machine.adb.mouse.click(true)
machine.adb.mouse.click(false)
mac.wait_mouse_taken(100000000)
scheduler.run(40000000)

print_directory()
scheduler.run(300000000)
echo(F"localtalk: ${appletalk.imagewriter.bytes} bytes, jobs ${appletalk.imagewriter.jobs}, ${appletalk.imagewriter.last_job_pages} page(s), ${appletalk.imagewriter.last_outcome}, crc ${appletalk.imagewriter.last_pdf_crc}")
assert(appletalk.imagewriter.jobs == 1, "the print did not become a document")
assert(appletalk.imagewriter.last_job_pages == 1, "the directory listing is one page")
-- The very stream the serial port carries, less the ESC ? query: the same PDF
assert(appletalk.imagewriter.last_pdf_crc == 0x0f4f3a4e, "the PDF differs from the serial port's")
assert(appletalk.imagewriter.busy == false, "the job did not end at the EOF")

-- Out of paper: the driver reads it from the OpenConnReply and waits
appletalk.imagewriter.paper_out = true
print_directory()
scheduler.run(150000000)
assert(appletalk.imagewriter.jobs == 1, "a printer out of paper printed")
assert(appletalk.imagewriter.bytes == 4140, "data was sent to a printer out of paper")
-- Paper in, then the alert's Continue
appletalk.imagewriter.paper_out = false
scheduler.run(2000000)
click(440, 118)
scheduler.run(300000000)
assert(appletalk.imagewriter.jobs == 2, "the job did not print after paper was added")
assert(appletalk.imagewriter.last_pdf_crc == 0x0f4f3a4e, "the resumed job's PDF differs")

-- Deselected: the card's statusBits say so (bit 11), and the driver shows
-- "The 'select' light is off" with Continue instead of sending.  Selected
-- again, the alert waits for Continue -- here over two minutes of guest
-- time, which the connection outlives only because the card tickles the
-- workstation and keeps its SendData going -- and the job prints whole.
appletalk.imagewriter.selected = false
print_directory()
scheduler.run(150000000)
mac.check("select-light-off.png")
assert(appletalk.imagewriter.jobs == 2, "a deselected printer printed")
assert(appletalk.imagewriter.bytes == 8280, "data was sent to a deselected printer")
appletalk.imagewriter.selected = true
scheduler.run(300000000)
click(440, 118)
scheduler.run(300000000)
assert(appletalk.imagewriter.jobs == 3, "the job did not print after the printer was selected again")
assert(appletalk.imagewriter.last_job_pages == 1, "the resumed directory listing is not one page")
assert(appletalk.imagewriter.last_pdf_crc == 0x0f4f3a4e, "the resumed job's PDF differs")

-- Cancelled at that alert, the job is given up ("could not be printed") and
-- makes no document; the next one starts at top of form: the same one-page
-- document, not one shifted down the page onto two.
appletalk.imagewriter.selected = false
print_directory()
scheduler.run(150000000)
click(440, 91)
scheduler.run(100000000)
appletalk.imagewriter.selected = true
assert(appletalk.imagewriter.jobs == 3, "the cancelled job became a document")
click(113, 167)
scheduler.run(50000000)
print_directory()
scheduler.run(300000000)
assert(appletalk.imagewriter.jobs == 4, "the print after the cancelled job did not become a document")
assert(appletalk.imagewriter.last_job_pages == 1, "the print after the cancelled job is not one page")
assert(appletalk.imagewriter.last_pdf_crc == 0x0f4f3a4e, "the print after the cancelled job does not start at top of form")

assert(machine.imagewriter.jobs == 0, "a network job reached the serial printer")
echo("PASS: appletalk-imagewriter")
quit()
