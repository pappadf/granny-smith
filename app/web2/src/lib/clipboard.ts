// Writes `text` to the clipboard.
export async function copyText(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
  } catch {
    // No clipboard permission (or an insecure context): nothing to do.
  }
}
