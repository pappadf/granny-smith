// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

declare module 'pngjs' {
  import { EventEmitter } from 'events';
  interface PNGOptions { width?: number; height?: number; filterType?: number; colorType?: number; inputColorType?: number; bitDepth?: number; inputHasAlpha?: boolean; }
  export class PNG extends EventEmitter {
    width: number; height: number; data: Buffer;
    constructor(options?: PNGOptions);
    static sync: { read(buffer: Buffer): PNG; write(png: PNG): Buffer };
    pack(): this; parse(data: Buffer, cb: (err: Error, data: PNG) => void): void;
  }
}
