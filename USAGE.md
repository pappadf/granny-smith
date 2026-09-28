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

A value may point *into* a zip or Mac archive (`…/roms.zip/iici.rom`), and archive.org URLs work as copied from the site. A bare HFS volume image (the Mini vMac / archive.org shape, with no partition map) boots as a SCSI hard disk.

**Example** (one line):
```
https://pappadf.github.io/gs-pages/staging/?ROM=https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip/368CADFE%20-%20Mac%20IIci.ROM&HD0=https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img
```
Encode `&`, `#`, `+` and `%` inside a value (`&` is `%26`). The full rules — container paths, archive.org routing, errors — are in [`docs/guide/web.md`](docs/guide/web.md#url-parameters).

#### 2. Persistent Filesystem (IDBFS)
If you have previously used the emulator, images may already exist in the persistent storage (`/persist/boot/`).
- If no URL parameters are provided, the emulator will use any images found in `/persist/boot/`.
- If URL parameters are provided, those images will overwrite any existing files with the same name.

#### 3. Drag-and-Drop
If a required image (such as the ROM) is missing, you can drag and drop the file onto the emulator canvas:
- The emulator will recognize the file type (by size, signature, and checksum).
- The file will be saved to `/persist/boot/` (e.g., `/persist/boot/rom`).
- A dialog will prompt you to start the emulator with the new image.

### Boot Process
- The emulator requires a valid ROM image at `/persist/boot/rom` to start.
- If floppy (`fd0`, `fd1`) or hard disk (`hd0`, etc.) images are present, they will be automatically inserted or attached before emulation begins.
    - Floppy: `insert-floppy /persist/boot/fd0 0`
    - Hard disk: `attach-hd /persist/boot/hd0 0`
- If no ROM image is available, emulation cannot start until one is provided (via URL, persistent storage, or drag-and-drop).

### Notes
- Persistent storage is managed via IDBFS and is available under `/persist`.
- Supplying images via URL will overwrite any existing images with the same name in persistent storage.
- The emulator will always use the most recently supplied or available images for each device slot.

For more information on coding style and contributing, see [`docs/STYLE_GUIDE.md`](docs/guide/STYLE_GUIDE.md).
