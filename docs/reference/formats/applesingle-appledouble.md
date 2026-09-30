# AppleSingle and AppleDouble File Formats

AppleSingle and AppleDouble are Apple's two standard encodings for storing a
file from a "home" file system (typically the Macintosh's two-fork HFS family)
on a "foreign" file system that can only hold a flat, contiguous byte stream —
while preserving everything else about the file: the resource fork, the Finder
metadata, the file dates, and per-platform attribute words. **AppleSingle** packs
all of that into one self-contained foreign file; **AppleDouble** splits it into
two foreign files: a *data file* holding the raw data fork and a *header file*
holding everything else. Both share one container grammar — a fixed 26-byte
header, a table of entry descriptors, and an arbitrary list of typed entries
located by absolute offset.

The formats are versioned: the original 1988/1989 version 1 (documented in the
Apple II File Type Notes and the A/UX 1.x documentation set) was superseded in
1990 by the version 2 developer's note, which is the specification still
implemented by every Apple-descended tool chain today [1] Appendix, p. 17.
Applications must understand version 1 but should create only version 2 [1]
Append, p. 17.

This page describes version 2 as its primary subject (with version 1 called
out wherever it differs), and is built from the version 2 developer's note, the
two Apple II File Type Notes that specify version 1, the HFS Plus and Finder
metadata context of TN1150 and *Inside Macintosh: Files*, and byte-level
decodes of eight real AppleDouble header files produced by Apple's own
Macintosh-to-foreign-filesystem copy tooling (§4).

**Contents:**

1. [Identification](#1-identification-magic-sizes) — what the two forms are for;
   magic numbers and versions; the identification test; version 1 in the wild;
   sizes and structural limits; where the format is encountered
2. [Layout tables](#2-layout-tables) — the fixed header; entry descriptors; the
   predefined entry IDs; the payload layout of every defined entry; file-layout
   rules; the AppleDouble pair; the version 1 entries
3. [Algorithms](#3-algorithms) — header parse and validation; entry enumeration
   and holes; date conversion; foreign-filesystem filename derivation; locating
   the AppleDouble data file; the version 1 upgrade; why there are no checksums
4. [Worked decode of a real specimen](#4-worked-decode-of-a-real-specimen) —
   eight observed `._` sidecars; byte-by-byte decode of one of them, down to the
   resource fork
5. [Open questions](#5-open-questions)

References

---

## 1. Identification (magic, sizes)

### 1.1 What the two forms are for

A Macintosh file has two forks — the data fork and the resource fork — plus
Finder information (type, creator, flags, icon position) and file dates. No
foreign file system of 1990 understood any of that. The two Apple formats
preserve it:

- **AppleSingle** stores data fork, resource fork, Finder information, comment
  and any other attributes in *one* foreign file [1] Chapter 2, p. 4. It is the
  storage/archival form: nothing can be moved or deleted inconsistently, but
  the file is harder to update in place [1] Chapter 1, p. 2.
- **AppleDouble** stores the data fork as a plain foreign file with no header
  at all (the *data file*), and everything else in a second file (the *header
  file*) whose grammar is identical to AppleSingle except that it carries no
  data-fork entry and uses a different magic number [1] Chapter 3, p. 13. It is
  the working form: foreign users can edit the data file directly [1]
  Chapter 1, p. 2.

Applications may create either form but must understand both [1] Chapter 1,
p. 2. Apple suggests — but does not require — that an AFP file server use one
of the two formats as its external storage format so that other applications
on the same machine can share the files [1] Chapter 1, p. 2.

The formats are deliberately home-system-agnostic: a file is stored as a
heterogeneous collection of entries, interpreted as needed by the reader, so a
ProDOS client can read its entry from a file a Macintosh client wrote [1]
Chapter 1, p. 1; Chapter 2, p. 7. Only the assumption that a file is a
contiguous set of bytes is made of the foreign file system [1] Chapter 1, p. 1.

### 1.2 Magic numbers and versions

Byte ordering throughout the header is MC68000 convention — most significant
byte first [1] Chapter 2, p. 5; the version 1 File Type Notes mark every
multi-byte header field "Reverse" for the same reason [2] [3].

| Field | AppleSingle | AppleDouble |
|---|---|---|
| Magic number (4 bytes, big-endian) | `$00051600` | `$00051607` |
| Version 1 version number | `$00010000` [2] [3] | `$00010000` [2] [3] |
| Version 2 version number | `$00020000` [1] Chapter 2, p. 5 | `$00020000` [1] Chapter 2, p. 5 |

The magic number is "modeled after the UNIX magic number feature" [1] Chapter 2,
p. 5; the values are fixed constants, not endian-fragile data. Both versions of
a form use the *same* magic number — the magic distinguishes AppleSingle from
AppleDouble, and the **version field** distinguishes version 1 from version 2
[1] Appendix, p. 17; [2] "About AppleSingle 2.0". A reader that keys only on
the magic will mis-parse nothing in the header (the layouts of the first 8
bytes are identical in both versions), but must check the version before
interpreting the entries.

The purpose of the distinct AppleDouble magic is explicit: an application that
finds `$00051607` knows it must look in a *second* file for the data fork [1]
Chapter 3, p. 13.

### 1.3 The identification test

The File Type Notes quantify the content test: a file beginning with the magic
number but not being an AppleSingle file would be a 4,294,967,295-to-1
coincidence; matching *both* magic and version is roughly 1.8 × 10^19 to 1
[2] "Identifying AppleSingle Files"; [3] "Identifying AppleDouble Files". The
practical identification sequence is therefore:

1. Read the first four bytes. `$00051600` → AppleSingle; `$00051607` →
   AppleDouble header file; anything else → not this format.
2. Read the version longword at offset +4 to select the entry interpretation
   (v1: `$00010000`; v2: `$00020000`).
3. Structurally validate the descriptor table (§3.1) — offsets and lengths
   must fit the file, and no entry ID may be zero [1] Chapter 2, p. 6.

On ProDOS, Apple also registered static file types so the format can be
identified without opening the file: type `$E0`, auxiliary type `$0001` for
AppleSingle; `$0002` for an AppleDouble header file and `$0003` for an
AppleDouble data file [2] [3] (both Notes "strongly encourage" these
assignments for new files).

### 1.4 Version 1 files in the wild

Version 1 files are identified by version `$00010000` and by one header
difference: bytes +8..+23, which version 2 defines as a 16-byte all-zero
filler [1] Chapter 2, p. 5, are in version 1 a fixed-length 16-byte ASCII
**home file system** string, padded with blanks [2] [3] (both Notes, "Home
File System"):

| Home system | Bytes (as printed in the File Type Notes) |
|---|---|
| ProDOS | `$50726F444F5320202020202020202020` ("ProDOS" + blanks) |
| Macintosh | `$4D6163696E746F736820202020202020` ("Macintosh" + blanks) |
| MS-DOS | `$4D532D444F5320202020202020202020` ("MS-DOS" + blanks) |
| Unix | `$556E9878202020202020202020202020` — as printed; see §5.6 |
| VAX VMS | `$56415820564D53202020202020202020` ("VAX VMS" + blanks) |

A v1-era header therefore often has visible ASCII in the filler position, while
a v2 header has zeros there. The version 1 File Info entry (ID 7, §2.11) is
per-home-system and has no v2 counterpart — v2 replaced it with the File Dates
Info entry (ID 8) plus one platform File Info entry (ID 10, 11 or 12) [1]
Appendix, p. 17; [2] [3] (both Notes, "About … 2.0").

### 1.5 Sizes and structural limits

| Quantity | Value | Source |
|---|---|---|
| Fixed header | 26 bytes (magic 4 + version 4 + filler 16 + entry count 2) | [1] Chapter 2, p. 5, Table 2-1 |
| Entry descriptor | 12 bytes (ID 4 + offset 4 + length 4) | [1] Chapter 2, p. 6 |
| Minimum file size | 26 bytes (zero entries is a legal count) | [1] Chapter 2, p. 5 |
| Entry count field | unsigned 16-bit; 0 is legal | [1] Chapter 2, p. 5 |
| Entry ID range | 1..$7FFFFFFF reserved by Apple; $80000000..$FFFFFFFF free for applications; ID 0 is invalid | [1] Chapter 2, pp. 6, 7 |
| Entry length | unsigned 32-bit; 0 is legal | [1] Chapter 2, p. 6 |
| AppleDouble data file | no header at all; the raw data fork, byte for byte | [1] Chapter 3, p. 13; [3] "The AppleDouble Data File" |

No maximum entry count, maximum file size, or alignment rule is stated
anywhere in the specification; the count is bounded only by the descriptor
table's own 12 bytes per entry (§5.9).

### 1.6 Where the format is encountered

Three families of tooling produce and consume these formats:

- **Apple file servers.** AFP servers may store foreign-hosted Macintosh files
  in AppleSingle or AppleDouble form; the note explicitly suggests the
  AppleDouble shape for server storage [1] Chapter 1, p. 2. The AFP-specific
  entries (IDs 13–15, §2.8) exist for exactly this case.
- **Foreign-filesystem mounting on Macintosh** — the historically dominant
  use: A/UX and other UNIX systems kept AppleDouble pairs on UFS/NFS volumes
  [1] Chapter 3, pp. 14–16, and later Apple operating systems kept the same
  grammar for their own `._`-prefixed sidecar files when copying forked files
  onto FAT, SMB and ISO-9660 volumes (*observed*; the corpus of §4 is exactly
  such a copy, and its naming convention — `._<name>` rather than the note's
  `%<name>` — is an Apple convention change that post-dates the 1990
  specification, see §5.3).
- **Archive and interchange of single files**, where AppleSingle's
  self-containment wins: a Macintosh file's both forks, Finder info and dates
  survive a trip through any byte-oriented medium. Disk-image formats are the
  neighbouring case — where DiskCopy 4.2 packages a *whole disk* with its own
  checksums ([diskcopy42.md](diskcopy42.md)), AppleSingle packages a *single
  file* — and, symmetrically, formats that keep their indexing metadata in the
  resource fork become undecodable once only a data fork survives a copy; the
  AppleDouble data-file/header split is the standard defence.

The emulator-adjacent engineering context in which these formats are actually
met today — resource forks and Finder info arriving as `._` sidecars beside
data-fork files — is background observed in practice, not part of the
specification.

## 2. Layout tables

### 2.1 The fixed header

Version 2 layout ([1] Chapter 2, p. 5, Table 2-1; byte order MC68000):

| Offset | Length | Field | Contents |
|---|---|---|---|
| +0 | 4 | Magic number | `$00051600` (AppleSingle) or `$00051607` (AppleDouble header) |
| +4 | 4 | Version number | `$00020000` for the format described in the note (§1.2 for v1) |
| +8 | 16 | Filler | all zero in v2 [1] Chapter 2, p. 5; the 16-byte home-file-system string in v1 (§1.4) |
| +24 | 2 | Number of entries | unsigned; if non-zero, that many entry descriptors follow immediately |
| +26 | 12 × N | Entry descriptors | §2.2 |

An AppleDouble header file has exactly this layout; an AppleDouble *data* file
has no header whatsoever — it is the data fork verbatim [1] Chapter 3, p. 13.

### 2.2 Entry descriptors

Each descriptor is three big-endian longwords [1] Chapter 2, p. 6:

| Offset | Length | Field | Meaning |
|---|---|---|---|
| +0 | 4 | Entry ID | unsigned; 1..$7FFFFFFF reserved by Apple, ID 0 invalid; $80000000 up is application-defined |
| +4 | 4 | Offset | unsigned; from the beginning of the file to the beginning of the entry's data |
| +8 | 4 | Length | unsigned; length of the data in bytes; may be zero |

The descriptors appear in the header in any order, and the entries they point
to may appear in the file in any order and need not be adjacent — holes are
legal and intended for growth (§2.9) [1] Chapter 2, p. 10; [2] "The Entries:".

### 2.3 Predefined entry IDs

Version 2 defines fifteen IDs ([1] Chapter 2, p. 6). The "typically created"
column is the note's own statement of which entries a writer emits for which
population of files [1] Chapter 2, p. 7 — the observed corpus of §4 violates
it in an interesting way (§4.5).

| ID | Name | Contents | Typically created |
|---|---|---|---|
| 1 | Data Fork | the file's data fork, verbatim | all files (AppleDouble: lives in the data file, never in the header) |
| 2 | Resource Fork | the file's resource fork, verbatim | Macintosh and ProDOS files |
| 3 | Real Name | the file's name as created on the home file system | all files |
| 4 | Comment | standard Macintosh comment (Get Info comment) | Macintosh files |
| 5 | Icon, B&W | standard Macintosh black-and-white icon | Macintosh files; rare — icons usually live in the app's resource fork bundle |
| 6 | Icon, Color | Macintosh color icon | as above |
| 8 | File Dates Info | creation, modification, backup, access times (§2.5) | all files |
| 9 | Finder Info | 16 bytes `ioFlFndrInfo` + 16 bytes `ioFlXFndrInfo` (§2.6) | Macintosh files |
| 10 | Macintosh File Info | locked/protected attribute bits (§2.7) | Macintosh files |
| 11 | ProDOS File Info | access, file type, auxiliary type (§2.7) | ProDOS files |
| 12 | MS-DOS File Info | MS-DOS attributes (§2.7) | MS-DOS files |
| 13 | Short Name | AFP short name (§2.8) | AFP servers only |
| 14 | AFP File Info | AFP attributes word (§2.8) | AFP servers only |
| 15 | Directory ID | AFP directory ID (§2.8) | AFP servers only |

Readers interpret the entries relevant to their home file system and treat the
rest as opaque: a Macintosh client understands IDs 1–6 and 8–10, a ProDOS
client reads ID 11 instead of IDs 9 and 10 [1] Chapter 2, p. 7. Applications
must ignore unknown entries *and preserve them* when moving or copying files
[1] Chapter 2, p. 12; Chapter 3, p. 16 — round-trip preservation of
application-defined entries is part of the format contract, not a courtesy.

ID 7 is conspicuously absent from this table: it is the version 1 File Info
entry (§2.11), which version 2 retired.

### 2.4 Entry payload layouts: forks, name and comment

**Data Fork (ID 1)** and **Resource Fork (ID 2)** are raw bytes, stored with no
added header, prefix or length marker — the length lives in the descriptor.
The resource fork entry is a complete Macintosh resource fork, byte for byte;
its internal structure is the Resource Manager's own on-disk format, not
AppleSingle's (§4.4 decodes one).

**Real Name (ID 3)** is the file's name in the home file system: "just ASCII
data" — no length byte, no terminator; the length is the descriptor's length
field [2] "The Real Name Entry:". (Contrast the Pascal-string-with-length-byte
convention of the DiskCopy 4.2 header, [diskcopy42.md](diskcopy42.md) — here
the descriptor table plays the length's role.) File *server* applications
typically do not create or read this entry: they use the reverse mapping of
the foreign filename when presenting names to the client [1] Chapter 2, p. 12.

**Comment (ID 4)** is the standard Macintosh comment [1] Chapter 2, p. 6. The
File Type Notes give it a context maximum of 200 characters for growth-planning
purposes — the note's hole example reserves 200 bytes for it [2] "The
Entries:"; [1] Chapter 2, p. 10.

**Icons (IDs 5, 6)** rarely appear, because icons are normally stored as a
bundle in the application file's resource fork [1] Chapter 2, p. 7.

### 2.5 File Dates Info (ID 8)

The entry consists of the file creation, modification, backup and access times
[1] Chapter 2, p. 7. Each time is a signed number of seconds before or after
midnight (00:00) January 1, 2000 GMT — the start of the year 2000 GMT is
date-time 0 — and applications must convert to their native conventions [1]
Chapter 2, p. 7.

| +Offset | Length | Field | Value |
|---|---|---|---|
| +0 | 4 | seconds from 2000-01-01 00:00:00 GMT | creation time |
| +4 | 4 | same | modification time |
| +8 | 4 | same | backup time |
| +12 | 4 | same | access time |

Two caveats attach to this table. First, the note presents the entry through
"Figure 2-1" (p. 7), a drawing whose field positions are not recoverable from
the text; the four-field order above follows the prose's enumeration
("creation, modification, backup and access") and is the order every
Apple-descended implementation uses, but the figure may show additional
reserved slots beyond the four (§5.1). Second, when initially created, a file's
**backup time** and any unknown date entries are set to `$80000000` — "the
earliest reasonable time", i.e. the most negative representable value [1]
Chapter 2, p. 7.

The epoch deliberately differs from the Macintosh file-system epoch: HFS and
HFS Plus store dates as unsigned seconds since midnight January 1, 1904 (GMT in
HFS Plus, local time in HFS) [4] "HFS Plus Dates"; version 1's Macintosh File
Info used that same 1904 epoch [2] "File Info Entry" (Macintosh). The constant
that bridges the two eras is exact arithmetic: 3,029,529,600 seconds
(`$B492F400`) separate 1904-01-01 from 2000-01-01. See §3.3.

### 2.6 Finder Info (ID 9)

The entry is 32 bytes: 16 bytes of Finder Info followed by 16 bytes of extended
Finder Info — the `ioFlFndrInfo` and `ioFlXFndrInfo` fields "as returned by the
Macintosh `PBGetCatInfo` call" [1] Chapter 2, p. 7; [5]. Those are the catalog
record's `FInfo`/`FXInfo` and `DInfo`/`DXInfo` blocks [5]; TN1150 defines the
same structure as `FileInfo` + `ExtendedFileInfo` for files [4] "Finder Info":

| +Offset | Length | Field (files) | Notes |
|---|---|---|---|
| +0 | 4 | `fdType` / `fileType` | file type OSType, e.g. `TEXT` |
| +4 | 4 | `fdCreator` / `fileCreator` | creator OSType, e.g. `ttxt` |
| +8 | 2 | `fdFlags` / `finderFlags` | Finder flags, below |
| +10 | 2 | `fdLocation.v` | icon position in the folder (Point.v) |
| +12 | 2 | `fdLocation.h` | icon position in the folder (Point.h) |
| +14 | 2 | `fdFldr` / `reservedField` | folder (window) ID the file is "in" |
| +16 | 8 | `reserved1[4]` | reserved |
| +24 | 2 | `extendedFinderFlags` (`fdXFlags`) | extended flags: `kExtendedFlagsAreInvalid` ($8000), `kExtendedFlagHasCustomBadge` ($0100), `kExtendedFlagHasRoutingInfo` ($0004) [4] "Finder Info" |
| +26 | 2 | `reserved2` | reserved |
| +28 | 4 | `putAwayFolderID` | Finder put-away state |

Finder flag bits used on this page (full set in [4] "Finder Info"): bit 6
`kIsShared` ($0040), bit 7 `kHasNoINITs` ($0080), bit 8 `kHasBeenInited`
($0100), bit 10 `kHasCustomIcon` ($0400), bit 12 `kHasBundle` ($2000), bit 14
`kIsInvisible` ($4000).

Rules for writers [1] Chapter 2, pp. 7–8: newly created files have zeros in
all Finder Info subfields; a writer may assign zero to any unknown subfield
(most are undefined unless the file resides on a valid HFS volume) but should
set `fdType` and `fdCreator`.

**Directories** carry `DInfo`/`DXInfo` (the window/view block) instead of
`FInfo`/`FXInfo`. One directory subfield has a mandated value: `frView`. The
Finder, on first opening a directory whose "inited" bit is clear, initializes
`frView` (how the window is viewed — by icon, by small icon, …) to a nonzero
value, and zero is not a legal value; if the directory is not writable, the
Finder displays things strangely. Writers should set it themselves, e.g.
`closedView` = 256 for the traditional view-by-icon [1] Chapter 2, pp. 7–8.

### 2.7 Platform File Info entries (IDs 10–12)

**Macintosh File Info (ID 10)** is 32 bits storing the locked and protected
bits [1] Chapter 2, p. 8. Macintosh file *times* live in entry ID 8, not here
[1] Chapter 2, p. 8. The bit assignment is not given in recoverable text by
the v2 note (its Figure 2-2 is a drawing); it descends directly from version
1, where the Macintosh File Info's Attributes field is a 32-bit flag word whose
bit zero is the locked bit and bit one is the protected bit [2] "File Info
Entry" (Macintosh):

| +Offset | Length | Field |
|---|---|---|
| +0 | 4 | attribute word — bit 0: locked; bit 1: protected; rest reserved (*inferred — unverified* beyond bit 1, §5.2) |

**ProDOS File Info (ID 11)** consists of the file access, file type and file
auxiliary type [1] Chapter 2, p. 8. The Access word may be used directly in
ProDOS 16 or GS/OS calls (only its low byte is significant to ProDOS 8); the
File Type word is the original file's type (low byte significant to ProDOS 8);
the Auxiliary Type long word is the original auxiliary type (low word
significant to ProDOS 8) [1] Chapter 2, p. 9. ProDOS file times live in entry
ID 8 [1] Chapter 2, p. 8:

| +Offset | Length | Field | ProDOS 8 significance |
|---|---|---|---|
| +0 | 2 | Access word | low byte only |
| +2 | 2 | File Type word | low byte only |
| +4 | 4 | Auxiliary Type long word | low word only |

UNIX files carry no dedicated platform entry — UNIX file times are stored in
entry ID 8 [1] Chapter 2, p. 9.

**MS-DOS File Info (ID 12)** is 16 bits storing the MS-DOS attributes [1]
Chapter 2, p. 9; MS-DOS file times live in entry ID 8. The attribute bit
positions are drawn in the note's Figure 2-4 and are not text-recoverable;
they are the standard MS-DOS attribute word (§5.2).

### 2.8 AFP server entries (IDs 13–15)

These three exist so an AFP server can keep its bookkeeping inside the same
container [1] Chapter 2, pp. 9–10.

**Short Name (ID 13)** holds the AFP short name; AFP servers must keep a
short-name mapping for all foreign files. If the entry does not exist, the
server derives a short name (which must be unique within the directory) and
creates the entry. To keep derived names from colliding with real ones, derived
short names all start with the character `!` (`$21`), and AFP clients are not
allowed to access foreign files whose names begin with that character. Beyond
this, "the short name algorithm remains flexible" [1] Chapter 2, p. 9.

**AFP File Info (ID 14)** is the AFP attributes word, drawn in the note's
Figure 2-5 (bit positions not text-recoverable, §5.2). Its one specified
behaviour: AFP servers should set the **BackupNeeded** bit whenever a file is
modified, or when the foreign file's modification time is later than the
modification time recorded in entry ID 8 [1] Chapter 2, p. 9.

**Directory ID (ID 15)** holds the AFP directory ID — a 4-byte value assigned
when a directory is created, or when a directory AppleDouble header file
without an ID entry is found [1] Chapter 2, p. 10. The next unused ID for a
volume is kept in a special AppleDouble header file named **`%RootInfo`** in
the volume root directory, carrying entries 3, 4, 8, 9 and 15 (Real Name,
Comment, Volume Dates, Finder Info, and Next File ID — note that in this one
file, entry 15 means *next file ID* rather than *directory ID*); it must be
locked while it is updated [1] Chapter 2, p. 10.

### 2.9 File layout rules: contiguity, holes, ordering

The entry data follows the last descriptor. Rules and conventions:

- **Contiguity.** Each entry's data must occupy a single contiguous block [1]
  Chapter 2, p. 10. A reader must locate every entry through its descriptor's
  offset, never by assuming it begins where the previous entry ended [2] "The
  Entries:".
- **Holes.** Unused space between entries is legal and intended: e.g. a
  10-byte comment can be given a 200-byte slot so it can grow without
  reorganizing the file [1] Chapter 2, p. 10. Version 1 even specifies how to
  *find* the holes: sort the descriptors by increasing offset; a hole exists
  wherever an entry's offset is greater than the previous entry's offset plus
  its length [2] "The Entries:" (§3.2).
- **Ordering conventions** (efficiency advice, not validity rules) [1]
  Chapter 2, p. 10: put the data fork last (it is the entry most commonly
  extended); put the small, frequently-read entries — Finder Info, File Dates
  Info, Macintosh File Info — nearest the header, so one or two block reads
  retrieve them; allocate the resource fork in 4 KB blocks to minimize
  reorganization when it is updated. For AppleDouble, where the data fork lives
  in its own file, the corresponding advice is to put the **resource fork last**
  in the header file, since it is then the most-extended entry [1] Chapter 3,
  p. 13.
- **Locking.** AppleSingle files, and AppleDouble header files, must be locked
  during access to ensure data integrity [1] Chapter 2, p. 12; Chapter 3,
  p. 16.

### 2.10 The AppleDouble pair

| File | Contents |
|---|---|
| AppleDouble **data file** | the data fork, byte for byte, with no header at all; it is an ordinary foreign file that foreign applications can read and write directly [1] Chapter 3, p. 13 |
| AppleDouble **header file** | exactly the AppleSingle grammar (§2.1–§2.9) minus the data fork entry; magic `$00051607` [1] Chapter 3, p. 13 |

The note permits an application-defined entry in the header file to point at
the data file — for example holding its name — and observes that some foreign
file systems could provide a more permanent pointer that survives the data
file being renamed [1] Chapter 3, p. 13. (Version 1 formalized exactly such an
entry — ID 100, Data Pathname, §2.11 — and the v1 File Type Note gives a
three-step lookup procedure for finding the data file, §3.5.)

There is **no guaranteed way** to keep the pair consistent: a user can always
rename, move or delete one half without the other; the filename conventions of
§3.4 are the mitigation, since knowledgeable users can preserve the connection
[1] Chapter 3, p. 14. Directories are stored as AppleDouble files if they are
created by the application, and applications should not create header files
for pre-existing foreign directories or files unless necessary to store
AppleDouble entries [1] Chapter 3, p. 16.

### 2.11 Version 1 entries (IDs 7 and 100)

**File Info (ID 7, v1 only)** is per-home-system [2] "The File Info Entry:":

| Home system | Layout (big-endian) | Length |
|---|---|---|
| ProDOS | create date (2, ProDOS 8 packed), create time (2), modification date (2), modification time (2), Access (2), File Type (2), Auxiliary Type (4) — dates in 2-byte ProDOS 8 form, not 8-byte GS/OS form | 16 |
| Macintosh | create date (4, unsigned seconds since 1904-01-01), modification date (4), last backup date (4), Attributes (4: bit 0 locked, bit 1 protected) | 16 |
| MS-DOS | modification date (4, MS-DOS packed), attributes (2) | 6 |
| Unix | create date/time (4), last-use date/time (4), last-modification date/time (4) | 12 |

**Data Pathname (ID 100, v1 AppleDouble)** is defined in the AppleDouble File
Type Note only: a "class one GS/OS input string" — a big-endian word giving
the path length, followed by the ASCII pathname of the AppleDouble data file
as originally created [3] "The Data Pathname Entry:". It does not appear in the
version 2 note's predefined table (§5.8).

## 3. Algorithms

The formats define **no checksums and no compression** (§3.7); the algorithms
below are the complete set a conforming reader/writer needs.

### 3.1 Header parse and structural validation

```
function parse_apple_file(buf, n):
    # --- identification (1.2, 1.3) ---
    if n < 26:                        fail "shorter than the fixed header"
    magic   = u32be(buf, 0)
    version = u32be(buf, 4)
    if magic not in {0x00051600, 0x00051607}: fail "bad magic"
    if version not in {0x00010000, 0x00020000}: fail "unknown version"

    # --- header geometry (2.1) ---
    count = u16be(buf, 24)
    table_end = 26 + 12 * count
    if count == 0:
        return empty file            # legal [1] Chapter 2, p. 5
    if table_end > n:                fail "descriptor table overruns file"

    # --- descriptor walk (2.2) ---
    entries = []
    for i in 0 .. count-1:
        d = 26 + 12*i
        id  = u32be(buf, d)
        off = u32be(buf, d+4)
        len = u32be(buf, d+8)
        if id == 0:                  fail "entry ID 0 is invalid"   [1] p. 6
        if off + len > n:            fail "entry outside file"
        entries.append(id, off, len)

    # --- form-specific rule (2.10) ---
    if magic == 0x00051607 and any(e.id == 1 for e in entries):
        note "data fork entry in an AppleDouble header"   # see 5.11

    return {magic, version, entries}
```

Two checks above are the specification's own ("entry ID 0 is invalid" [1]
Chapter 2, p. 6) or direct consequences of "the offset … shows the offset from
the beginning of the file" [1] Chapter 2, p. 6. Whether real-world writers
ever produce zero-length descriptor tables, and whether Apple's readers
tolerate them, is unrecorded (§5.9).

### 3.2 Enumerating entries and finding holes

Entries may be in any physical order, so enumeration is always
descriptor-driven. The version 1 note's hole-detection procedure [2] "The
Entries:" doubles as a complete layout walk:

```
function find_holes(entries):
    s = sort entries by offset ascending
    holes = []
    for i in 1 .. len(s)-1:
        prev_end = s[i-1].offset + s[i-1].length
        if s[i].offset > prev_end:
            holes.append((prev_end, s[i].offset - prev_end))   # unused bytes
    return holes
```

### 3.3 Date conversion

Entry 8's times are signed seconds relative to 2000-01-01 00:00:00 GMT [1]
Chapter 2, p. 7. Three conversions cover all interoperability cases:

```
# Exact epoch bridge, verified arithmetic:
#   1904-01-01 .. 2000-01-01 = 35,064 days = 3,029,529,600 s = 0xB492F400
EPOCH_1904_TO_2000 = 3031569600
UNKNOWN_TIME       = 0x80000000          # -2147483648 s = "earliest
                                         #   reasonable time" [1] p. 7

function to_mac_1904(t):                 # -> HFS/HFS+ catalog date
    if t == UNKNOWN_TIME: return t       # keep the sentinel on round-trip
    return (t + EPOCH_1904_TO_2000) mod 2^32

function from_mac_1904(d):               # HFS+ catalog date -> entry 8
    t = (d - EPOCH_1904_TO_2000) mod 2^32  # wraps to signed below
    if t >= 2^31: t = t - 2^32             # interpret as signed
    return t

function to_unix(t):                     # -> seconds since 1970-01-01 UTC
    UNIX_2000_01_01 = 946684800
    return t + UNIX_2000_01_01
```

Two cautions. First, the *sign* convention: v1's Macintosh dates are unsigned
1904-epoch seconds [2]; v2's are signed 2000-epoch seconds — a v1→v2 migration
must subtract the bridge, not copy the word (§3.6). Second, the HFS family's
own GMT/local-time wrinkle is out of scope of *this* format but adjacent:
HFS Plus dates are GMT, the HFS Plus *volume header's* creation date is local
time by later policy [4] "HFS Plus Dates", and HFS dates are local time —
the destination a converted date is headed for determines whether a further
timezone adjustment is correct. The PRAM clock's alarm default is another
consumer of the 1904 epoch ([mac-pram.md](mac-pram.md) §4.1).

### 3.4 Foreign-filesystem filename derivation

The foreign filename is derived from the Real Name by per-system rules; the
rule set must be constant across a single foreign volume [1] Chapter 2, p. 11.
For UNIX and NFS there are three conventions, chosen as the most complete
subset the foreign system supports [1] Chapter 2, pp. 11–12; Chapter 3,
pp. 14–16:

- **8-bit**: any 8-bit character is legal except slash `$2F`, null `$00` and
  percent `$25`.
- **7-bit ASCII**: any 7-bit ASCII character is legal, same three exceptions.
- **7-bit alphanumeric**: only alphanumerics, plus underscore `$5F` and the
  *last* period `$2E` of the name, are left unescaped.

In all three, the escape is a percent sign followed by the two-character
hexadecimal code of the escaped character; the three reserved characters are
always escaped [1] Chapter 2, pp. 11–12. The note's own worked example, home
name `Cañada return - 20%` (ñ is `$96` in Macintosh Roman):

| Convention | Resulting foreign name |
|---|---|
| 8-bit | `Cañada return - 20%25` (only `%` escaped) |
| 7-bit ASCII | `Ca%96ada return - 20%25` (`$80..$FF` also escaped) |
| 7-bit alphanumeric | `Ca%96ada%20return%20%2d%2020%25` |

Runnable derivation:

```
function derive_unix_name(name, convention):     # [1] Chapter 2, pp. 11-12
    out = ""
    for i, ch in enumerate(name):
        if ch in {'/', chr(0), '%'}:
            out += '%' + hex2(ch)                    # always escaped
        elif convention == "7bit-ascii" and ord(ch) >= 0x80:
            out += '%' + hex2(ch)
        elif convention == "7bit-alnum" and not is_alnum(ch)
             and ch != '_' and ch != last_period_of(name):
            out += '%' + hex2(ch)
        else:
            out += ch
    return out

# Header-file prefix, per foreign system:
#   UNIX/NFS:  prefix "%" to the data-file name [1] Chapter 3, p. 16
#   A/UX:      "%" prefix; other UNIX systems may instead gather headers
#              in one subdirectory such as ".AppleDouble/" [3]
#   ProDOS:    prefix "R." to the data-file name [1] Chapter 3, p. 14
#   MS-DOS:    data file = 8-char name + apt extension (e.g. ".TXT");
#              header file = same 8-char name + ".ADF" [1] Chapter 3, p. 14
```

For AppleSingle, the *single* file's name is derived with the same character
rules, with truncation to the foreign system's limits: 15 characters on
ProDOS, 8 plus extension on MS-DOS [1] Chapter 2, p. 11. For AppleDouble, the
data-file name is truncated to 13 characters on ProDOS (two under the 15-char
maximum, to leave room for the `R.` prefix) [1] Chapter 3, p. 14. On UNIX the
data-file name is not explicitly truncated — the system's own `create()`/`
open()` limits apply [3] "Filename Conventions:". Because percent-escaping can
lengthen a 31-character Macintosh name past what some foreign systems accept,
"behavior under these conditions is not defined" [1] Chapter 2, p. 12 — a
specification gap (§5.9).

### 3.5 Locating the AppleDouble data file

The header file alone does not name its data file; the version 1 File Type
Note fixes a deterministic search order [3] "Finding the AppleDouble Data
File", which the v2 note's application-defined pointer entry (§2.10)
generalizes:

```
function find_data_file(header_file, directory):
    # 1. explicit pointer, if present
    if entry 100 (or an application-defined pathname entry) exists:
        if exists(pathname):            return open(pathname)
        tail = basename(pathname)
        if exists(directory/tail):      return open(directory/tail)
    # 2. reverse the naming convention (3.4) for this foreign file system
    for candidate in convention_names(header_file):
        if exists(candidate):           return open(candidate)
    # 3. give up deterministically
    ask the user where the AppleDouble data file is located
```

### 3.6 Upgrading a version 1 file to version 2

The note's complete migration procedure [1] Appendix, p. 17:

1. Overwrite the **version number** and **filler** fields in the header — the
   v1 home-file-system string in bytes +8..+23 becomes the v2 all-zero filler,
   and the version longword becomes `$00020000`.
2. Replace the v1 **File Info entry (ID 7)** with the v2 **File Dates Info
   entry (ID 8)** *and one of* Macintosh File Info (ID 10), ProDOS File Info
   (ID 11) or MS-DOS File Info (ID 12) — splitting the v1 entry's dates from
   its platform attributes, and re-basing the dates from the v1 conventions
   (per-home-system epochs and packed forms, §2.11) to the signed
   seconds-since-2000 form of §2.5.

Applications should understand v1 but create only v2 [1] Appendix, p. 17.

### 3.7 Integrity: why there are no checksums

The specification defines no checksum, CRC, magic-anchored trailer, or
compression anywhere — integrity is delegated entirely to the foreign file
system and to the locking rule of §2.9. This is a deliberate contrast with
Apple's disk-image formats of the same era, which checksum every region
([diskcopy42.md](diskcopy42.md)); a file-level wrapper has no redundant copy
to verify against, so the only self-checks available are structural: magic,
version, descriptor table fit, entry fit, ID 0 rejection (§3.1), and — for
AppleDouble — the pair-consistency heuristics of §3.5. A corrupt AppleSingle
file that passes those checks will decode silently wrong data; there is
nothing in the format to catch it.

## 4. Worked decode of a real specimen

### 4.1 The specimen corpus

The corpus is eight AppleDouble header files observed in a preservation copy
of a 3dfx Voodoo2 driver disk for Macintosh — a Macintosh volume copied onto a
fork-less filesystem, exactly the situation the format was designed for. All
values below are *observed* by direct byte decode; the producing Apple
tooling's version is not identified by the files themselves (§5.3).

Every one of the eight is a `._<name>` sidecar beside its data file — the
naming convention of the copying system, not the 1990 note's `%<name>` — and
every one carries the identical shape:

| Property | Value (all 8 files) |
|---|---|
| Magic | `$00051607` (AppleDouble header) |
| Version | `$00020000` (v2) |
| Filler | 16 zero bytes |
| Entry count | 2 |
| Descriptors | ID 9 (Finder Info), then ID 2 (Resource Fork) |
| Finder Info | offset 50, length 32 — immediately after the descriptor table, no holes |
| Resource fork | offset 82, length = fork size — last, as recommended (§2.9) |
| File size | exactly 82 + resource fork length |
| Entries 1, 3, 8, 10 | absent — no data fork (per the form), no Real Name, no dates, no attribute word |

The pairing includes the Finder's invisible per-folder custom-icon file: the
zero-byte data file `Icon<CR>` is accompanied by a 568-byte sidecar whose
Finder Info reads type `icon`, creator `MACS`, flags `$4000`
(`kIsInvisible`, §2.6) — the Finder's own machinery faithfully carried across
in the container (§5.12).

The remaining tables walk the largest specimen, `._Voodoo2 Read Me`
(994 bytes), whose data file is a 2461-byte SimpleText read-me.

### 4.2 Fixed-header decode

First 50 bytes of the file:

```
0000  00 05 16 07  00 02 00 00  00 00 00 00  00 00 00 00
0010  00 00 00 00  00 00 00 00  00 02 00 00  00 09 00 00
0020  00 32 00 00  00 20 00 00  00 02 00 00  00 52 00 00
0030  03 90  ...
```

| Bytes | Field | Decoded |
|---|---|---|
| `00 05 16 07` | magic | `$00051607` — AppleDouble header file (§1.2) |
| `00 02 00 00` | version | `$00020000` — version 2 |
| 16 × `00` | filler | zeros, as v2 requires (§2.1) |
| `00 02` | number of entries | 2 |
| `00 00 00 09` / `00 00 00 32` / `00 00 00 20` | descriptor 1 | entry ID 9 (Finder Info), offset `$32` = 50, length `$20` = 32 |
| `00 00 00 02` / `00 00 00 52` / `00 00 03 90` | descriptor 2 | entry ID 2 (Resource Fork), offset `$52` = 82, length `$390` = 912 |

Structural checks (§3.1) pass: the descriptor table ends at 26 + 2×12 = 50,
which is exactly entry 9's offset — no holes before the first entry; entry 9
ends at 50 + 32 = 82, exactly entry 2's offset; entry 2 ends at 82 + 912 = 994,
exactly the file size. The two descriptors also obey the note's ordering
advice: the most-extended entry (resource fork) last (§2.9).

### 4.3 Finder Info decode

Entry 9, the 32 bytes at offset 50:

```
0032  54 45 58 54  74 74 78 74  01 00 00 00  00 00 00 00
0042  00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
```

| +Offset | Bytes | Field | Decoded |
|---|---|---|---|
| +0 | `54 45 58 54` | `fdType` | `TEXT` — SimpleText document |
| +4 | `74 74 78 74` | `fdCreator` | `ttxt` — SimpleText |
| +8 | `01 00` | `fdFlags` | `$0100` = `kHasBeenInited` (§2.6): the Finder has already processed this file |
| +10..+13 | zeros | `fdLocation` | icon at (0,0) — never positioned |
| +14 | `00 00` | `fdFldr` | 0 — not in a window |
| +16..+31 | zeros | extended Finder Info | all zero, as the note's rule for unknown subfields permits (§2.6) |

The other read-me files in the corpus differ only in type (`ttro` —
read-only SimpleText — for the license agreement and OpenGL notes) and the
driver libraries read `shlb`/`3Dfx`, `shlb`/`3DFx`, `shlb`/`gld1`,
`shlb`/`tnsl` with flag words composed of `kIsShared`/`kHasNoINITs`/
`kHasBeenInited`/`kHasCustomIcon`/`kHasBundle` (§2.6) — all ordinary Finder
Info traffic, nothing format-specific.

### 4.4 Resource fork decode

Entry 2 is the 912-byte resource fork at offset 82. Its first 16 bytes are the
Resource Manager's on-disk fork header, not an AppleDouble structure — but
they provide a strong independent consistency check on the container:

```
0052  00 00 01 00  00 00 03 5e  00 00 02 5e  00 00 00 32
```

| +Offset (within fork) | Bytes | Decoded |
|---|---|---|
| +0 | `00 00 01 00` | data offset = 256 |
| +4 | `00 00 03 5e` | map offset = 862 |
| +8 | `00 00 02 5e` | data length = 606 |
| +12 | `00 00 00 32` | map length = 50 |

The internal arithmetic closes: data offset + data length = 256 + 606 = 862 =
map offset, and map offset + map length = 862 + 50 = 912 = the entry length
from the descriptor — the container's length field and the fork's own header
agree. The resource map's type list names a single type, `styl` (SimpleText
style-run data), with a single resource of ID `$0080` (128) — the resource
that gives a SimpleText read-me its styled text, and the reason this
read-me needs a resource fork at all. (The map's remaining internals belong to
the Resource Manager's format, out of this page's scope.)

This specimen also demonstrates the practical stakes of §1.6: the data file
alone — 2461 bytes of text — loses the `styl` resource silently; only the
AppleDouble pair preserves the file as the Finder created it.

### 4.5 What the corpus does not contain

The observed files carry **only** entries 9 and 2. The v2 note states that
entries 1, 3 and 8 are "typically created for all files" and entry 10 for
Macintosh files (§2.3) — yet this corpus, produced by Apple's own tooling,
contains no Real Name, no File Dates Info and no Macintosh File Info entry in
any file. The finding is *observed*; the explanation is not in evidence. The
most economical reading — that a copy-out tool writing beside a file whose
name and dates are already kept by the foreign system omits every entry it
deems redundant — is *inferred — unverified* (§5.4). A reader must therefore
treat all of entries 3, 8 and 10 as optional in practice, whatever "typically"
says; a writer targeting maximal fidelity should still emit them, since the
foreign system's own name and dates are not the home system's.

## 5. Open questions

1. **The exact layout and total length of File Dates Info (entry 8).** The
   note defines the entry through Figure 2-1 (p. 7), a drawing whose field
   boxes are not recoverable from the text layer. The four named times
   (creation, modification, backup, access) fix at least 16 bytes; whether the
   figure shows additional reserved slots (and thus a larger canonical entry)
   is unverified. No observed specimen in the §4 corpus carries entry 8 at
   all, so practice cannot arbitrate.
2. **Bit positions inside entries 10, 12 and 14.** Figures 2-2, 2-4 and 2-5
   are drawings. The locked (bit 0) / protected (bit 1) assignment for entry
   10 is re-established from the version 1 note's Attributes field [2] and the
   v2 upgrade rule (§3.6), and the MS-DOS attributes of entry 12 are the
   standard MS-DOS word by construction — but the exact drawn positions in the
   v2 figures, and the position of the BackupNeeded bit in entry 14's AFP
   attributes word, are not text-recoverable. The corpus's AFP document is the
   AFP 2.1/2.2 addenda, which presumes the base specification rather than
   restating it.
3. **The `._` naming convention's provenance.** The 1990 note specifies `%`
   for UNIX/NFS (§3.4); every observed specimen uses `._`. Which Apple
   system version introduced `._`, and in which document it is specified, is
   not established by this evidence set — the specimens date themselves only
   by their content (a 1998-era driver disk).
4. **Why the observed corpus omits entries 3, 8 and 10.** The note says these
   are "typically created" (§2.3); the corpus contradicts "typically" (§4.5).
   Whether the omission is a deliberate copy-out policy, a limitation of the
   producing tool, or an artifact of the destination filesystem is unknown.
5. **The AFP short-name derivation algorithm.** Beyond the `!` prefix and
   uniqueness requirement (§2.8), the note leaves the algorithm "flexible";
   no canonical derivation is specified anywhere in the evidence set.
6. **The v1 "Unix" home-file-system constant.** The File Type Notes print
   `$556E9878`, whose third byte pair is not the ASCII string "Unix"
   (`$556E6978`) the same table names. Whether the printed Notes carry a
   typo or the online transcription is corrupted is unverified against a
   printed original.
7. **The appendix's v1 version constant.** The v2 note's appendix renders the
   version 1 constant as "0x0010000" — seven hex digits [1] Appendix, p. 17.
   The File Type Notes' `$00010000` (§1.2) is authoritative, but whether the
   developer's note itself mis-prints the constant or the OCR layer dropped a
   digit is unverified.
8. **The status of entry ID 100 (Data Pathname).** Defined in the v1
   AppleDouble File Type Note [3], absent from the v2 predefined table, which
   instead only alludes to application-defined pointer entries (§2.10).
   Whether ID 100 remained a de-facto reserved value in v2-era Apple
   implementations, or was abandoned, is unknown.
9. **Unbounded quantities.** No maximum entry count, file size, alignment
   rule, or defined behavior for over-long escaped names is stated ("behavior
   under these conditions is not defined", §3.4); whether real writers emit
   zero-entry headers, and whether Apple's readers accept them, is unrecorded.
10. **The MIME encapsulation.** An IETF standards-track document encapsulating
    these formats (RFC 1740) exists, but is outside this evidence corpus; its
    mapping of AppleSingle/AppleDouble to MIME media types, and any
    divergences from the Apple note, are deliberately not asserted here.
11. **Strictness about entry 1 in AppleDouble headers.** The v1 note says the
    header has "exactly the same format as an AppleSingle file, except it has
    no data fork entry" [3]; whether any Apple reader *rejects* a
    `$00051607` file that still carries entry 1, or silently ignores it, is
    unverified — no specimen or documented behaviour answers it.
12. **The `Icon<CR>` sidecar's generality.** The corpus shows the Finder's
    invisible per-folder icon file travelling as an ordinary AppleDouble pair
    (§4.1). Whether every producing tool replicates it, or only Finder-driven
    copies do, is unknown; it matters to anyone reconstructing a folder view
    from a sidecar corpus.
13. **Entry 8's date semantics against real specimens.** The 2000-01-01 GMT
    epoch, the signed representation and the `$80000000` unknown sentinel are
    established from the specification text alone (§2.5); no specimen in the
    evidence set carries a dates entry, so round-trip behaviour against
    Apple-produced bytes is unverified.

## References

1. Apple Computer, Inc., *AppleSingle/AppleDouble Formats for Foreign Files:
   Developer's Note* (version 2), 1990 — Chapter 1 "About AppleSingle/AppleDouble
   Formats" pp. 1–3; Chapter 2 "The AppleSingle Format" pp. 4–12 (Table 2-1
   "AppleSingle file header" p. 5; magic, version, filler, entry-count fields
   p. 5; entry descriptors and predefined entry IDs pp. 6–7; entry payload
   descriptions and Figures 2-1…2-5 pp. 7–9; file layout p. 10; AppleSingle
   filename conventions pp. 11–12; usage p. 12); Chapter 3 "The AppleDouble
   Format" pp. 13–16; Appendix "Updating Version 1 AppleSingle/AppleDouble
   Files" p. 17.
2. Apple II File Type Note $E0/$0001, "AppleSingle File", Matt Deatherage,
   Apple Developer Technical Support, written March 1989, revised January
   1991.
3. Apple II File Type Note $E0/$0002 and $E0/$0003, "AppleDouble File",
   Matt Deatherage, Apple Developer Technical Support, written March 1989,
   revised November 1990.
4. Apple Computer, Inc., Technical Note TN1150, "HFS Plus Volume Format" —
   "HFS Plus Dates" (GMT seconds since 1904; the volume-header creation-date
   local-time exception) and "Finder Info" (Point/Rect, Finder flag bit
   definitions, FileInfo and ExtendedFileInfo structures) sections.
5. Apple Computer, Inc., *Inside Macintosh: Files*, Addison-Wesley, 1992 —
   File Manager reference: the `PBGetCatInfo` parameter block (`ioFlFndrInfo`,
   `ioFlXFndrInfo`) and the catalog record's Finder information
   (`FInfo`/`FXInfo`, `DInfo`/`DXInfo`).
6. Observed AppleDouble specimen corpus — eight `._<name>` AppleDouble
   version 2 header files written by Apple's Macintosh-to-foreign-filesystem
   copy tooling during a preservation copy of a 3dfx Voodoo2 Macintosh driver
   disk onto a fork-less filesystem; all byte values in §4 are direct decodes
   of these files (`._Voodoo2 Read Me`, 994 bytes, decoded in full;
   `._Icon<CR>`, 568 bytes, cited for Finder Info contrast).
