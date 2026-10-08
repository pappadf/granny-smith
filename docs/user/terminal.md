# The terminal

The **Terminal** panel is a command shell into the emulator. Everything
the interface can do — and a good deal more — can be done by typing
commands: inserting disks, saving screenshots, changing settings, saving
checkpoints. You never *need* it, but it is the quickest way to do many
things, and the only way to do some.

## 1. Basics

- Type a command at the `gs>` prompt and press **Enter**. The prompt shows
  the running model (`gs iici>`); when the machine is stopped it also shows
  the processor's position (`gs iici @00401F6E>`).
- The right-hand side of the panel is a **command browser**: every command
  and setting as a tree, with a one-line description. Click an entry to
  see its usage.
- Results print below the command; most commands that do something print
  `true` when they succeed.

| Keys | Does |
|---|---|
| **↑** / **↓** | Previous / next command from history |
| **Tab** | Complete the command or path |
| **Ctrl-L** | Clear the terminal |
| **Ctrl-C** | Copy the selection; with nothing selected, interrupt the running command |
| **Ctrl-F** (**⌘-F** on a Mac) | Find in the output (next, previous, match case) |
| **Ctrl-Shift-Space** | Show the usage of the command being typed |
| **Ctrl-V** | Paste |

Right-click the output for **Copy**, **Copy output**, **Copy as commands**,
**Copy value as JSON**, **Paste**, **Select all** and **Clear**.

## 2. Finding your way

Everything is a path of names separated by dots, rooted at a few objects:
`machine` (the emulated computer), `files`, `checkpoint`, `pacing`,
`scheduler`, `debug`, `log`, `appletalk`, `catalog`, `shell`.

```
help                         what is at the top level
objects machine              the parts of the machine
methods files                what files can do
attributes machine.screen    the values machine.screen has
help files.hd_create         usage, arguments and an example
```

Reading and changing a value:

```
machine.screen.width                     → 640
machine.imagewriter.paper = "a4"         change a setting
```

Calling a method: arguments after the name, separated by spaces, or in
parentheses:

```
files.hd_create /opfs/images/hd/work.img 80mb
machine.screen.save("/opfs/upload/screen.png")
```

Text with spaces goes in double quotes. The System panel shows the same
tree; right-click a row there and **Copy path** to get the name to type.

## 3. Everyday commands

### 3.1 Files

| Command | Does |
|---|---|
| `ls [dir]`, `cd <dir>`, `pwd` | List, change and show the current folder |
| `mkdir <dir>`, `cp <src> <dst>`, `mv <src> <dst>`, `rm <path>` | Make a folder; copy, move/rename, delete |
| `cat <file>` | Print a file |
| `files.list <path>` | List a folder, and also the inside of disk images and archives |
| `files.download <path>` | Download a file to your computer |
| `files.probe <image>` | Say what kind of disk image a file is |
| `files.partmap <image>` | Print a disk's partition map |
| `files.convert <in> <out>` | Write a compact `.dmg`; add `format=raw` for a plain image |
| `files.archive.extract <archive> [dir]` | Unpack a `.sit`, `.cpt`, `.zip`, … |
| `files.hd_create <path> <size>` | Make a blank hard disk (`80mb`, `HD230SC`, `100m`) |
| `files.fd_create <path> <size>` | Make a blank floppy (`800k`, `1440k`) |

The browser storage is `/opfs`; your images are in `/opfs/images/<kind>/`.
`/tmp` is temporary and is lost when the page reloads.

### 3.2 Drives and disks

| Command | Does |
|---|---|
| `machine.floppy.drive[0].insert "<image>"` | Insert a floppy into drive 0 (`[1]` for the second) |
| `machine.floppy.drive[0].eject` | Eject it |
| `machine.attach_hd "<image>" [n]` | Attach a hard disk to the machine's n-th hard-disk position (0 is the startup disk) |
| `machine.attach_cdrom "<image>"` | Put a CD into the CD-ROM drive |
| `machine.eject_media <bus> [id]` | Take a medium out (`floppy`, `scsi`, `profile`; for example `machine.eject_media scsi 3`) |

### 3.3 The machine

| Command | Does |
|---|---|
| `run`, `stop` | Run and stop the machine (as the toolbar's run/pause) |
| `machine.restart` | Power-cycle the machine: memory is cleared; PRAM, the clock and disks stay |
| `reset` (`machine.reset`) | Press the reset switch: keeps memory, PRAM and disks |
| `machine.screen.save("<file>.png")` | Save a screenshot of the Mac's screen |
| `pacing.mode = "paced"` | Speed mode: `paced` (Real), `accelerated` (Faster), `turbo` (Max) |
| `checkpoint.save "<file>"` | Save the whole machine to a file in storage |
| `checkpoint.load "<file>"` | Restore it |
| `checkpoint.auto = false` | Stop the automatic background checkpoints (`true` to resume) |

### 3.4 Input, printing, network, logs

| Command | Does |
|---|---|
| `machine.adb.keyboard.type "text"` | Type into the Mac ([Mouse and keyboard](mouse-and-keyboard.md) §4) |
| `machine.adb.mouse.move x y "global"`, `machine.adb.mouse.click true` | Move the Mac's pointer, press the button |
| `machine.imagewriter.connection = "serial-b"` | Connect the ImageWriter ([Printing](printing.md)) |
| `appletalk.afp.volumes.add "<name>" "<folder>"` | Share another folder with the Mac as an AppleShare volume |
| `log.set <category> <level>` | Turn up logging for one area, shown in the Logs panel ([Debugger and logs](debugger.md)) |

## 4. Saving a screenshot to your computer

```
machine.screen.save("/opfs/upload/screen.png")
files.download /opfs/upload/screen.png
```
