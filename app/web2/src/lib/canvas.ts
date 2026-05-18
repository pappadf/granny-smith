// Mock screen painter — verbatim port of the prototype's paintScreen() at
// app.js:884-921. Renders a checkerboard + Mac menu bar + Untitled window
// to the supplied 512x342 canvas. Phase 3 replaces this with the real
// emulator framebuffer.

export function paintMockScreen(canvas: HTMLCanvasElement): void {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // White background.
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  // Black menu bar (20 px tall) with mock menu titles.
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, canvas.width, 20);
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold 10px monospace';
  ctx.fillText('  File  Edit  View  Special', 6, 14);

  // Checkerboard desktop (8 px squares, gray on white).
  const s = 8;
  ctx.fillStyle = '#c0c0c0';
  for (let y = 20; y < canvas.height; y += s) {
    for (let x = 0; x < canvas.width; x += s) {
      if ((x / s + y / s) % 2 === 0) ctx.fillRect(x, y, s, s);
    }
  }

  // Mock "Untitled" window.
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(60, 60, 280, 180);
  ctx.strokeStyle = '#000000';
  ctx.lineWidth = 1;
  ctx.strokeRect(60.5, 60.5, 280, 180);

  // Window title bar.
  ctx.fillStyle = '#000000';
  ctx.fillRect(60, 60, 280, 14);
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold 10px monospace';
  ctx.fillText('Untitled', 170, 72);

  // Window body text.
  ctx.fillStyle = '#000000';
  ctx.font = '10px monospace';
  ctx.fillText('Welcome to Granny Smith.', 72, 96);
  ctx.fillText('This is a mocked Macintosh screen.', 72, 112);
  ctx.fillText('(Real emulator arrives in Phase 3.)', 72, 128);
}
