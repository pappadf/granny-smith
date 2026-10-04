# The Apple ImageWriter and ImageWriter II

The ImageWriter (1983) and ImageWriter II (1985) are Apple's serial dot-matrix
printers for the Apple II, the Lisa and the Macintosh. Both have a nine-wire
print head and speak one command language, inherited from the C. Itoh 8510:
ESC commands with fixed-length ASCII decimal parameters, and graphics sent as
8-dot columns. The ImageWriter II adds colour (a four-band ribbon), three print
qualities, MouseText and custom characters, software switches, and an option
slot that takes the LocalTalk Option card, which puts the printer on an
AppleTalk network as a PAP server. This page describes the printers as a host
sees them: the serial interface, the command set, the dot geometry, the
resident character sets, and the LocalTalk card's protocol.

**Contents:** 1. Overview · 2. Serial interface · 3. Switches · 4. The
command language · 5. Dots, paper and colour · 6. Character sets · 7. Self
ID · 8. The LocalTalk Option card · 9. What host drivers send · 10. Open
questions · Appendix A. Granny Smith implementation notes · References

---

## 1. Overview

| | ImageWriter | ImageWriter II |
|---|---|---|
| Introduced | 1983 | 1985 |
| Print head | 9 wires, 1/72 in apart | 9 wires, 1/72 in apart |
| Interface | RS-232, DB-25 | RS-422, mini-DIN-8; option slot |
| Rates | 300, 1200, 2400, 9600 baud | 300, 1200, 2400, 9600 baud, 8N1 |
| Colour | no | four-band ribbon (yellow, magenta, cyan, black) |
| Qualities | one | draft, correspondence, near letter quality (NLQ) |
| Buffer | 2 KB | 2 KB; 32 KB with the memory option |

Vertical motion is in 1/144 in units; horizontal positions are dot columns at
the current pitch's dot spacing [1] ch. 4–5.

## 2. Serial interface

The ImageWriter II's connector is the Macintosh-style mini-DIN-8; its
handshake output is DTR, high (ready) while the printer accepts data [2]
App. C. On a Macintosh the printer's DTR reaches the computer's HSKi, which
the Macintosh delivers uninverted to the SCC's /CTS input [3] Table 10-1, so a
ready printer reads as CTS *not* asserted in the SCC's RR0. On the Lisa the
printer's DTR is the port's DSR, which the Lisa delivers to the SCC's /SYNC
input (asserted when ready) [4] §15.

With DIP switch 2-3 closed the printer uses XON/XOFF instead: DC3 ($13) when
it cannot take more, DC1 ($11) when it can again [1] Table A-3.

Taking the printer off line with the front panel's SELECT button drops DTR;
data already in the buffer stays there and prints when SELECT is pressed
again. With the software select response enabled (soft switch A-5 open), the
host's DC3 deselects the printer and DC1 selects it [1] ch. 6.

## 3. Switches

### 3.1 DIP switches (ImageWriter II)

Bit numbering used below: switch *x*-*n* closed. Defaults per [1] Table A-3
and [2] App. C.

| Switch | Function | Settings (O open, C closed) |
|---|---|---|
| 1-1 … 1-3 | Language | OOO American, COO Italian, OCO Danish, CCO British, OOC German, COC Swedish, OCC French, CCC Spanish |
| 1-4 | Form length | O 11 in, C 12 in |
| 1-5 | Perforation skip | O off, C on |
| 1-6, 1-7 | Pitch at power-on | OO 10 cpi, CO 12 cpi, OC 17 cpi, CC 160 dpi proportional |
| 1-8 | LF after CR | O no, C yes |
| 2-1, 2-2 | Rate | OO 300, CO 1200, OC 2400, CC 9600 |
| 2-3 | Protocol | O hardware (DTR), C XON/XOFF |
| 2-4 | Option card | C when the LocalTalk or memory option is fitted |
| 2-5, 2-6 | Hammer timing | factory set |

The original ImageWriter's bank 1 is laid out the same way except that its
switch 1-5 selects whether the eighth data bit is ignored [5].

### 3.2 Software switches (ImageWriter II)

`ESC D a b` closes and `ESC Z a b` opens the switches whose bits are set in
the raw bytes *a* (register A) and *b* (register B) [1] Tables A-4, A-5. Only
A's bits $F7 and B's bits $25 exist; the others are ignored. Bit 0 of A is
A-1, bit 7 is A-8; likewise for B.

