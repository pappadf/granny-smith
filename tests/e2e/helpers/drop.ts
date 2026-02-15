// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import type { Page } from '@playwright/test';

/**
 * Dispatch a real drop event with a DataTransfer containing the given file.
 * This simulates a true drag/drop as close as possible in Playwright.
 * 
 * @param page - Playwright page
 * @param targetSelector - CSS selector for drop target (usually '#screen' canvas)
 * @param fileName - Name for the dropped file
 * @param fileData - File content as Uint8Array
 * @param mimeType - MIME type (default: application/octet-stream)
 */
export async function dispatchDropEvent(
  page: Page,
  targetSelector: string,
  fileName: string,
  fileData: Uint8Array,
  mimeType: string = 'application/octet-stream'
): Promise<boolean> {
  return page.evaluate(
    ({ selector, name, data, mime }: { selector: string; name: string; data: number[]; mime: string }) => {
      const target = document.querySelector(selector);
      if (!target) {
        console.error(`[drop] target not found: ${selector}`);
        return false;
      }
      
      // Convert data array back to Uint8Array and create a File object
      const bytes = new Uint8Array(data);
      const file = new File([bytes], name, { type: mime });
      
      // Create a DataTransfer object and populate it
      const dataTransfer = new DataTransfer();
      dataTransfer.items.add(file);
      
      // Dispatch dragenter first (to show the drop hint)
      const dragEnter = new DragEvent('dragenter', {
        bubbles: true,
        cancelable: true,
        dataTransfer
      });
      target.dispatchEvent(dragEnter);
      
      // Dispatch dragover (required for drop to work)
      const dragOver = new DragEvent('dragover', {
        bubbles: true,
        cancelable: true,
        dataTransfer
      });
      target.dispatchEvent(dragOver);
      
      // Finally dispatch the drop event
      const drop = new DragEvent('drop', {
        bubbles: true,
        cancelable: true,
        dataTransfer
      });
      target.dispatchEvent(drop);
      
      console.log(`[drop] dispatched drop event with file: ${name} (${bytes.length} bytes)`);
      return true;
    },
    { selector: targetSelector, name: fileName, data: Array.from(fileData), mime: mimeType }
  );
}

/**
 * Dispatch drop event for multiple files at once.
 */
export async function dispatchMultiFileDropEvent(
  page: Page,
  targetSelector: string,
  files: Array<{ name: string; data: Uint8Array; mime?: string }>
): Promise<boolean> {
  return page.evaluate(
    ({ selector, filesData }: { selector: string; filesData: Array<{ name: string; data: number[]; mime: string }> }) => {
      const target = document.querySelector(selector);
      if (!target) {
        console.error(`[drop] target not found: ${selector}`);
        return false;
      }
      
      const dataTransfer = new DataTransfer();
      for (const fd of filesData) {
        const bytes = new Uint8Array(fd.data);
        const file = new File([bytes], fd.name, { type: fd.mime });
        dataTransfer.items.add(file);
      }
      
      // Dispatch drag sequence
      target.dispatchEvent(new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer }));
      target.dispatchEvent(new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer }));
      target.dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer }));
      
      console.log(`[drop] dispatched drop event with ${filesData.length} files`);
      return true;
    },
    {
      selector: targetSelector,
      filesData: files.map(f => ({
        name: f.name,
        data: Array.from(f.data),
        mime: f.mime || 'application/octet-stream'
      }))
    }
  );
}
