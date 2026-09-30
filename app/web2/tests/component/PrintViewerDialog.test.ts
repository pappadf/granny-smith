import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import { tick } from 'svelte';
import PrintViewerDialog from '@/components/dialogs/PrintViewerDialog.svelte';
import { printer, showPrintedDocument, _resetPrinterForTests } from '@/state/printer.svelte';

beforeEach(() => {
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = () => 'blob:the-pdf';
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = () => undefined;
  _resetPrinterForTests();
});

const show = () =>
  showPrintedDocument({
    name: '00002-MacOS7.pdf',
    title: 'MacOS7',
    pages: 3,
    pdf: new Uint8Array([0x25, 0x50, 0x44, 0x46]),
  });

describe('PrintViewerDialog', () => {
  it('shows nothing until a document is printed', () => {
    const { container } = render(PrintViewerDialog);
    expect(container.querySelector('.modal-backdrop')).toBeNull();
  });

  it('shows the document in a frame, with a download under its name', async () => {
    const { container } = render(PrintViewerDialog);
    show();
    await tick();
    expect(container.querySelector('.modal-title')?.textContent).toBe(
      'LaserWriter: MacOS7 (3 pages)',
    );
    const frame = container.querySelector('iframe.pdf-frame') as HTMLIFrameElement;
    expect(frame.getAttribute('src')).toBe('blob:the-pdf');
    const links = Array.from(container.querySelectorAll('a.btn')) as HTMLAnchorElement[];
    const download = links.find((a) => a.hasAttribute('download'));
    expect(download?.getAttribute('download')).toBe('00002-MacOS7.pdf');
    expect(download?.getAttribute('href')).toBe('blob:the-pdf');
    const tab = links.find((a) => a.getAttribute('target') === '_blank');
    expect(tab?.getAttribute('href')).toBe('blob:the-pdf');
  });

  it('Close hides the viewer and keeps the document', async () => {
    const { container, getByText } = render(PrintViewerDialog);
    show();
    await tick();
    await fireEvent.click(getByText('Close'));
    expect(container.querySelector('.modal-backdrop')).toBeNull();
    expect(printer.viewerOpen).toBe(false);
    expect(printer.document).not.toBeNull();
  });
});
