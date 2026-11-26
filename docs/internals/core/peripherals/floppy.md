# Floppy — the IWM/SWIM controller model (`floppy.c`)

`src/core/peripherals/floppy.c` (with `floppy_gcr.c` and `floppy_swim.c`;
the 6-and-2 GCR codec under the track encoder is `src/core/storage/gcr.c`)
models the Macintosh floppy controller in its IWM-only form (Mac Plus) and
the SWIM's dual IWM/ISM form (SE/30 and the II family), together with the
drives behind it. The controller-driven SWIM III and New Age chips have
their own pages ([swim3.md](swim3.md), [new_age.md](new_age.md)). This page
records the model's timing choices that deliberately depart from the drive
specification.

## Register windows

The controller maps nothing itself (`floppy_init` takes no memory map). The
68000 compact boards place either chip with `floppy_map_stride_window`: CPU
A9-A12 select the register (a $200 stride) and the chip sits on the low
byte lane. The IWM's window is `iwm_iface`, which also logs an even-address
byte access and answers a wide access with open bus; the SWIM's is
`floppy_swim_stride_iface`, which hands every access, wide ones included, to
the chip at the decoded register index. Every other SWIM machine decodes its
own window over `floppy_get_memory_interface` (the SWIM's index-addressed
register file).

## A select that reaches no drive

