export {
  initEmulator,
  shutdownEmulator,
  pauseEmulator,
  resumeEmulator,
  applySchedulerMode,
  bootstrap,
  isModuleReady,
  whenModuleReady,
  gsEval,
  gsOk,
  isGsError,
  gsErrorText,
  type GsError,
  gsEvalLine,
  getRuntimePrompt,
  seedPrompt,
  shellInterrupt,
  tabComplete,
  getModule,
  type CompletionResult,
} from './emulator';
export { setTerminalSink, routePrintLine, routeLogEmit } from './logSink';
export {
  loadSystemRoots,
  loadSystemChildren,
  loadNodeMethods,
  type SystemTreeNode,
  type MethodInfo,
} from './systemTree';
export {
  disasmAt,
  writeRegister,
  peekL,
  peekBytes,
  listBreakpoints,
  addBreakpoint,
  removeBreakpoint,
  removeBreakpointAt,
  continueExec,
  pauseExec,
  stepInto,
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
export {
  maybeOfferBackgroundCheckpoint,
  isResumePending,
  resolveResume,
  saveCheckpoint,
  type SaveCheckpointResult,
} from './checkpoint';
export type {
  MachineConfig,
  RomInfo,
  OpfsEntry,
  RecentEntry,
  ImageCategory,
  CheckpointEntry,
} from './types';
