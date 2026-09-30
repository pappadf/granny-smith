import { describe, it, expect, beforeEach, vi } from 'vitest';
import {
  printer,
  setPrinterStatus,
  showPrintedDocument,
  closePrintedDocument,
  reopenPrintedDocument,
  _resetPrinterForTests,
} from '@/state/printer.svelte';

// jsdom has no object URLs.
let urls = 0;
const createObjectURL = vi.fn(() => `blob:doc-${++urls}`);
const revokeObjectURL = vi.fn();

beforeEach(() => {
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = createObjectURL;
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = revokeObjectURL;
  _resetPrinterForTests();
  urls = 0;
  createObjectURL.mockClear();
  revokeObjectURL.mockClear();
});

const doc = (name: string) => ({
  name,
  title: 'MacOS7',
  pages: 1,
  pdf: new Uint8Array([0x25, 0x50, 0x44, 0x46]),
});

describe('printer status', () => {
  it('reads the PAP status strings the core announces', () => {
    setPrinterStatus('status: starting up');
    expect(printer.activity).toBe('starting');

    setPrinterStatus('status: busy; source: AppleTalk; job: MacOS7');
    expect(printer.activity).toBe('busy');
    expect(printer.job).toBe('MacOS7');
    expect(printer.page).toBe(0);

    setPrinterStatus('status: printing; source: AppleTalk; job: MacOS7; page: 2');
    expect(printer.activity).toBe('printing');
    expect(printer.page).toBe(2);

    setPrinterStatus('status: idle');
    expect(printer.activity).toBe('idle');
    expect(printer.job).toBe('');
    expect(printer.status).toBe('status: idle');
  });

  it('a busy status without a job name has no job', () => {
    setPrinterStatus('status: busy; source: AppleTalk');
    expect(printer.activity).toBe('busy');
    expect(printer.job).toBe('');
  });

  it('an idle status carrying an error is a failed job', () => {
    setPrinterStatus('status: idle; error: interpreter refused the job');
    expect(printer.activity).toBe('error');
    expect(printer.error).toBe('interpreter refused the job');
    setPrinterStatus('status: busy; source: AppleTalk');
    expect(printer.error).toBe('');
  });
});

describe('printed documents', () => {
  it('a document opens the viewer; closing keeps it for the status bar', () => {
    showPrintedDocument(doc('00002-MacOS7.pdf'));
    expect(printer.viewerOpen).toBe(true);
    expect(printer.document).toMatchObject({
      name: '00002-MacOS7.pdf',
      title: 'MacOS7',
      pages: 1,
      url: 'blob:doc-1',
    });
    closePrintedDocument();
    expect(printer.viewerOpen).toBe(false);
    expect(printer.document).not.toBeNull();
    expect(revokeObjectURL).not.toHaveBeenCalled();
    reopenPrintedDocument();
    expect(printer.viewerOpen).toBe(true);
  });

  it('the next document replaces the last and releases its URL', () => {
    showPrintedDocument(doc('00002-MacOS7.pdf'));
    showPrintedDocument(doc('00004-MacOS7.pdf'));
    expect(printer.document?.name).toBe('00004-MacOS7.pdf');
    expect(revokeObjectURL).toHaveBeenCalledWith('blob:doc-1');
  });

  it('reopening with nothing printed does nothing', () => {
    reopenPrintedDocument();
    expect(printer.viewerOpen).toBe(false);
  });
});
