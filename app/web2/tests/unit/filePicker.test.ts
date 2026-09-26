// openFilePicker (bus/upload.ts) resolves on both ends of the OS dialog:
// `change` with the files, `cancel` with [] -- and removes its hidden input
// either way (a cancel used to leave the promise pending and the input
// behind).
import { describe, it, expect, vi, afterEach } from 'vitest';
import { openFilePicker } from '@/bus/upload';

afterEach(() => vi.restoreAllMocks());

function lastInput(): HTMLInputElement {
  const inputs = document.body.querySelectorAll('input[type="file"]');
  return inputs[inputs.length - 1] as HTMLInputElement;
}

describe('openFilePicker', () => {
  it('resolves [] on cancel and removes its input', async () => {
    vi.spyOn(HTMLInputElement.prototype, 'click').mockImplementation(() => {});
    const p = openFilePicker('.img');
    const input = lastInput();
    expect(input.accept).toBe('.img');
    input.dispatchEvent(new Event('cancel'));
    await expect(p).resolves.toEqual([]);
    expect(document.body.contains(input)).toBe(false);
  });

  it('resolves the chosen files on change and removes its input', async () => {
    vi.spyOn(HTMLInputElement.prototype, 'click').mockImplementation(() => {});
    const p = openFilePicker('', false);
    const input = lastInput();
    expect(input.multiple).toBe(false);
    const file = new File(['x'], 'disk.img');
    Object.defineProperty(input, 'files', { value: [file] });
    input.dispatchEvent(new Event('change'));
    await expect(p).resolves.toEqual([file]);
    expect(document.body.contains(input)).toBe(false);
    // A late cancel after the change is ignored.
    input.dispatchEvent(new Event('cancel'));
  });
});
