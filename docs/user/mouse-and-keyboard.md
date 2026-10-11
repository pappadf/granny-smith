# Mouse and keyboard

The Mac gets your mouse and keyboard only when you hand them over. That
keeps your keys working in the emulator's own panels — the terminal, the
dialogs, the debugger — the rest of the time.

## 1. Capturing and releasing

1. **Click the Mac's screen** while the machine is running. The browser
   hides your pointer and the Mac's pointer follows your mouse. The click
   that captures is not passed on to the Mac.
2. Use the mouse and keyboard as on a real Mac.
3. **Press Esc** to release them. The browser uses Esc for this, so to
   send Esc to the Mac use the terminal (§4).

The mouse is also released when you pause or shut down the machine, when a
dialog opens, and when a printed document is shown. If the window loses
focus, any keys you were holding are let go, so no key stays stuck down.

While the mouse is *not* captured, typing goes to the emulator's panels,
not to the Mac.

## 2. Key map

Keys are mapped by their position, the same on every model.

| Your key | Mac key |
|---|---|
| Windows key (PC) / ⌘ Command (Mac) | ⌘ Command |
| Alt / ⌥ Option | ⌥ Option |
| Ctrl | Control |
| Shift | Shift |
| Backspace | Delete |
| Enter / Return | Return |
| Esc | releases the mouse (send Esc to the Mac from the terminal, §4) |
| Arrow keys | Arrow keys |
| Page Up, Page Down, Home, End, Delete (forward) | the same keys of the Apple Extended Keyboard |
| Insert | Help |
| Num Lock | keypad Clear |
| F1 – F12 | F1 – F12 (on extended keyboards) |
| Numeric keypad | Numeric keypad |
| ` and the extra key left of Z on international keyboards | the corresponding Mac keys |

On a PC, **Command is the Windows key**, not Ctrl. Some browser and system
shortcuts (for example Windows-key combinations handled by the operating
system) never reach the page; keys the Mac does not have are left to the
browser.

### 2.1 Lisa and Macintosh XL

The Lisa keyboard has no Control key. Ctrl is sent as the Apple
(Command) key instead, so Ctrl-D works as Apple-D — for example to end
input in Xenix — and the browser does not act on it.

## 3. Caps Lock

The Mac's Caps Lock follows your keyboard's Caps Lock light, and it stays
set across restarts, as the locking key on a real Mac keyboard does. The
**⇪ Caps Lock** chip in the status bar shows its state; click it to latch
Caps Lock on a keyboard that has none. (Holding Caps Lock down at startup
matters on some systems — for example to start Mac OS 8 (Copland) from a
volume that has it installed.)

## 4. Typing from the terminal

The terminal can press keys and move the mouse on the Mac without
capturing anything — handy for keys the browser keeps for itself, or for
repeatable steps:

```
machine.adb.keyboard.type "Hello"          type a short line (newline = Return)
machine.adb.keyboard.press "esc"           tap one key (by name or key code)
machine.adb.keyboard.down "command"        hold a key …
machine.adb.keyboard.press "q"             … press another …
machine.adb.keyboard.up "command"          … and let go: Command-Q
machine.adb.mouse.move 100 40 "global"     move the pointer to screen position 100, 40
machine.adb.mouse.click true               press the button (false releases it)
```

These work on every Macintosh model, the Plus included. See [The terminal](terminal.md).

## 5. Copy and paste

Text cannot yet be pasted from your computer into the Mac. To move text or
files between the two, use a shared folder or a disk image — see
[Sharing files with the Mac](sharing-files.md). In the terminal panel, the
usual copy and paste shortcuts work.
