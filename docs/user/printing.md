# Printing

Two printers are available to the emulated Mac: an **ImageWriter**
dot-matrix printer on a serial port, and a **LaserWriter** on the
AppleTalk network. Whatever the Mac prints arrives in your browser as a
PDF.

## 1. The ImageWriter

### 1.1 Connecting it

1. In the [New Machine dialog](new-machine.md), set **ImageWriter** to
   **ImageWriter** or **ImageWriter II** (or add `imagewriter=imagewriter2`
   to a [link](url-parameters.md)).
2. The printer is plugged into the serial port AppleTalk does not use: the
   **modem port** while AppleTalk is active, the **printer port** when it
   is inactive. On a Lisa it is **Serial A** — in the Lisa Office System,
   assign it there under Preferences → Device Connections.

### 1.2 Printing from the Mac

1. Open the **Chooser** from the Apple menu.
2. Select **ImageWriter** and click the port it is connected to (§1.1).
3. Close the Chooser and print from any application.

A few seconds after the last data arrives, the document opens in the print
viewer (§3). Text prints in the ImageWriter II's own draft and
correspondence fonts; colour jobs print in colour.

### 1.3 Printer settings

The ImageWriter's settings are under **System → machine → imagewriter**,
or in the terminal (right-click the printer in the status bar for a
shortcut):

```
machine.imagewriter.model = "imagewriter"        the original ImageWriter (or "imagewriter2")
machine.imagewriter.paper = "a4"                 letter, a4, legal, fanfold-letter, fanfold-15in
machine.imagewriter.connection = "localtalk"     an ImageWriter II with the LocalTalk Option card
```

Connected over LocalTalk, it appears in the Chooser under **AppleTalk
ImageWriter** as **Virtual ImageWriter**. Other connections are
`"serial-a"` (modem port), `"serial-b"` (printer port) and `"none"`.

## 2. The LaserWriter

When AppleTalk is active, the network has a PostScript printer named
**Virtual LaserWriter**.

1. In the Chooser, select **LaserWriter** (with AppleTalk active).
2. Select **Virtual LaserWriter** and close the Chooser.
3. Print as usual.

## 3. The print viewer

Each finished job opens in a viewer titled with the printer, the document
name and the number of pages.

- **Save to computer…** downloads the PDF.
- **Open in new tab** shows it in a browser tab.
- **Close** dismisses the viewer.

Printed documents are not stored by the emulator; save the ones you want.
The printer button in the status bar reopens the last document. In a
browser without a built-in PDF viewer, the PDF is downloaded directly.
