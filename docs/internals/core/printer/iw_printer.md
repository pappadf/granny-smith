# The virtual ImageWriter

`machine.imagewriter` is a dot-matrix printer per machine: an ImageWriter or
ImageWriter II that the guest's own printer driver prints to, over a serial
port or (an ImageWriter II with the LocalTalk Option card) over AppleTalk.
Each job becomes a PDF that the platform keeps: headless writes it to
`--print-dir`, the browser opens it in the print viewer. The hardware it
models is [reference/hardware/imagewriter.md](../../../reference/hardware/imagewriter.md).

**Contents:** 1. Shape · 2. The interpreter · 3. Sheets and the PDF ·
4. Glyphs · 5. Front ends · 6. Jobs · 7. Output · 8. Object model ·
9. Checkpoints · 10. Tests · 11. Not done yet

## 1. Shape

The printer is modelled at the command level, not by running its firmware:
one interpreter for the byte stream, whichever front end delivers it.

```
 guest driver ──SCC async──► scc.c port device ─┐
 guest driver ──LLAP/ATP/PAP──► appletalk_imagewriter.c ─┤
 script ──feed() / feed_file()───────────────────────────┤
                                                         ▼
                   iw_printer.c   device: settings, connection, jobs, object, checkpoint
                     iw_interp.c  parser, commands, line buffer, paper motion
                     iw_page.c    one sheet: 1-bit planes per ribbon band
                     iw_font.c    glyph lookup (iw_font_data.c, generated)
                                                         ▼ finished sheets
                   pdf_writer.c   one image per page, FlateDecode, deterministic
                                                         ▼ printer_document_t
                   printer_sink.h  platform: headless file / browser viewer
```

| File | Role |
|---|---|
| `src/core/printer/iw_interp.{c,h}` | The command language: state (`iw_state_t`, plain data), parser state machine, command execution, line staging, paper motion, sheet lifecycle |
| `src/core/printer/iw_page.{c,h}` | One sheet: a 1-bit plane per ribbon band at 288 or 576 dpi, allocated on the first dot; dot stamping; emission to the PDF writer; serialisation for checkpoints |
| `src/core/printer/iw_font.{c,h}`, `iw_font_data.{c,h}` | Glyphs: the generated ImageWriter II tables (draft, correspondence, NLQ, both proportional sets), a built-in 5×7 placeholder where no table exists |
| `src/core/printer/pdf_writer.{c,h}` | A small PDF 1.4 writer: pages added as 1-bit stencils or 4-bit indexed images, compressed as they come; no dates, so the same pages give the same bytes |
| `src/core/printer/printer_sink.{c,h}` | The platform sinks (`printer_sink_document`, `_capture`, `_status`) with weak defaults |
| `src/core/printer/iw_printer.{c,h}` | The device: settings, connection, input queue and scheduler events, jobs, the object model, the checkpoint part |
| `src/core/network/appletalk_imagewriter.c` | The LocalTalk Option card's PAP server |

## 2. The interpreter

`iw_interp_feed()` runs bytes through a state machine (`IW_PS_*`): normal,
after ESC, collecting a command's fixed-length parameters, a tab list, a
counted graphics run, `US n`, and the three states of a custom-character load.
Graphics bytes are counted, never interpreted, so the Lisa Office System's
opening CAN run completes a graphics command an aborted job left unfinished.
The eighth bit is stripped before dispatch when soft switch B-6 (IW II) or DIP
1-5 (IW I) says so; raw parameter bytes (`ESC D a b`, `ESC R nnn c`) and
graphics data are taken before stripping.

**Positions are exact.** Horizontally the unit is 1/1640160 in, the least
common multiple of every dot pitch including 107.2 dpi (= 536/5); a column's
pixel is computed from its exact position once, so long lines and colour
passes do not drift. Draft glyphs are placed in half dot columns, each column
computed from the character's start. Vertically the unit is 1/144 in, which
lands on whole pixels at 288 and 576 dpi.

