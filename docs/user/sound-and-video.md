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
provide it, or its graphics stop working, the page is replaced by a short
explanation — for example **WebGL 2 is required** or **The browser’s
graphics stopped working** — with what to try and a **Reload** button. See
[Troubleshooting](troubleshooting.md) §1 for each message.

## 5. Recording the screen and sound

The **Record** button on the toolbar (●) records the Mac's screen and
sound to a video file, exactly as the Mac drew them: nothing is lost to
compression, and the recording follows the Mac's own clock, so it plays at
the original's speed whatever the speed mode (§2 of
[The interface](interface.md)). A paused machine records nothing; Max
speed records the same video, only sooner.

1. Click **Record**, choose the options if you want others than last
   time — the frame rate (**Full Rate (60 fps)**, **Half Rate (30 fps)**,
   **Quarter Rate (15 fps)**) and **Record Sound** — and click **Start
   Recording**. The status bar shows a red dot, the time recorded and the
   file's size.
2. Click the red stop button to finish. A notice names the file, with
   **Download** (saves it to your computer) and **Show in Files** (the
   Files tab, where every recording is kept, in `/opfs/recordings/`).

The file is a Matroska video (`.mkv`). It plays in VLC, mpv and ffplay,
and YouTube takes Matroska uploads; browsers, QuickTime Player and phones do
not play it. To make an MP4 that plays everywhere, scaled up four times
with square pixels (small videos look poor on YouTube), use ffmpeg:

```bash
ffmpeg -i plus.mkv -vf scale=iw*4:ih*4:flags=neighbor -c:v libx264 -crf 16 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart plus.mp4
```

Good to know:

- **Hiding the tab** (switching tabs, minimising, closing) finishes the
  file at once, so it is complete even if the browser then closes the
  tab. When you come back, the recording carries on in a new file named
  with **-2**, **-3** and so on; **Download** saves all of them.
- **A new screen size** the recording cannot hold (a larger resolution,
  or thousands of colours after a recording started in 256), and starting
  a different machine, also carry on in a new file.
- **Dropped frames**: when your computer cannot keep up, a frame is
  repeated instead, and the status bar counts it. The sound stays in
  step. Half or quarter rate needs less.
- **3D and GPU-drawn screens** (the Voodoo2 or Rage 128 with WebGPU
  rendering) are recorded at half rate at most.
- If the page shows that browser storage could not be opened, a
  recording is lost when the page closes: download it first. A notice
  says so when you start.
