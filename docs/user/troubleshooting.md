# Troubleshooting

Find the symptom, then try the fix. The first line of the **Terminal**
panel shows the emulator's build date — include it when you report a
problem.

## 1. The page does not start

| Symptom | Fix |
|---|---|
| The page reloads once on the first visit | Normal: it installs a helper that enables the multi-threaded features the emulator needs. |
| **Hardware acceleration required** / **WebGL is unavailable** | The emulator needs WebGL 2. Turn on hardware acceleration in the browser settings ("Use hardware acceleration when available"), restart the browser, update the graphics driver, then click **Retry**. `chrome://gpu` (or `about:support` in Firefox) shows what the browser reports. |
| **Browser storage is unavailable** | Allow site data for the page, or leave private/incognito browsing, then reload. |
| **The emulator could not start** | Click **Reload** — especially right after the emulator has been updated. |
| Odd drawing or no sound in Safari or Firefox | Use a Chromium-based browser (Chrome, Edge, Brave). Safari is not supported yet; Firefox works only partly. |

## 2. Starting a machine

| Symptom | Fix |
|---|---|
| New Machine says **No ROMs in storage** | Load a ROM first: **Back**, then **Load ROM...** ([Getting started](getting-started.md)). |
| The model I want is not in the list | Its ROM is not loaded, or the file is not that ROM. ROMs are recognised by checksum — compare with [Supported machines](machines.md) §1. |
| A link's notice says **Booted … without …** | A disk in the link could not be downloaded or is not valid for its slot; the notice (and the terminal) give the reason. See [URL parameters](url-parameters.md) §5.4. |
| A link's notice says **speed=…** or **model=…** was ignored | The value is not one the page knows (`speed` is `paced`, `accelerated` or `turbo`), or the ROM cannot boot that model. The machine starts without that setting. |
| A link shows **The machine could not be started** | The reason is under the ROM's row. A network or CORS error means the file's server does not allow other sites to read it; `http://` files cannot be used from an `https://` page; a 404 means the address is wrong. See [URL parameters](url-parameters.md) §5. |
| The machine starts but shows a floppy with a blinking **?** | It found no startup disk. Check the notices: a disk from a link may have failed to download, or may not be bootable on this model. In the New Machine dialog, check **Start up from**. |
| The Mac chimes twice and restarts at the start of System 7.6 or later | Set **Addressing** to **32-bit** (or `addressing=32` in a link). The system otherwise switches the mode itself and restarts — harmless, but slower. |
| A Power Mac 7500/8500/9500 will not start from a System 7.5 disk | These models need System 7.6 or later. |
| "Image saved but not inserted" when loading a disk | The image is stored; insert it from the Images panel or choose it in the dialog's drive menu. |
| A large link takes minutes before anything happens | The files are downloaded whole before the machine starts; the progress view shows how far it is. A 2 GB disk takes several minutes. |

## 3. While running

| Symptom | Fix |
|---|---|
| The Mac ignores the keyboard | Click the Mac's screen first — keys go to the Mac only while it has the mouse. |
| I cannot click the toolbar or panels | The Mac has the mouse: press **Esc**. |
| Command-key shortcuts do not work on a PC | Command is the **Windows key**, not Ctrl ([Mouse and keyboard](mouse-and-keyboard.md)). |
| No sound | Click anywhere on the page once (browsers block sound until you do). Check the Mac's Sound control panel and your computer's volume. |
| The Mac's screen is cut off | Zoom out, scroll the display, or use **Screen Mode → Full Screen**. |
| It runs slowly | Try **Faster** or **Max**. Close other heavy tabs. The status bar's MIPS figure shows the emulated speed. |
| The camera or microphone button is missing | Only the Quadra 840AV and 660AV have video and sound inputs. |

## 4. Saving and storage

| Symptom | Fix |
|---|---|
| No **Continue from saved checkpoint?** after the emulator was updated | Checkpoints belong to the build that made them; after an update they cannot be resumed. Start the machine again from its disks ([Saving and resuming](saving-and-resuming.md) §6). |
| "Not enough browser storage", or "Could not copy … into the browser's storage" when opening a saved-state file | Delete images and checkpoints you no longer need, in the Images and Checkpoints panels. Large hard disks are stored compressed already; a saved-state file is copied into storage while it is opened, so it needs about its own size free. |
| My changes to a disk are not in the file I saved from the Images panel | Stored images are never changed; the Mac's changes live with the machine. Use the shared folder or **Save State** ([Disks and images](disks-and-images.md) §6). |

## 5. Current limitations

These are known and being worked on:

- Text cannot be pasted from your computer into the Mac.
- Remote files must be served with CORS headers to be used in links.
