import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';

const setCatLevel = vi.fn<(cat: string, level: number) => Promise<boolean>>(async () => true);
const setCatStdout = vi.fn<(cat: string, on: boolean) => Promise<boolean>>(async () => true);
vi.mock('@/state/logs.svelte', async (orig) => ({
  ...(await orig<typeof import('@/state/logs.svelte')>()),
  refreshCatLevels: async () => undefined,
  setCatLevel: (cat: string, level: number) => setCatLevel(cat, level),
  setCatStdout: (cat: string, on: boolean) => setCatStdout(cat, on),
}));

import CategoryLevelsPopover from '@/components/panel-views/logs/CategoryLevelsPopover.svelte';
import { logs } from '@/state/logs.svelte';

beforeEach(() => {
  setCatLevel.mockClear();
  setCatStdout.mockClear();
  logs.catLevels = { floppy: 0 };
  logs.catStdout = { floppy: true };
});

// Levels are any non-negative integer (the core uses levels up to 13 and
// 40); negatives and non-integers snap back to the core's level.
describe('CategoryLevelsPopover', () => {
  async function field() {
    const { getByLabelText } = render(CategoryLevelsPopover, { open: true, onClose: () => {} });
    return getByLabelText('Level for floppy') as HTMLInputElement;
  }

  it('has no upper limit and sends a level above 9', async () => {
    const input = await field();
    expect(input.getAttribute('max')).toBeNull();
    input.value = '40';
    await fireEvent.change(input);
    await waitFor(() => expect(setCatLevel).toHaveBeenCalledWith('floppy', 40));
  });

  it.each(['-1', '1.5', ''])('rejects %j and restores the level', async (bad) => {
    const input = await field();
    input.value = bad;
    await fireEvent.change(input);
    expect(setCatLevel).not.toHaveBeenCalled();
    expect(input.value).toBe('0');
  });

  // Every line reaches the Logs panel; the Terminal copy is per category.
  it("turns a category's Terminal copy off", async () => {
    const { container } = render(CategoryLevelsPopover, { open: true, onClose: () => {} });
    const box = container.querySelector('.cat-stdout input') as HTMLInputElement;
    expect(box.checked).toBe(true);
    await fireEvent.click(box);
    await waitFor(() => expect(setCatStdout).toHaveBeenCalledWith('floppy', false));
  });
});
