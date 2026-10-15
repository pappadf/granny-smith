# Sharing files with the Mac

The easiest way to move files between your computer and the emulated Mac
is the **shared folder**: a folder in the browser's storage that the Mac
sees as an AppleShare file server on its network. Disk images are the
other way.

## 1. The shared folder

Granny Smith runs a small AppleShare server on the Mac's simulated
AppleTalk network. Its name is **Shared Folders**, and it offers one
volume, **Shared**, which is the folder `/opfs/shared` in the
**Filesystem** panel.

It is available on every Macintosh model when **AppleTalk** is **Active**
in the machine's configuration (the default). The Mac needs AppleShare
client software, which System 7 and later include.

### 1.1 Connecting from the Mac

1. On the Mac, choose **Chooser** from the Apple menu.
2. Click **AppleShare**. If the Mac asks to turn AppleTalk on, click **OK**.
3. Select **Shared Folders** in the list of file servers and click **OK**.
4. Choose **Guest** and click **OK** (no password is needed).
5. Select the **Shared** volume and click **OK**, then close the Chooser.
6. The **Shared** volume appears on the Mac's desktop.

### 1.2 Copying files into the Mac

1. Open the **Filesystem** panel and find `/opfs/shared`.
2. Drag files from your computer onto that folder.
3. On the Mac, open the **Shared** volume and drag the files to the Mac's
   own disk.

A StuffIt archive or a BinHex file keeps its Macintosh resource fork
intact on the way; expand it on the Mac with StuffIt Expander.

### 1.3 Copying files out of the Mac

1. On the Mac, drag the files onto the **Shared** volume.
2. In the **Filesystem** panel, find them in `/opfs/shared`, right-click,
   and choose **Save to computer…**.

Mac files keep their resource forks and Finder information (type,
creator, icon position) in hidden companion files named `._<name>` next to
each file. File names longer than 31 characters — longer than a Mac allows
— are shown to the Mac in a shortened form.

## 2. Disk images

- **Into the Mac:** put files on a floppy or hard-disk image on your
  computer (or download one), then load it into the machine (drop it on
  the display, or **Insert** / **Mount** in the Images panel). Archives
  such as `.sit` or `.zip` that contain a disk image work too.
- **Looking inside an image without the Mac:** in the **Filesystem**
  panel, expand any disk image or archive to browse its files, and **Save
  to computer…** the ones you want. This shows the image as stored — not
  changes the Mac has made since (see
  [Disks and images](disks-and-images.md) §6).

## 3. Text

There is no copy and paste between your computer and the Mac yet. For
text, save it as a file and use the shared folder.
