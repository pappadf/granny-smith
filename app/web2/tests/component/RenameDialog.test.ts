import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import RenameDialog from '@/components/panel-views/filesystem/RenameDialog.svelte';

// A rename target is a single path component. A '/' would silently turn the
// rename into a move — worst case into the item's own subtree, which the
// C-side storage.mv copy fallback would mangle.
describe('RenameDialog validation', () => {
  function setup() {
    const onSubmit = vi.fn();
    const utils = render(RenameDialog, {
      open: true,
      initial: 'old.txt',
      onSubmit,
      onClose: () => {},
    });
    const input = utils.container.ownerDocument.querySelector('#rename-input') as HTMLInputElement;
    return { ...utils, onSubmit, input };
  }

  it('submits a plain name', async () => {
    const { getByRole, onSubmit, input } = setup();
    await fireEvent.input(input, { target: { value: 'new.txt' } });
    await fireEvent.click(getByRole('button', { name: 'Rename' }));
    expect(onSubmit).toHaveBeenCalledWith('new.txt');
  });

  it.each(['a/b', 'a\\b', '.', '..'])('rejects %j with an inline error', async (bad) => {
    const { getByRole, onSubmit, input, container } = setup();
    await fireEvent.input(input, { target: { value: bad } });
    await fireEvent.click(getByRole('button', { name: 'Rename' }));
    expect(onSubmit).not.toHaveBeenCalled();
    await waitFor(() => {
      expect(container.ownerDocument.querySelector('.rename-error')).not.toBeNull();
    });
  });
});
