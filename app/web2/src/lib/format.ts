// Byte counts as short display strings: the one formatter every size the UI
// shows goes through (file lists, checkpoints, import and download progress,
// storage messages).

const UNITS = ['KB', 'MB', 'GB', 'TB'];

// "512 B", "4.4 KB", "800 KB", "1.4 MB": binary units, one decimal below 10;
// "0 B" for nothing, a negative or a non-number.
export function formatSize(bytes: number): string {
  if (!(bytes > 0)) return '0 B';
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  let v = bytes / 1024;
  let u = 0;
  while (v >= 1024 && u < UNITS.length - 1) {
    v /= 1024;
    u++;
  }
  // 9.96 would print as "10.0": from there on, whole units.
  const text = v < 9.95 ? v.toFixed(1) : String(Math.round(v));
  return `${text} ${UNITS[u]}`;
}