| Switch | Bit | Closed (D) | Open (Z) | Power-on |
|---|---|---|---|---|
| A-1 … A-3 | $07 | language as DIP 1-1 … 1-3 | | per DIP 1-1 … 1-3 |
| A-5 | $10 | software select response disabled | enabled | disabled |
| A-6 | $20 | LF when the line is full | no LF | no LF |
| A-7 | $40 | CR, LF and FF print the line | only CR prints | CR, LF, FF |
| A-8 | $80 | LF after CR | CR only | per DIP 1-8 |
| B-1 | $01 | slashed zero | unslashed | unslashed |
| B-3 | $04 | no perforation skip | skip | per DIP 1-5 |
| B-6 | $20 | eighth data bit ignored | included | ignored |

Applesoft BASIC sets bit 7 of every byte it sends, so its `ESC D`/`ESC Z`
also change A-8 [1] ch. 3.

## 4. The command language

### 4.1 Parsing

- A numeric parameter is a fixed number of ASCII digits; a leading space
  counts as zero [1] ch. 4.
- `ESC D`, `ESC Z` take two raw bytes; `ESC R nnn c` and `ESC V nnnn c` end
  in one raw byte.
- Graphics data (`ESC G`, `ESC S`, `ESC g`) is a counted run of raw bytes:
  bytes that look like ESC, CR or CAN in it are columns, not commands. A
  host that may have left a graphics command unfinished therefore opens a
  job with enough CANs to complete it (§9.1).
- An unknown ESC command consumes only its command byte.
- With the eighth bit ignored, bit 7 is cleared before anything else looks at
  the byte, so $80–$9F act as control characters.

### 4.2 Line buffering

Printable characters, graphics and attribute changes are collected in a line
buffer and put on paper by a print command: CR, LF or FF (only CR with A-7
open), or the line filling up. CAN erases the buffered line. A command reaches
the printer's attention only when the line containing it is processed, which
is why `ESC ?` (§7) is answered late on a busy printer [1] ch. 6.

### 4.3 Control characters

| Byte | Action |
|---|---|
| BS $08 | The next character backs up by its own width and is printed over the previous one |
| HT $09 | Next horizontal tab stop |
| LF $0A | Print the line; feed one line (reverse after `ESC r`); a CR first unless `ESC l 1` |
| VT $0B | ImageWriter II: a fixed 2/144 in feed. ImageWriter: next vertical tab stop |
| FF $0C | Print the line; feed to the next top of form |
| CR $0D | Print the line; head to the left margin; LF too if A-8 / DIP 1-8 |
| SO $0E / SI $0F | Double width on / off (characters and graphics) |
| DC1 $11 / DC3 $13 | Select / deselect (software select response only) |
| CAN $18 | Erase the line not yet printed |
| US $1F *n* | Feed *n* lines, *n* = '1' … '9', ':' … '?' (1 … 15) |

### 4.4 ESC commands

| Command | Parameters | Function |
|---|---|---|
| `ESC n N E e q Q` | — | Pitch 9, 10, 12, 13.4, 15, 17 cpi; dot spacing 72, 80, 96, 107.2, 120, 136 dpi |
| `ESC p P` | — | Proportional, 144 / 160 dpi |
| `ESC s n`, `ESC 1` … `ESC 6` | 1 / — | Proportional dot spacing; insert 1–6 dots |
| `ESC a n`, `ESC m`, `ESC M` | 1 / — | Quality: 0 correspondence, 1 draft, 2 NLQ (`m` = 0, `M` = 2) |
| `ESC X Y`, `ESC ! "` | — | Underline on/off, bold on/off |
| `ESC w W`, `ESC x y z` | — | Half height on/off; superscript, subscript, off |
| `ESC & $` | — | MouseText on; standard characters (also ends custom characters) |
| `ESC ' *` | — | Custom characters; high custom set through low codes |
| `ESC - +`, `ESC I` … EOT | var | Custom width 8 / 16 dots (erases the set); load custom characters (§6.3) |
| `ESC L nnn` | 3 | Left margin at character column *nnn* |
| `ESC ( nnn,…,nnn.`, `ESC ) …`, `ESC u nnn`, `ESC 0` | var / 3 / — | Set, clear, add tab stops; clear all |
| `ESC F nnnn` | 4 | Head to dot column *nnnn* past the left margin |
| `ESC R nnn c` | 3+1 | Repeat character *c* |
| `ESC > <` | — | Unidirectional / bidirectional |
| `ESC A B`, `ESC T nn` | — / 2 | Line spacing 1/6, 1/8, *nn*/144 in |
| `ESC f r` | — | Line feed forward / reverse |
| `ESC H nnnn`, `ESC v` | 4 / — | Form length *nnnn*/144 in; top of form here |
| `ESC l n` | 1 | 0: CR before LF and FF (default); 1: none |
| `ESC o O` | — | Paper-out sensor on / off |
| `ESC G nnnn`, `ESC S nnnn`, `ESC g nnn` | 4 / 4 / 3 | *nnnn* (or *nnn* × 8) graphics columns follow |
| `ESC V nnnn c` | 4+1 | Repeat graphics column *c* |
| `ESC K n` | 1 | Ribbon colour (§5.3) |
| `ESC D a b`, `ESC Z a b` | 2 raw | Close / open software switches (§3.2) |
| `ESC c` | — | Print the buffer, then reset the software settings ([1] Table A-2): top of form and custom characters stay |
| `ESC ?` | — | Send the ID string (§7) |

