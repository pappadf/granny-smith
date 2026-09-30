// Offsets between a JS string's UTF-16 code units and its UTF-8 bytes: the
// core's shell reports and takes byte offsets (shell.complete's cursor and
// span, shell.highlight's spans), the editor works in UTF-16.

// UTF-8 bytes of the first `index` code units of `s`.
export function utf16ToUtf8(s: string, index: number): number {
  let bytes = 0;
  const end = Math.min(index, s.length);
  for (let i = 0; i < end; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) bytes += 1;
    else if (c < 0x800) bytes += 2;
    else if (c >= 0xd800 && c < 0xdc00 && i + 1 < end) {
      bytes += 4; // a surrogate pair is one 4-byte sequence
      i++;
    } else bytes += 3;
  }
  return bytes;
}

// The code-unit index at which `bytes` UTF-8 bytes of `s` end.
export function utf8ToUtf16(s: string, bytes: number): number {
  let b = 0;
  let i = 0;
  while (i < s.length && b < bytes) {
    const c = s.charCodeAt(i);
    if (c < 0x80) b += 1;
    else if (c < 0x800) b += 2;
    else if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.length) {
      b += 4;
      i++;
    } else b += 3;
    i++;
  }
  return i;
}
