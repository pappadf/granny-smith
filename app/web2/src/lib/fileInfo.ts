// The Filesystem tab's Date modified column: a Unix-seconds modification
// time as a short display string (its Size column is lib/format formatSize).

import { localStamp } from './time';

// "2026-10-04 14:32" in local time; "" for 0 (unknown).
export function formatMtime(secs: number): string {
  if (!secs) return '';
  return localStamp(new Date(secs * 1000));
}