**Line staging.** Printable characters and graphics become staged *marks*
(`iw_mark_t`: position, 9-bit wire pattern, ribbon mask, wire spacing, vertical
offset). CR, LF and FF (only CR with A-7 open) commit the staged line to the
sheet at the current paper position; CAN erases it. This is what makes CAN,
backspace overprinting and "print, CR, print again" behave as on the printer.

**Paper.** `y` is the print line in 1/144 in from the start of the job's paper;
`tof` is top of form; `page_top` the top edge of the sheet being printed.
Power-on puts the print line `tof_offset` (default 80/144 in) below the
sheet's top edge — where both the Lisa and the Macintosh drivers expect it —
and a sheet is the form length long. A sheet is finished lazily, when a line
is committed below it or the job ends, so reverse feeds within a sheet work;
the paper cannot roll back above the sheet being printed. Blank sheets are
never emitted. Perforation skip and cut-sheet mode follow imagewriter.md §5.2.

## 3. Sheets and the PDF

A sheet has one plane on an ImageWriter, four (Y, M, C, K) on an ImageWriter
II with a colour ribbon. A dot is a disc of 1/72 in (4 px at 288 dpi) or, with
`dot_shape = "square"`, a square, stamped into each plane its ribbon mask
names: orange is a dot in the yellow and the magenta plane. A black-only sheet
goes into the PDF as a 1-bit `/ImageMask`; a coloured one as a 4-bit
`/Indexed` image whose index *is* the ribbon mask, with a 16-entry palette
mixed subtractively from the ink colours (any mix with black is black).

## 4. Glyphs

`iw_font_data.c` is generated by a script kept with the project's private
evidence (it reads the ImageWriter II firmware; the file holds only re-encoded
9-bit column tables, no ROM image or code). It covers every ImageWriter II
set: draft and NLQ in half columns (NLQ with its second pass, staged 1/144 in
lower), correspondence, and the correspondence and NLQ proportional sets with
each glyph's own width; MouseText and the 28 alternate-language glyphs with
the per-language substitution table. Draft with a proportional pitch prints
correspondence proportional; MouseText, which has no proportional shapes,
prints its fixed shape 16 proportional columns wide. The original
ImageWriter prints with the placeholder; a job that used it ends with
`last_outcome = "ok (placeholder font)"`.
Bold strikes each column (both NLQ passes) again 1/160 in to the right; double width doubles
each column; underline is wire 9 under the whole advance; half height and the
scripts halve the wire spacing.

## 5. Front ends

