export {
  initEmulator,
  shutdownEmulator,
  pauseEmulator,
  resumeEmulator,
  saveCheckpoint,
  bootstrap,
  isModuleReady,
  gsEval,
  gsEvalLine,
  getRuntimePrompt,
  shellInterrupt,
  tabComplete,
  getModule,
  type CompletionResult,
} from './emulator';
export { setTerminalSink, routePrintLine, routeLogEmit } from './logSink';
export { loadMachineRoots, loadMachineChildren, type MachineTreeNode } from './machineTree';
export {
  disasmAt,
  readRegisters,
  writeRegister,
  peekL,
  peekBytes,
  peekPhysBytes,
  listBreakpoints,
  addBreakpoint,
  removeBreakpoint,
  continueExec,
  pauseExec,
  stepInto,
  stepOver,
  stopMachine,
  restart,
  type DisasmRow,
  type Registers,
  type Breakpoint,
} from './debug';
export {
  opfs,
  setOpfsBackend,
  MockOpfs,
  BrowserOpfs,
  writeToOPFS,
  removeFromOPFS,
  type OpfsBackend,
} from './opfs';
export { acceptFiles, processDataTransfer } from './upload';
export { processUrlMedia, parseUrlMediaParams } from './urlMedia';
export { maybeOfferBackgroundCheckpoint, isResumePending, resolveResume } from './checkpoint';
export type {
  MachineConfig,
  RomInfo,
  OpfsEntry,
  RecentEntry,
  ImageCategory,
  CheckpointEntry,
} from './types';
