// The emulated LaserWriter, as the page shows it: what the printer is doing
// (the status bar) and the last document it printed (the viewer).
//
// The activity comes from the core: the PAP layer announces its status
// string on every change (a `printer_status` event, appletalk_printer.c), in
// the form a workstation reads it -- "status: idle", "status: starting up",
// "status: busy; source: AppleTalk; job: <name>", "status: printing; ...;
// page: <n>", or idle with "; error: <why>" after a failed job.  The document
// comes from the interpreter worker (printer/platen.ts), which posts each
// finished PDF to the page.

export type PrinterActivity = 'idle' | 'starting' | 'busy' | 'printing' | 'error';

export interface PrintedDocument {
  name: string; // <job id, 5 digits>-<title>.pdf
  title: string; // the driver's job name, "" when it gave none
  pages: number;
  url: string; // an object URL for the PDF
}

export interface PrinterState {
  activity: PrinterActivity;
  job: string; // the running job's name, "" when none or unknown
  page: number; // pages shown so far by the running job
  error: string; // why the last job failed, "" when it did not
  status: string; // the raw PAP status string
  // The last finished document, kept so the viewer can show it again; its
  // URL is released when the next one replaces it.
  document: PrintedDocument | null;
  viewerOpen: boolean;
}

export const printer: PrinterState = $state({
  activity: 'idle',
  job: '',
  page: 0,
  error: '',
  status: 'status: idle',
  document: null,
  viewerOpen: false,
});

// The value of `key` in a PAP status string ("status: busy; job: X" ->
// "busy" for "status"), or "" when absent.
function field(status: string, key: string): string {
  for (const part of status.split(';')) {
    const at = part.indexOf(':');
    if (at >= 0 && part.slice(0, at).trim() === key) return part.slice(at + 1).trim();
  }
  return '';
}

// Takes the core's status string (the printer_status event).
export function setPrinterStatus(status: string): void {
  printer.status = status;
  const state = field(status, 'status');
  const error = field(status, 'error');
  printer.job = field(status, 'job');
  const page = Number.parseInt(field(status, 'page'), 10);
  printer.page = Number.isFinite(page) ? page : 0;
  if (state === 'starting up') {
    printer.activity = 'starting';
    printer.error = '';
  } else if (state === 'busy') {
    printer.activity = 'busy';
    printer.error = '';
  } else if (state === 'printing') {
    printer.activity = 'printing';
    printer.error = '';
  } else if (error) {
    printer.activity = 'error';
    printer.error = error;
  } else {
    printer.activity = 'idle';
  }
}

// A finished document from the interpreter worker: kept, and shown in the
// viewer.  The previous document's URL is released.
export function showPrintedDocument(doc: {
  name: string;
  title: string;
  pages: number;
  pdf: Uint8Array;
}): void {
  const url = URL.createObjectURL(new Blob([doc.pdf as BlobPart], { type: 'application/pdf' }));
  if (printer.document) URL.revokeObjectURL(printer.document.url);
  printer.document = { name: doc.name, title: doc.title, pages: doc.pages, url };
  printer.viewerOpen = true;
}

// Shows the last document again (the status bar's button).
export function reopenPrintedDocument(): void {
  if (printer.document) printer.viewerOpen = true;
}

// Closes the viewer.  The document stays, so the status bar can reopen it;
// a tab it was opened in keeps working.
export function closePrintedDocument(): void {
  printer.viewerOpen = false;
}

// For tests: back to a printer that has done nothing.
export function _resetPrinterForTests(): void {
  if (printer.document) URL.revokeObjectURL(printer.document.url);
  printer.activity = 'idle';
  printer.job = '';
  printer.page = 0;
  printer.error = '';
  printer.status = 'status: idle';
  printer.document = null;
  printer.viewerOpen = false;
}