**Serial.** `connection = "serial-a"` / `"serial-b"` plugs the printer into
an SCC channel as a port device (`scc_attach_port_device`, scc.md "The far
end of a port"). The SCC hands each asynchronous byte over inside the
guest's write to WR8; the printer only queues it and interprets the queue
from a scheduler event 1 ms of guest time later. The printer's DTR drives the
port's ready line: on the Lisa the machine wires Serial A's /SYNC itself; on
a Macintosh the printer wires /CTS (not asserted when ready) on attach and
unwires it on detach. With DIP 2-3 closed it sends DC3/DC1 instead. The
`ESC ?` reply goes back byte by byte through `scc_port_rx_byte`.

Front-panel deselect (`selected = false`) and `paper_out` drop the ready line
and pause the printer: its queue holds what arrived, the idle timer waits,
and selecting again prints the rest — a host DC3 instead makes the
interpreter ignore input until DC1, as the printer does.

**LocalTalk.** `connection = "localtalk"` (ImageWriter II only) publishes
the card: `appletalk_imagewriter.c` registers `localtalk_name:ImageWriter@*`
on its own PAP socket (9), separate from the LaserWriter's server
(appletalk_printer.c), so both printers can be on the network together. It
accepts one connection, pulls data with SendData (flow quantum 8, 10 ms of
guest time between transactions, held while the printer is paused), feeds
each Data packet to `iw_printer_feed()`, and ends the job at the EOF. It
answers OpenConn and SendStatus with the statusBits payload (imagewriter.md §8)
and holds the workstation's own reads until the job ends, then answers them
with EOF. Nothing travels back over AppleTalk for `ESC ?`.

## 6. Jobs

A job starts with the first byte after idle. It ends after `idle_timeout_ms`
(default 5000) of guest time with no serial input — drivers rarely end with a
form feed — at the PAP EOF, on `eject()`, or when the connection changes.
A run of 16 CANs at the start of a line on a job that has printed starts a new
job (the Office System opens every job that way). A job that put nothing on
paper (the Macintosh driver's lone `ESC ?`) makes no document and gives its
number to the next job. The interpreter's settings persist from job to job,
as a printer keeps them until reset.

## 7. Output

At the end of a job the PDF goes to `printer_sink_deliver()`:

- **Headless** (`headless_main.c`): `<print-dir>/<slug>-<job:05>-<title>.pdf`
  (`imagewriter2-00001-Print.pdf`), and with `capture` the job's raw input as
  `<slug>-<job:05>.iw` beside it. The title is `Print`: the serial stream
  carries none.
- **Browser** (`em_main.c`): the bytes go to the page over the download path
  (I/O job, transfer buffer, `download_chunk` events) with
  `kind: "document"`, the printer's name, job, pages and title;
  `bus/download.ts` opens them in the print viewer, or saves them where the
  browser shows no PDF inline. Status changes are the LaserWriter's
  `printer_status` event with a `printer` field (docs/guide/web.md).

## 8. Object model

`machine.imagewriter` (every machine with an SCC): `model`
(`imagewriter` / `imagewriter2`), `connection`, `localtalk_name`, `status`,
`busy`, `dip1`/`dip2`, `color_ribbon`, `sheet_feeder`, `paper`
(`fanfold-letter`, `letter`, `a4`, `legal`, `fanfold-15in`), `paper_mode`,
`idle_timeout_ms`, `resolution`, `dot_shape`, `tof_offset`, `selected`,
`paper_out`, `capture`; counters `jobs`, `pages`, `bytes`, `last_job_pages`,
`last_outcome`, `last_pdf_crc`, `last_pdf_bytes`; read-outs `pitch`,
`line_spacing`, `soft_switches`; methods `eject()`, `reset()`, `feed(data)`,
`feed_file(path)`. `machine.scc.<ch>.device` names a device plugged into a
channel. `help machine.imagewriter` has the full text.

## 9. Checkpoints

The printer is a machine part (`machine_part_imagewriter`, after the SCC and
AppleTalk): its settings and job state, the interpreter state as one block,
the sheet in progress (planes deflated), the document so far (the PDF
writer's buffer), the input queue and the capture. Its scheduler events
(`imagewriter.process`, `.idle`, `.reply`) are registered at construction so
a restore re-arms them. The connection is configuration: a restore plugs the
printer back into its port or re-publishes the card.

## 10. Tests

| Test | What it covers |
|---|---|
| unit `imagewriter` | parser, switches, tabs, pixel-exact columns for every pitch, interleave, opaque graphics, the CAN recovery, pages, perforation skip, colour planes, self ID, the eighth bit, custom characters, ROM glyphs (draft, NLQ passes, proportional widths), PDF structure, determinism and serialisation |
| unit `scc_port` | the port-device seam: bytes to the device, ready line, RX injection |
| `lisa-imagewriter` | LOS 3.1 prints the Calculator tape on Serial A; PDF golden |
| `mac-imagewriter` | a Plus with System 6.0.8 prints Faster, Best (deselected mid-job) and Draft on the printer port; PDF goldens |
| `appletalk-imagewriter` | the same over the LocalTalk card, with paper-out alert and recovery |
| `imagewriter-checkpoint` | a checkpoint mid-job restores to the same PDF |
| e2e `imagewriter-print` | a job opens in the browser's print viewer |

The goldens are CRCs (`last_pdf_crc`): every output is deterministic.

## 11. Not done yet

The original ImageWriter's character generator and its vertical format unit; a simulated buffer (2 KB / 32 KB)
that would make the handshake bite mid-job; a dedicated printer panel in the
web UI (today the SYSTEM tab edits `machine.imagewriter`); the title from the
guest's foreground application; persistence of the printer settings across
sessions.
