// Whether the emulator answers.  Two ways it stops: a stall (its heartbeat
// stands still while requests are pending -- it may come back) and a crash
// (a wasm trap or an abort -- it does not).  Neither takes the page down:
// the banner (EmulatorHealthBanner) says what happened and the console
// stays open with the diagnostics the page dumped (bus/diagnostics.ts).

export const emulatorHealth = $state({
  stalled: false,
  stallSince: 0, // performance.now() when the heartbeat stopped (3 s before the stall was noticed)
  stallReason: '',
  stalls: 0, // stalls since the page loaded (the ones that ended included)
  crashed: false,
  crashReason: '',
});
