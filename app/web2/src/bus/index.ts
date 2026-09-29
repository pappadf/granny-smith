export { initEmulator } from './boot';
export {
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
  needsContinuation,
  getModule,
  type CompletionResult,
  type CompletionCandidate,
} from './emulator';
export { setConsoleSink, routeConsole, routePrintLine, routeLogEmit } from './logSink';
export { loadMembers, type MemberInfo, type TypeDescriptor, type ArgInfo } from './systemTree';
export {
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
  type Registers,
  type Breakpoint,
} from './debug';
export {
  opfs,
  setOpfsBackend,
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
export type { MachineConfig, RomInfo, OpfsEntry, ImageCategory, CheckpointEntry } from './types';
