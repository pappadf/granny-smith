// Pure helpers for machine display.

// Drop the leading "Macintosh " for compact display (status bar etc.).
export function shortModel(model: string): string {
  return model.replace(/^Macintosh\s+/i, '');
}

// A RAM size in KB as a label: "512 KB", "4 MB", "2.5 MB".  RAM is a number
// everywhere else — the profile, the boot document, machine.ram — and only
// becomes text here.
export function formatRamKb(kb: number): string {
  if (kb >= 1024 && kb % 1024 === 0) return `${kb / 1024} MB`;
  if (kb >= 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${kb} KB`;
}
