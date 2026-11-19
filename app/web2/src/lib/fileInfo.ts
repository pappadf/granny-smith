// The Filesystem tab's Size and Date modified columns: a byte count and a
// Unix-seconds modification time as short display strings.

import { localStamp } from './time';

const UNITS = ['KB', 'MB', 'GB', 'TB'];

// "512 B", "4.4 KB", "800 KB", "1.4 MB": binary units, one decimal below 10.
export function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
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

// "2026-10-04 14:32" in local time; "" for 0 (unknown).
export function formatMtime(secs: number): string {
  if (!secs) return '';
  return localStamp(new Date(secs * 1000));
}