Power-on defaults are [1] Table A-1: draft quality, pitch per DIP, 6 lines
per inch, top of form at the print line, form length per DIP 1-4, black.

## 5. Dots, paper and colour

### 5.1 Geometry

A graphics byte is one column of eight dots, bit 0 the top wire. Wires are
1/72 in apart, so a line feed of 16/144 in puts the next band directly below;
a 1/144 in feed between two passes interleaves them, doubling the vertical
resolution [1] ch. 8. Every fixed pitch prints characters eight dot columns
wide; only the column spacing differs. The ninth wire prints the underline
and the descenders (§6.1).

### 5.2 Paper motion

Top of form is the print line at power-on (or where `ESC v` was given); a
form feed moves to the next top of form, a whole form if already there. The
manual sets top of form half an inch below the top of the sheet [1] ch. 2.
With perforation skip on, a line feed that ends within half an inch of the
end of a form continues to the next top of form [1] ch. 2.

### 5.3 Colour

`ESC K n` selects the ribbon band: 0 black, 1 yellow, 2 magenta, 3 cyan. The
secondary colours are printed by striking each dot through two bands in turn:
4 orange (yellow, magenta), 5 green (yellow, cyan), 6 purple (magenta, cyan)
[1] Table A-18. Host drivers print a colour image as one pass per band.

## 6. Character sets

### 6.1 Encoding of the resident glyphs (ImageWriter II)

The ImageWriter II's firmware holds its draft and correspondence character
sets as tables of dot columns, one byte per column, bit 0 the top wire [6]:

- **Correspondence:** eight columns per character (seven of shape, one of
  space), codes $20–$7F, then the 32 MouseText characters, then 28
  alternate-language characters.
- **Draft:** twelve bytes per character, one per *half* dot column: draft
  never fires a wire in two neighbouring half columns, which is what lets the
  head print it fast. The same code ranges, in the same order.

In both, a byte with bit 7 set means its bits 0–6 print two wires lower
(wires 3–9): that is how the descenders of g, j, p, q, y, the comma and
semicolon, and the underscore reach wire 9 [6], [1] App. C. The NLQ and
proportional sets are held separately and are not described here (§10).

### 6.2 Languages

The language (DIP 1-1 … 1-3, soft switches A-1 … A-3) replaces up to ten
codes [1] Table A-8, App. C:

| Language | # | @ | [ | \ | ] | \` | { | \| | } | ~ |
|---|---|---|---|---|---|---|---|---|---|---|
| American | # | @ | [ | \ | ] | \` | { | \| | } | ~ |
| Italian | £ | § | ° | ç | é | ù | à | ò | è | ì |
| Danish | # | @ | Æ | Ø | Å | \` | æ | ø | å | ~ |
| British | £ | @ | [ | \ | ] | \` | { | \| | } | ~ |
| German | # | § | Ä | Ö | Ü | \` | ä | ö | ü | ß |
| Swedish | # | @ | Ä | Ö | Å | \` | ä | ö | å | ~ |
| French | £ | à | ° | ç | § | \` | é | ù | è | ¨ |
| Spanish | £ | § | ¡ | Ñ | ¿ | \` | ° | ñ | ç | ~ |

### 6.3 MouseText and custom characters

`ESC &` maps codes $40–$5F onto the 32 MouseText characters; with the eighth
bit included, $C0–$DF print them too [1] ch. 4. Custom characters are loaded
with `ESC I`, then for each character a key (a code $20–$7E, or $A0–$EF with
the 8-dot maximum), a width code (`A`–`P`: 1–16 columns on wires 1–8;
`a`–`p`: the same on wires 2–9) and that many column bytes; EOT ($04) ends the
load [1] ch. 7, Tables 7-1, 7-2. `ESC '` prints them for their own codes,
`ESC *` reaches the high set through codes $20–$6F.

## 7. Self ID

`ESC ?` makes the ImageWriter II send `IW10`, then `C` if a colour ribbon is
fitted and `F` if a SheetFeeder is, at the current rate with the eighth bit
0 [1] Table 6-7. `10` is the 10-inch carriage. The Macintosh ImageWriter
driver sends `ESC ?` at the start of every print (observed, System 6.0.8,
driver 2.7).

