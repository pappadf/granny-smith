# Usage Guide for Granny Smith Emulator

## Configuring the Emulator

The Granny Smith emulator offers flexible configuration options for specifying ROM, floppy, and SCSI hard disk images. These options allow you to control the system state before starting emulation.

### Image Types
- **ROM image**: The essential firmware required to boot the emulator (referred to as `rom`).
- **Floppy images**: Optional disk images, referenced as `fd0` and `fd1`.
- **SCSI hard disk images**: Optional disk images, referenced as `hd0`, `hd1`, `hd2`, etc.

### Configuration Methods

#### 1. URL Parameters (Recommended for Immediate Setup)
You can specify images directly in the page URL as query arguments (`rom=`, `fd0=`, `hd0=`, `cd=`, `model=`, `speed=`; names are case-insensitive, and `HD=` means `hd0=`). A URL with a ROM boots straight into a running machine, without the configuration dialog. Each image is downloaded, kept in the browser's storage under `/opfs/images/<kind>/`, and attached.

A value may point *into* a zip or Mac archive (`…/roms.zip/iici.rom`). Each URL is fetched exactly as given, so it must name an endpoint the browser may read from another site (for archive.org: `archive.org/cors/<item>/<file>`, or the file server's `view_archive.php` for a file inside a zip). Downloaded images are named by slot and time (`hd0_2026-10-04_17-42-05`). A bare HFS volume image (the archive.org shape, with no partition map), or a partitioned disk image without a driver (the Disk Copy 4.2 shape), boots as a SCSI hard disk.

**Example** (one line):
```
https://pappadf.github.io/gs-pages/staging/?ROM=https%3A%2F%2Fia800908.us.archive.org%2Fview_archive.php%3Farchive%3D%2F12%2Fitems%2Fmac_rom_archive_-_as_of_8-19-2011%2Fmac_rom_archive_-_as_of_8-19-2011.zip%26file%3D368CADFE%2520-%2520Mac%2520IIci.ROM&HD0=https://archive.org/cors/AppleMacintoshSystem753/System7_5_3.img
```
Encode `&`, `#`, `+` and `%` inside a value (`&` is `%26`); a value with a query string of its own is encoded as a whole. The full rules — container paths, linking archive.org, names, errors — are in [`docs/guide/web.md`](../guide/web.md#url-parameters).

**Blank disks.** Instead of a URL, `hdN=` and `fdN=` take `blank:<model or size>`, which creates a new, empty disk in that slot — for example an empty drive to install System on, without hosting an empty image:
```
?rom=…&cd=…/System75.iso&hd0=blank:HD230SC    # a blank Apple HD230SC (Quantum LPS 240S)
?rom=…&fd0=…/Install1.dsk&hd0=blank:80mb      # 80 MB, rounded up to the nearest drive model (HD80SC)
?rom=…&hd0=blank:100m                         # exactly 100 MiB, no particular model
?rom=…&fd1=blank:800k                         # a blank floppy: 800k or 1440k
```
A hard disk takes a drive model (`HD20SC` … `HD1000SC`), a size in MB or GB that is rounded up to the nearest model (`40mb`, `1gb`), or an exact size (`100m`, `512k`) up to 2 GB — the same sizes as the terminal's `files.hd_create` and **Create blank image…**. On a Lisa, `hd0=blank:5mb` or `blank:10mb` (the ProFile and Widget). The disk is unformatted, like a new drive: initialise it from the installer or Drive Setup / HD SC Setup. It is stored in `/opfs/images/hd/` (or `fd/`) under a name made from the slot, the size and the link (`blank_HD230SC_hd0_1a2b3c4d.dmg`), so reloading the same link attaches that disk again instead of making another; `blank!:` replaces it with a fresh one. What the machine writes to a disk is kept with the machine (its checkpoint), not in the stored image, so a reload of the link starts from the blank disk again — use the start screen's resume to continue a machine. A model or size that cannot be created is reported in the progress view, and the machine boots without that disk. (Formatting the disk as HFS ahead of time, `blank:HD80SC,hfs`, is not supported.)

#### 2. Persistent Storage
If you have previously used the emulator, images may already exist in the browser's persistent storage (`/opfs/images/<kind>/` — `rom`, `vrom`, `fd`, `hd`, `cd`).
- If no URL parameters are provided, the New Machine dialog lists every stored ROM (identified by its own checksum), and the media dropdowns offer the stored floppy, hard-disk and CD images.
- If URL parameters are provided, the downloaded images land in the same folders.

#### Compact storage of hard-disk and CD images
A browser charges a stored file's full size against the site's storage allowance, even the parts that are empty. So hard-disk and CD images larger than 16 MB — loaded, dropped, or given in the URL — are stored compressed, as `.dmg` (UDIF) files: empty space costs nothing and the rest is compressed, so a 2 GB disk with a small system on it takes tens of MB. They are converted while they download or load (a zip or gzip is unpacked on the way), so the full-size disk never has to fit in the browser's storage, even for a moment. A `System 7.5.3.img` you add is stored as `System_7.5.3.dmg`. A blank hard disk made with **Create blank image…** is a `.dmg` too, a couple of KB whatever its size. Saving one to your computer from the Filesystem tab gives you that `.dmg`, which macOS (`hdiutil`), 7-Zip and `dmg2img` open; `files.convert <dmg> <img> format=raw` in the terminal writes a plain image. The progress shows how much was read and how much stored, with a button to cancel.

#### 3. Drag-and-Drop
If a required image (such as the ROM) is missing, you can drag and drop the file onto the emulator screen:
- The emulator will recognize the file type (by size, signature, and checksum).
- The file will be saved to the browser's persistent storage under `/opfs/images/<kind>/` (e.g. `/opfs/images/rom/`).
- A dropped ROM boots straight away when no machine is running: the emulator picks the first model the ROM is compatible with and starts it. Dropped floppy images go into the first empty floppy drive; CD images go into the machine's CD bay.

### Boot Process
- The emulator requires a valid ROM image to start.
- If floppy (`fd0`, `fd1`), hard disk (`hd0`, etc.) or CD images are part of the machine's configuration, they are inserted or attached before emulation begins.
- If no ROM image is available, emulation cannot start until one is provided (via URL, persistent storage, or drag-and-drop).

### Notes
- Persistent storage is the browser's OPFS, mounted at `/opfs` in the emulator, and survives page reloads.
- Supplying images via URL will overwrite any existing images with the same name in persistent storage.
- The emulator will always use the most recently supplied or available images for each device slot.

For more information on coding style and contributing, see [`docs/guide/STYLE_GUIDE.md`](../guide/STYLE_GUIDE.md).

## Printing

Two printers are available to the emulated machine.

**The ImageWriter** (dot matrix, the Mac's and the Lisa's everyday printer)
is part of every machine. Plug it in from the terminal, then print from the
guest as you would to a real one:

```
machine.imagewriter.connection = "serial-b"    # a Mac's printer port
machine.imagewriter.connection = "serial-a"    # the Lisa's Serial A
machine.imagewriter.connection = "localtalk"   # ImageWriter II with the LocalTalk Option card
```

On a Mac choose **ImageWriter** in the Chooser (or **AppleTalk ImageWriter**
for the LocalTalk connection, which lists "Virtual ImageWriter"). Each job
opens as a PDF in the print viewer when it is finished, a few seconds after
the last data arrives; the viewer has a Save to computer… button. Text prints in the
ImageWriter II's own draft and correspondence fonts; colour jobs print in
colour. `machine.imagewriter.model = "imagewriter"` switches to the original
ImageWriter, and `machine.imagewriter.paper = "a4"` changes the paper.

**The LaserWriter** is on the AppleTalk network (`appletalk.printer`); choose
**LaserWriter** in the Chooser. It needs the PostScript interpreter that
ships with the browser build.

From the headless build, documents are written to the directory given with
`--print-dir` (`imagewriter2-00001-Print.pdf`).
