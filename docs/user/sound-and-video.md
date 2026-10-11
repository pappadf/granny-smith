# Sound, camera and 3D

## 1. Sound output

The Mac's sound plays through your browser. Browsers allow a page to play
sound only after you have interacted with it, so the sound starts with your
first click or key press on the page (a link that boots straight away may
be silent until then).

There is no volume control in the emulator: use the Mac's own **Sound**
control panel (levels 0–7) and your computer's volume.

## 2. Microphone and camera (Quadra 840AV, Centris/Quadra 660AV)

The AV Macs have a sound input and a video input. Granny Smith can connect
them to your computer's microphone and camera; the toolbar then shows a
microphone and a camera button. Your browser asks for permission the first
time.

### 2.1 Microphone

1. Click the **microphone** button ("Connect microphone to the sound
   input"). The browser asks to use the microphone.
2. Once connected, click the button again for a menu: **Disconnect
   microphone**, **System default**, or a specific input device by name.

The browser's echo cancellation, noise suppression and automatic gain are
turned off, so the Mac hears the microphone as it is. If the Mac starts
recording while nothing is connected, a notice says so. If the chosen
device disappears, the system default is used.

### 2.2 Camera

Click the **camera** button ("Connect camera to the video input") to
connect your camera, and click it again to disconnect. The Mac sees it as
a video source (for example in the Apple Video Player); the picture is
about 640 × 480.

### 2.3 Speech recognition

With the microphone connected, the AV Macs' own PlainTalk speech
recognition works as on the real machines: install and turn on Speakable
Items / Speech Recognition on the Mac and speak commands such as "What day
is it?".

## 3. 3D graphics: the Voodoo2 card

The PCI Power Macs (7500, 8500, 9500, G3) and the Network Servers can take
a **3dfx Voodoo2** 3D card: in the [New Machine dialog](new-machine.md),
**Add card** → **3dfx Voodoo2**. Games that use it (Glide or the Mac's 3D
drivers) then render through it.

The card has a **Rendering** option:

- **Software** works in any browser.
- **WebGPU** uses your graphics card; it is offered only
  when your browser supports WebGPU (the card list then also shows
  **3dfx Voodoo2 (WebGPU)**, the same card with this option chosen).

A machine saved with WebGPU rendering and restored in a browser without
WebGPU falls back to software rendering.

## 4. Display requirements

The emulator draws the Mac's screen with WebGL 2. If the browser cannot
provide it, the page says **Hardware acceleration required** (or **WebGL is
unavailable**) with suggestions — turn on hardware acceleration in the
browser's settings, restart the browser, update graphics drivers — and a
**Retry** button. See [Troubleshooting](troubleshooting.md).