A drive select (the IWM SELECT line, or the ISM's DRIVE1/DRIVE2 enables,
through the board's `floppy_set_enbl_route` steering) can reach no drive:
`FLOPPY_NO_DRIVE`, or a position past the drives the machine cables
(`n_drives`). Every path that acts on the selected drive resolves it through
one chokepoint, `floppy_cabled_drive` / `floppy_cabled_disk`
(`floppy_internal.h`), which return NULL then: the sense lines float high
(/DRVIN "no drive"), commands and data writes go nowhere, a data read finds
no disk.

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

## Ejecting a disk

Every eject ends in `floppy_drive_eject`: the IWM/SWIM eject command, the
SWIM3, the New Age, the IIfx SWIM IOP and the host's `eject()`. It writes the
modified GCR tracks back into the image, drops the track cache, and hands the
image to `config_release_image`, which closes it and frees its slot in the
machine's tracked images. The file is released at once, so it can be copied,
renamed, deleted or replaced; inserting it again opens it afresh. A checkpoint
taken while the disk was in still restores, since the restore reopens the
image from the checkpoint's own image list. The Lisa's Sony drive is not a
`floppy_t` and keeps an ejected diskette open for a faithful reinsert (its
writes and page-label tags, both in that image's delta, then come back with
it).

The write-back reads each modified buffer with the sector scan the MOOF
importer and the capture use (`moof_scan_track`): as a loop, so a sector
running across the buffer's end — a captured track can put one anywhere —
is found like any other. Every sector it finds that reads (a valid data
checksum, every byte a codeword, the data field repeating the sector
number) is written to the image, those the guest did not touch with the
contents they already had. Only an address field naming the track and side
being flushed counts, and a sector number is written once: of two address
fields naming it, the first copy that reads, as the scan of a capture takes
it. So a header a duplicator laid down naming another track, or a decoy
copy, never writes another track's sector or replaces the one the image
holds. A side the medium does not have (a 400K disk's side 1) has nothing
written back. The per-sector write-through as the guest writes a whole
sector (`iwm_write_through`, a format) follows the same rules: the sector it
completes may have started before the index, and it is written only when its
header names the track and side under the head.

## Write protection

`/WRTPRT` reads the image's writability. A disk opened read-only takes no
writes at all: `floppy_iwm_write` drops the byte (the head keeps turning,
so the position still advances) instead of storing it into the track
buffer, where it used to reach the cache and a checkpoint's residue of a
disk the guest could not write. An insert with no explicit `writable=`
opens a MOOF whose INFO chunk records write protection read-only, as the
original disk's tab was ([image.md](../storage/image.md)); `writable=true`
overrides it.

## Tracks: what the GCR read path sees

The IWM and the SWIM in IWM mode read a disk as a stream of disk bytes. Each
drive caches one buffer per side and track (`floppy_track_t`), built on first
touch by `iwm_track_data()` from `floppy_media_track()` (`floppy_gcr.c`) and
read in a loop: one revolution, indexed by the drive's `offset`, which every
data-register read advances (a read takes no emulated time, so the position
is continuous from whoever read last — the ROM's address-field search to a
guest routine reading on from there). The buffer is a cache: what it holds is
a function of the image (and, on a weak track, of the draw — "Weak bits"
below), rebuilt on the next touch after an eject, a format change or a
restore; only bytes the guest wrote that never formed a sector
(the residue) exist nowhere else.

`floppy_media_track()` owns the rule for the bytes:

1. **A plain sector image** (or a side a captured disk does not have, side 1
   of a 400K disk): the track is laid down from the image's sectors and tags
   as a Mac formats it — 2:1 interleave, self-sync before each field — and
   padded with sync to the zone's length (9,320 down to 6,224 bytes).
2. **A bit-level capture** (a MOOF; `src/core/storage/image_capture.h`)
   **whose sectors the image still holds**: the captured revolution itself,
   framed as the IWM frames it in steady state (`gcr_frame_revolution`), so
   gaps, marks and fields that are not sectors reach the guest as they were
   on the disk. "Still holds" is a comparison of contents: every sector the
   capture decodes must have the same data and tag in the image; a sector it
   could not decode must still hold what the importer gave it (zeros, or the
   bytes as read for a failing checksum) and must not have been written
   since (`image_block_in_delta`).
3. **Some sectors changed**: the capture with each changed sector's data
   field replaced — from the end of its captured address field through the
   `DE AA` that closes the data field, the gap becoming sync of the same
   length and the field a standard one encoded from the image. That is what a
   drive does when the driver rewrites a sector; the address field, the
   other fields and the gaps stay as captured. A replacement may grow the
   track (a short non-standard field replaced by a 709-byte one), within 2%
   of the capture's length. A data field with no `DE AA` after its values
   ends where a standard one would, or before the sync of the next field's
   mark if that comes sooner (`moof_scan_track`): the replacement never
   takes another field with it, and one that would cover an address field
   is refused.
4. **Otherwise** (every sector changed — a reformat; a changed sector with no
   data field in the capture; a replacement that would cover an address
   field; growth past 2%): laid down from the sectors as in 1.

The rule reads only the capture and the image's current contents, so a
track is the same whatever path brought the image to its state: a guest
write, the eject flush (which rewrites the sectors of a dirty track that
read, those the guest did not write with contents they already had), a
quick or a consolidated checkpoint restore onto the same capture, a VFS
write, a reinsert. The one input that is not content is the delta bit of a
sector the capture could not read: a guest writing zeros there
leaves the content as it was but would leave a readable sector on the real
disk. A consolidated restore clears the delta bit of a block equal to its
base, so after one such a sector reverts to the capture's unreadable field.
That case — all-zero data and tags written to exactly the sectors a capture
could not read — is accepted.

### Weak bits

A long run of cells with no flux transition reads differently on every
revolution of a real drive: the read amplifier's gain climbs in the
silence and, a few cells in, turns noise into transitions at random, so the
IWM's framing after the run slips differently each time. Weak-bit copy
protection reads such an area once per revolution and requires the reads
to differ; a nibble copy, which writes the noise back as real data, reads
the same every time.

The capture store records each track's **weak runs** when it is built
(`image_capture_weak_runs`): zero runs of at least `IMAGE_WEAK_MIN_RUN`
(12) cells, found on the track as a loop, so a run the index cuts is one
run. Standard captures carry short runs — up to six cells, and one of ten,
on tracks that read correctly as captured — that must stay as they are;
deliberate weak areas run 16 cells and more. The unit suites guard the
threshold against every MOOF in the test corpus.

A cache entry built by rule 2 (the capture as it is, from captured bits)
for a track with weak runs is **weak**: it holds one *draw* of the track
(`image_capture_weak_track`) — the captured bits with every weak run
filled with noise, framed in steady state. Inside a run, a cell more than
three cells after the last transition (captured or invented) becomes a 1
with probability 0.3 (`gcr.h`, "Weak bits"). Each time a guest
data-register read carries `offset` past the end of the buffer
(`iwm_reg_read`, latch and non-latch), the drive's `revolution` counter
advances and the entry is redrawn at the index (`iwm_track_index`); the
first touch draws at the current count. The pre-rolled revolution of draw
N is draw N − 1, so a read running on across the index sees one unbroken
stream. A draw is a pure function of the capture's fingerprint, the track,
the side and N, and the counter moves only on guest reads, so the same
command sequence gives the same draws in any pacing mode. The model is
revolution-granular: two reads of a weak area within one revolution read
the same draw, which a real drive cannot be asked to do either.

What does not change: the content rule's comparison and the sector status
use the noise-free framing (a draw can never move a track from rule 2 to
rule 3); a spliced or synthesised track is never weak; the sector-level
controllers, the VFS and the `.moof` export (which copies the captured
bits, weak runs included) see no noise; an inspection of an unbuilt track
sees the noise-free framing and draws nothing. The time-based RDDATA sense
bit indexes the current draw but never draws one. A guest write into a weak
entry freezes it — its last draw plus the guest's bytes, which the
write-through and the eject's flush read as they stand. The freeze is the
write itself, not `modified`: the write-through clears `modified` as each
sector reaches the image. A drive's counter carries on across disks;
two drives holding the same capture draw independently.

A sector the capture could not read, laid down from sectors (rules 1 and
4), gets a failing data checksum, as does one the image cannot read; and the
sector-level paths — `floppy_media_read_sector()` (SWIM3, New Age) and the
IIfx's IOP SWIM — fail a read of it (`floppy_media_block_unreadable`) rather
than return the zeros the sector view holds. Writing it makes it readable.

### Track lengths

A track's length is its buffer's (`floppy_track_len`, keyed on the buffer
being there: `size` is checkpointed plain data and is stale without one). A
captured track is as long as its revolution — Applesauce captures run a
little longer than the zone constants — so the data-register read and its
latch-mode search, the write path, and the time-based RDDATA sense bit all
wrap at the track's own length. `iwm_track_data()` brings the drive's
`offset` (modulo the length) and `write_hdr_start` inside the buffer every
time it hands one out: a head position carried over from a track of another
length, or restored from a checkpoint, is never used to index past the end.
A seek resets `offset` to 0, as it always has.

An inspection (`iwm_track_data_peek`, behind the register peeks) caches
nothing: it sees a built buffer as it is, and for a captured disk builds the
track into a scratch buffer of its own (which reads the image, and so counts
toward its activity light); a plain disk's unbuilt track reads as sync.

### Checkpoints

A checkpoint saves each drive's residues — the buffers the guest wrote that
are `modified` — each with its length; clean buffers are rebuilt from the
image. A restore bounds a residue's length, before allocating it, by the
longest track the disk can have: the zone's length, or the capture's longest
track, plus the 2% a splice may add. The image is reopened by name, so a
checkpoint also records each drive's capture fingerprint (the MOOF header's
CRC, or one computed when the file leaves it 0). A quick checkpoint's disk is
its base plus a delta, so a quick restore that reopens a different capture
is refused. A consolidated one carries every block and restores whatever
became of the file, logging the change: with the MOOF gone the recreated
base has no capture, so the disk's tracks are laid down from the restored
sectors (rule 1) and a sector the capture could not read reads as the zeros
the image holds; onto another capture they are that capture's, by the same
rule. Its residues come back as saved — each is a whole buffer, not a
change to the capture, and holds guest writes the image does not have yet
— bounded then by twice the zone's length, since the capture they were
read from is not there to say. No captured bits go into a checkpoint.

A weak entry is not a residue and is not saved. Each drive's `revolution`
and each entry's `weak` flag and `draw` are plain data in the checkpointed
prefix, and the first touch after a restore rebuilds the saved draw — not
the current count's, which reads of another weak track may have moved — so
the guest reads on from the same bytes at the same `offset`.