## 8. The LocalTalk Option card

The ImageWriter II's LocalTalk Option card is a PAP server in the option slot
([7] ch. 10; [8] for PAP). It registers the NBP name `<name>:ImageWriter@*`
(observed: System 6.0.8's AppleTalk ImageWriter driver looks up type
`ImageWriter`), takes one connection at a time, reads the job with SendData,
and prints the data, which is the same byte stream a serial cable carries.
The workstation's EOF ends the job. DIP switch 2-4 must be closed.

Where a LaserWriter answers OpenConn and SendStatus with a status string,
the card answers with a **statusBits** word [9]. The status data starts at
offset 4 of the ATP data (offsets 0–3 of a Status response are unused; of an
OpenConnReply they are the responding socket, the flow quantum and the
result). It is a length byte of 2 followed by the word, high byte first
(observed: with bit 13 set, System 6.0.8's driver reports the printer out of
paper). Bits known [9]:

| Bit | Meaning |
|---|---|
| 14 | SheetFeeder installed |
| 13 | Paper out |
| 10 | Paper jam (reported for running out of paper when a SheetFeeder is fitted) |

The note warns that the card sometimes returns all ones in the high byte,
which the driver must treat as invalid [9]. The driver does not poll status
while it prints: it reads the OpenConnReply's word (observed).

## 9. What host drivers send

### 9.1 Lisa Office System

The Office System's ImageWriter driver opens each job with a run of CANs (one
more than the widest graphics line, 1280 columns at 160 dpi) to complete any
graphics command an aborted job left unfinished, sets its pitch and line
spacing, and prints every page as graphics bands with `ESC F` positioning,
`ESC g` data and 1/144 in interleave feeds. It reverses the paper with
`ESC r` and follows each reverse feed with a forward "burp" against gear
lash, and assumes the print line is 80/144 in below the top of the sheet at
top of form [10] (`LibPr`). It ends with `ESC c`.

### 9.2 Macintosh

Observed with System 6.0.8, ImageWriter driver 2.7 (serial) and AppleTalk
ImageWriter 2.7: after `ESC ?` the driver sets the form length (`ESC H1584`)
and backs the paper up 76/144 in before the first line. *Faster* prints
72 dpi bands (`ESC N`, `ESC G` up to 640 columns); *Best* prints at 160 dpi
(`ESC P`) in two passes 1/144 in apart; *Draft* sends text, placing each word
with `ESC F` and choosing `ESC q` or `ESC Q` for its width. Each band is
preceded by `ESC K0`.

## 10. Open questions

- The remaining statusBits (NW20's Figure 5 is an image not yet
  transcribed), and whether the card's flow quantum is 8.
- The NLQ glyph tables (two passes 1/144 in apart) and the proportional
  sets' widths in the ImageWriter II firmware.
- The original ImageWriter's character generator, and whether it answers
  `ESC ?`.
- The undocumented ImageWriter II control GS ($1D, a vertical-format
  command) and the original ImageWriter's vertical format unit programming.
- Exact half-height, superscript and subscript dot placement.
- Where column 0 lies on the sheet (a quarter inch from the left edge is
  assumed for a centred 8.5 in sheet).

## Appendix A. Granny Smith implementation notes

Granny Smith models the printer at the command level, not the firmware:
see [internals/core/printer/iw_printer.md](../../internals/core/printer/iw_printer.md).

## References

1. Apple Computer, *ImageWriter II Technical Reference Manual* (1985): ch. 2–8, App. A (Tables A-1 … A-19), App. C.
2. Apple Computer, *ImageWriter II Owner's Manual* (1985), App. C.
3. Apple Computer, *Guide to the Macintosh Family Hardware*, 2nd ed. (1990), ch. 10, Table 10-1.
4. [lisa.md](../machines/lisa/lisa.md) §15 (serial ports).
5. Apple Computer, *ImageWriter User's Manual, Part 1: Reference* (1983).
6. ImageWriter II firmware ROM (32 KB, uPD7810), character tables: disassembly and table analysis.
7. Apple Computer, *ImageWriter II/LQ LocalTalk Option User's Guide* (1988).
8. Gursharan S. Sidhu, Richard F. Andrews, Alan B. Oppenheimer, *Inside AppleTalk*, 2nd ed. (1990), ch. 10; [pap.md](../protocols/pap.md).
9. Apple Computer, Technical Note NW20, "PAP Status Buffer" (1990).
10. Lisa Office System 3.1 source, printer library (released by the Computer History Museum).
