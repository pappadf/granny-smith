import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import Modal from '@/components/common/Modal.svelte';

describe('Modal', () => {
  it('renders nothing when open is false', () => {
    const { container } = render(Modal, { open: false });
    expect(container.querySelector('.modal-backdrop')).toBeNull();
  });

  it('renders backdrop + card when open is true', () => {
    const { container } = render(Modal, { open: true, title: 'Hi' });
    expect(container.querySelector('.modal-backdrop')).not.toBeNull();
    expect(container.querySelector('.modal-title')?.textContent).toBe('Hi');
  });

  it('clicking the backdrop calls onClose when dismissible', async () => {
    const onClose = vi.fn();
    const { container } = render(Modal, { open: true, onClose });
    const backdrop = container.querySelector('.modal-backdrop') as HTMLElement;
    await fireEvent.click(backdrop);
    expect(onClose).toHaveBeenCalled();
  });

  it('clicking the backdrop does nothing when not dismissible', async () => {
    const onClose = vi.fn();
    const { container } = render(Modal, { open: true, onClose, dismissible: false });
    const backdrop = container.querySelector('.modal-backdrop') as HTMLElement;
    await fireEvent.click(backdrop);
    expect(onClose).not.toHaveBeenCalled();
  });

  it('escape key calls onClose when dismissible', async () => {
    const onClose = vi.fn();
    render(Modal, { open: true, onClose });
    await fireEvent.keyDown(document, { key: 'Escape' });
    expect(onClose).toHaveBeenCalled();
  });

  it('clicking inside the card does not dismiss', async () => {
    const onClose = vi.fn();
    const { container } = render(Modal, { open: true, onClose, title: 'X' });
    const card = container.querySelector('.modal-card') as HTMLElement;
    await fireEvent.click(card);
    expect(onClose).not.toHaveBeenCalled();
  });
});
