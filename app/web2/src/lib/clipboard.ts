// Writes `text` to the clipboard; false when the browser refused (no
// clipboard permission, or an insecure context), for the caller to say so.
export async function copyText(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    return false;
  }
}
