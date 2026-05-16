// Static catalogue of shell commands for the Terminal view's Command
// Browser pane (spec §4.3.0). Lifted from local/gs-docs/prototypes/ui/app.js
// lines 47-136. The plan slates runtime augmentation via
// gsEval('shell.meta.methods') / gsEval('shell.aliases') for a later
// phase — this static set is the starting baseline.
//
// Each node has:
//   name      — displayed in the tree
//   desc      — multi-line plain-text description (rendered as a
//               blue-tinted block when the row is expanded)
//   insert    — text written into the terminal prompt on click; omit
//               for category-only rows
//   children  — nested commands / sub-commands (recursive)

export interface CommandNode {
  name: string;
  desc: string;
  insert?: string;
  children?: CommandNode[];
}

export const commandsTree: CommandNode[] = [
  {
    name: 'Scheduler',
    desc: 'Control when emulation runs and how strictly it simulates real hardware speeds.',
    children: [
      {
        name: 'run',
        insert: 'run',
        desc: 'Resume emulation. Optional argument stops after N instructions:\n  run [instructions]',
      },
      { name: 'stop', insert: 'stop', desc: 'Halt emulation immediately.' },
      {
        name: 'schedule',
        insert: 'schedule',
        desc: 'Set or show scheduler mode:\n  schedule strict|live|fast\n\n  strict — hardware-accurate timing (slowest, most faithful)\n  live   — real-time (default)\n  fast   — maximum host speed',
      },
      {
        name: 'status',
        insert: 'status',
        desc: 'Returns 1 if running, 0 if idle. Useful in scripts.',
      },
      {
        name: 'events',
        insert: 'events',
        desc: 'Show pending CPU event queue (timer interrupts, scheduled callbacks, …).',
      },
    ],
  },
  {
    name: 'Debugger',
    desc: 'Breakpoints, stepping, memory inspection, and disassembly. The Disassembly pane and the right-hand Sections pane (Registers, Memory, MMU, Breakpoints, …) are GUI surfaces for these same commands.',
    children: [
      {
        name: 'continue',
        insert: 'continue',
        desc: 'Resume execution until the next breakpoint or pause.',
      },
      { name: 'step', insert: 'step', desc: 'Execute exactly one instruction.' },
      {
        name: 'next',
        insert: 'next',
        desc: 'Step over JSR/BSR — execute one instruction, but treat subroutine calls as a single step (no descent).',
      },
      { name: 'finish', insert: 'finish', desc: 'Run until the current subroutine returns (RTS).' },
      {
        name: 'until',
        insert: 'until',
        desc: 'Run until the PC reaches a given address:\n  until <addr>',
      },
      {
        name: 'advance',
        insert: 'advance',
        desc: 'Run forward to a specific PC, then stop. Like a one-shot temporary breakpoint.',
      },
      {
        name: 'break',
        insert: 'break',
        desc: 'Set a PC breakpoint:\n  break <addr> [if <cond>]\n\nConditional breakpoints suspend execution only when <cond> evaluates true.',
      },
      {
        name: 'tbreak',
        insert: 'tbreak',
        desc: 'Like `break`, but the breakpoint auto-deletes after the first hit.',
      },
      {
        name: 'watch',
        insert: 'watch',
        desc: 'Set a memory watchpoint:\n  watch <addr> [r|w|rw]\n\nFires on read, write, or both. See the Watchpoints section in the Debug pane.',
      },
      {
        name: 'logpoint',
        insert: 'logpoint',
        desc: 'Set a logpoint — a non-stopping breakpoint that emits a log message when hit. Useful for tracing without halting.',
      },
      {
        name: 'examine',
        insert: 'examine',
        desc: 'Examine memory at a logical address:\n  examine <addr> [count]\n\nAlias: x. Goes through the MMU. To bypass it, use `info phys-bytes` instead.',
      },
      {
        name: 'disasm',
        insert: 'disasm',
        desc: 'Disassemble at an address:\n  disasm <addr> [count]\n\nAliases: dis, d.',
      },
      {
        name: 'translate',
        insert: 'translate',
        desc: 'Translate a logical address to physical:\n  translate <addr>\n\nOutput format: L:$<hex> -> P:$<hex> (kind, flags). Same call the disasm and Memory views use behind the scenes.',
      },
      {
        name: 'set',
        insert: 'set',
        desc: 'Set a register or shell variable:\n  set <name> <value>\n\nWorks for CPU registers (D0..D7, A0..A7, PC, SR), MMU registers, and user-defined shell variables.',
      },
      {
        name: 'print',
        insert: 'print',
        desc: 'Evaluate and print an expression. Mostly used for inspecting variables and computed addresses.',
      },
      {
        name: 'find',
        insert: 'find',
        desc: 'Search memory for a byte pattern:\n  find <start> <end> <bytes...>',
      },
      {
        name: 'info',
        insert: 'info',
        desc: 'Multi-purpose introspection. Prints subsystem state — registers, MMU tables, exceptions, breakpoints, processes, etc. Use one of the subcommands below.',
        children: [
          {
            name: 'info mmu',
            insert: 'info mmu',
            desc: 'Dump MMU registers: TC, CRP, SRP, TT0, TT1.',
          },
          {
            name: 'info mmu-walk',
            insert: 'info mmu-walk',
            desc: 'Walk the page tables for a logical address:\n  info mmu-walk <logical>\n\nUnder TC.SRE=1, walks both supervisor (SRP) and user (CRP) trees.',
          },
          {
            name: 'info mmu-map',
            insert: 'info mmu-map',
            desc: 'List contiguous mapped logical ranges:\n  info mmu-map [<start> [<end>]]\n\nDefault range is $00000000-$1FFFFFFF.',
          },
          {
            name: 'info mmu-descriptors',
            insert: 'info mmu-descriptors',
            desc: 'Decode raw memory at <address> as PMMU descriptors:\n  info mmu-descriptors <address> [count]',
          },
          {
            name: 'info phys-bytes',
            insert: 'info phys-bytes',
            desc: 'Dump bytes from a physical address (bypasses the MMU):\n  info phys-bytes <addr> [count]',
          },
          {
            name: 'info exceptions',
            insert: 'info exceptions',
            desc: 'Dump the exception trace ring (bus errors, address errors, illegal instructions).',
          },
          {
            name: 'info break',
            insert: 'info break',
            desc: 'List all active breakpoints with their addresses and conditions.',
          },
          { name: 'info logpoint', insert: 'info logpoint', desc: 'List all active logpoints.' },
          {
            name: 'info process',
            insert: 'info process',
            desc: 'Print the kernel process table — PIDs, names, states.',
          },
          {
            name: 'info events',
            insert: 'info events',
            desc: 'Show the pending CPU event queue (alias for top-level `events`).',
          },
        ],
      },
      {
        name: 'screenshot',
        insert: 'screenshot',
        desc: 'Capture the emulator screen to PNG:\n  screenshot [<path>]',
      },
    ],
  },
  {
    name: 'Logging',
    desc: 'Per-category log output. Every module (cpu, scsi, floppy, mmu, …) has its own log category with an integer level (0 = off, higher = more verbose).',
    children: [
      {
        name: 'log',
        insert: 'log',
        desc: 'Set or show log levels:\n  log\n  log <cat>\n  log <cat> <level>\n  log <cat> level=N stdout=on|off file=PATH|off ts=on|off pc=on|off\n\nWith no arguments, lists all registered categories and their current level. The Logs panel view exposes the same controls graphically.',
      },
    ],
  },
  {
    name: 'Storage',
    desc: 'Disk image / ROM management. Mount, unmount, persist images, and download files between OPFS and the host.',
    children: [
      {
        name: 'rom',
        insert: 'rom',
        desc: 'ROM image management:\n  rom list\n  rom load <path>\n  rom checksum <path>',
      },
      {
        name: 'fd',
        insert: 'fd',
        desc: 'Floppy drive control:\n  fd insert <path> <drive> [boot]\n  fd eject <drive>\n  fd list',
      },
      {
        name: 'image',
        insert: 'image',
        desc: 'Generic disk image management — mount/unmount, list, etc.',
      },
      {
        name: 'checkpoint',
        insert: 'checkpoint',
        desc: 'Save or load full machine state:\n  checkpoint --save <path>\n  checkpoint --load <path>\n\nThe Display Toolbar Save State button is a one-click wrapper.',
      },
      {
        name: 'download',
        insert: 'download',
        desc: 'Download a file from OPFS to the host disk:\n  download <opfs-path>',
      },
    ],
  },
  {
    name: 'Mac',
    desc: 'Macintosh-specific commands — process info, mouse/keyboard injection, RTC.',
    children: [
      {
        name: 'pi',
        insert: 'pi',
        desc: "Print process information from the kernel's process table.",
      },
      {
        name: 'set-mouse',
        insert: 'set-mouse',
        desc: 'Set or move the mouse cursor:\n  set-mouse [--global|--hw|--aux] <x> <y>',
      },
      {
        name: 'mouse-button',
        insert: 'mouse-button',
        desc: 'Inject a mouse-button event:\n  mouse-button [--global|--hw] up|down',
      },
      { name: 'set-time', insert: 'set-time', desc: 'Set the emulated RTC clock.' },
    ],
  },
  {
    name: 'General',
    desc: 'Built-in shell commands — help, echo, etc.',
    children: [
      {
        name: 'help',
        insert: 'help',
        desc: 'Show help for a command:\n  help [<cmd>]\n\nWith no argument, lists all registered commands grouped by category.',
      },
      { name: 'echo', insert: 'echo', desc: 'Print arguments to stdout. Useful inside scripts.' },
      { name: 'time', insert: 'time', desc: 'Show the wall clock.' },
    ],
  },
];
