# Shell Command Framework

This document describes the shell command framework in `src/core/shell/`. The framework provides a unified command
interface that serves both interactive (terminal) and programmatic (JavaScript/WASM bridge) callers through a single
command implementation, with declarative argument specifications, typed results, captured I/O, tab completion, and
symbol resolution.

## Overview

The shell is the primary interface for controlling and inspecting the emulator. Users type commands interactively in the
xterm.js terminal, and the JavaScript frontend issues the same commands programmatically via the WASM bridge. The
framework ensures that a single command implementation serves both use cases without code duplication.

The framework addresses several problems with the earlier `(int argc, char *argv[]) -> uint64_t` command model:

- **Return value ambiguity** — the old `uint64_t` was overloaded as exit code, data payload, or error sentinel, with
  no way for the caller to distinguish which. The WASM transport further truncated it to `int32_t`.
- **No string returns** — commands that wanted to return structured data to JS had no channel other than stdout, making
  programmatic use fragile.
- **Redundant argument parsing** — every command manually parsed `argv[]`, repeating address parsing, subcommand
  dispatch, flag parsing, numeric conversion, and arity checks.
- **No machine-readable metadata** — commands declared only a freeform text synopsis, making auto-completion and
  validation impossible.
- **No output routing** — commands wrote directly to stdout/stderr with no way to capture output separately for
  programmatic callers.

## Source Files

| File | Purpose |
|------|---------|
| [cmd_types.h](../src/core/shell/cmd_types.h) | All type definitions: argument specs, parsed values, context, results, completion |
| [cmd_parse.c](../src/core/shell/cmd_parse.c) | Argument parser: tokenized argv to typed `arg_value[]` using `arg_spec` declarations |
| [cmd_symbol.c](../src/core/shell/cmd_symbol.c) | Unified symbol resolver for `$`-prefixed tokens (registers, Mac globals) |
| [cmd_io.c](../src/core/shell/cmd_io.c) | I/O stream setup for interactive vs. programmatic mode |
| [cmd_complete.c](../src/core/shell/cmd_complete.c) | Metadata-driven tab completion engine |
| [cmd_json.c](../src/core/shell/cmd_json.c) | JSON serializer for `cmd_result` (WASM bridge) |
| [shell.c](../src/core/shell/shell.c) | Command registry, dispatcher, tokenizer, built-in commands, initialization |
| [shell.h](../src/core/shell/shell.h) | Public API for registration, dispatch, and completion |
| [peeler_shell.c](../src/core/shell/peeler_shell.c) | Archive extraction commands (peeler library integration) |

## Architecture

### Command Lifecycle

1. **Input arrives** — either from the interactive terminal (via `handle_command()`) or from the JS bridge.
2. **Tokenization** — the input line is split into argv tokens. The tokenizer supports backslash escapes, single
   quotes, double quotes, and UTF-8 curly quotes.
3. **Command lookup** — the dispatcher searches the registry by command name, then by aliases (case-insensitive).
4. **Argument parsing** — for new-style commands, the framework parses argv against the command's declared `arg_spec`
   array, producing typed `arg_value` entries. For commands with subcommands, the first argument is matched against
   declared `subcmd_spec` entries (including subcommand aliases), then remaining arguments are parsed against that
   subcommand's arg specs.
5. **I/O setup** — the dispatcher creates a `cmd_io` context. In interactive mode, `ctx->out` and `ctx->err` point to
   stdout/stderr. In programmatic mode, they point to `fmemopen` capture buffers.
6. **Execution** — the command handler receives the parsed `cmd_context` and writes its `cmd_result`.
7. **I/O finalization** — for programmatic calls, capture buffers are flushed and attached to the result.
8. **JSON serialization** — for the WASM bridge, the result is serialized to JSON in a shared buffer.

### Dual Command Signatures

The framework supports two command handler signatures:

- **New-style** (`cmd_fn`): receives a `cmd_context` with pre-parsed, typed arguments and writes a `cmd_result` with
  typed return values. Commands use `cmd_printf(ctx, ...)` for output.
- **Simple/legacy** (`cmd_fn_simple`): the classic `(int argc, char *argv[]) -> uint64_t` signature. The dispatcher
  wraps the return value in `RES_INT` automatically.

Both signatures are registered through `cmd_reg` and coexist in the same registry. This allows incremental migration —
commands can be converted one at a time from simple to new-style without breaking anything.

## Argument Specification

Commands declare their expected arguments at registration time using `arg_spec` arrays. The framework parses and
validates arguments centrally before the command handler is invoked.

### Argument Types

| Type | Parsed as | Description |
|------|-----------|-------------|
| `ARG_STRING` | `const char *` | Passed through as-is |
| `ARG_INT` | `int64_t` | Decimal, `0x` hex, `$` hex, or `0d` explicit decimal |
| `ARG_ADDR` | `uint32_t` | Address: hex, `$register`, symbol, or `L:`/`P:` prefixed |
| `ARG_BOOL` | `int` (0/1) | Accepts `on`/`off`, `true`/`false`, `1`/`0` |
| `ARG_SYMBOL` | `resolved_symbol` | Full symbol resolution (name, address, value, size, kind) |
| `ARG_PATH` | `const char *` | Filesystem path (enables path completion) |
| `ARG_ENUM` | `const char *` | One of a fixed set of declared string values |
| `ARG_REST` | `const char *` | Gathers all remaining tokens |

Any type can be OR'd with `ARG_OPTIONAL` (`0x100`) to mark the argument as optional. The `ARG_BASE_TYPE()` macro
strips the optional flag, and `ARG_IS_OPTIONAL()` tests for it.

### Subcommands

Commands with multiple modes (like `break set`, `break del`, `break list`) declare a `subcmd_spec` array. Each
subcommand entry has:

- A name (e.g., `"del"`, `"clear"`, `"list"`)
- Optional aliases (e.g., `{"r", NULL}` so `info r` works for `info regs`)
- Its own `arg_spec` array and argument count
- A one-line description for help text

A subcommand with `name = NULL` acts as the default — it matches when no subcommand token is recognized. This allows
`break 0x400000` (default subcommand) alongside `break set 0x400000` (explicit subcommand).

### Parsed Argument Block

After parsing, the command handler receives a `cmd_context` containing:

- `subcmd` — the matched subcommand name (NULL if none or default)
- `args[]` — array of `arg_value` entries, each with a `type`, `present` flag, and a union holding the parsed value
- `nargs` — number of argument specs
- `raw_argc` / `raw_argv` — the original tokenized arguments, available as an escape hatch
- `out` / `err` — FILE* streams for command output

Each `arg_value` has a `present` field: 1 if the argument was provided, 0 if it was optional and omitted. The union
provides type-appropriate access: `as_int`, `as_addr`, `as_bool`, `as_str`, or `as_sym`.

## Symbol Resolution

The symbol resolver (`cmd_symbol.c`) provides uniform `$`-prefix expansion for all commands. Any argument token
starting with `$` is resolved before the command sees it.

### Resolution Priority

1. **CPU registers** — `$pc`, `$sp`, `$d0`–`$d7`, `$a0`–`$a7`, `$sr`, `$ssp`, `$usp`, `$msp`, `$ccr`, and
   individual condition code flags (`$c`, `$v`, `$z`, `$n`, `$x`, `$s`, `$t`)
2. **FPU registers** — `$fp0`–`$fp7`, `$fpcr`, `$fpsr`, `$fpiar`
3. **MMU registers** — `$tc`, `$crp`, `$srp`, `$tt0`, `$tt1`
4. **Mac low-memory globals** — 471 entries from `mac_globals_data.c` (e.g., `$MBState`, `$Ticks`, `$MemTop`)

### How Symbols Interact with Argument Types

The meaning of a resolved symbol depends on the argument type that receives it:

- **`ARG_ADDR`** — for registers, resolves to the register's *value* (i.e., the address the register points to). For
  Mac globals, resolves to the global's *address* in low memory. This is the "use as pointer" interpretation.
  Example: `disasm $pc 200` disassembles starting from the current PC value.
- **`ARG_SYMBOL`** — resolves to the full `resolved_symbol` struct, giving the command access to the canonical name,
  address, current value, data size, and kind (register vs. Mac global). Used by commands like `print` and `set` that
  need to know *what* they are operating on.
  Example: `print $MBState` shows the name, address, and current value.
- **`ARG_INT`** — for Mac globals, resolves to the current *value* stored at the global's address (reads memory).
  Allows using globals as numeric arguments anywhere.

The `ARG_SYMBOL` type also supports raw address access with size suffixes (e.g., `0400.w`, `0400.b`, `0400.l`) for
inspecting arbitrary memory locations with an explicit data size.

### Resolved Symbol Structure

Each resolved symbol contains:

- `name` — canonical name (e.g., `"PC"`, `"MBState"`)
- `address` — the symbol's address (0 for registers, low-memory address for globals)
- `value` — current value (register contents or memory read)
- `size` — data size in bytes (1, 2, 4, or 10 for FPU extended precision)
- `kind` — `SYM_REGISTER`, `SYM_MAC_GLOBAL`, or `SYM_UNKNOWN`

## I/O Streams and Output Capture

Commands write to `ctx->out` and `ctx->err` (via `cmd_printf` and `cmd_eprintf` macros) instead of directly to
stdout/stderr. The framework configures these streams based on the invocation mode:

### Invocation Modes

| Mode | `ctx->out` | `ctx->err` | Behavior |
|------|-----------|-----------|----------|
| `INVOKE_INTERACTIVE` | stdout | stderr | Output goes directly to the terminal. No capture overhead. |
| `INVOKE_PROGRAMMATIC` | fmemopen buffer | fmemopen buffer | All output captured into buffers. Nothing reaches the terminal. |
| `INVOKE_PIPE` | (reserved) | (reserved) | Future: output feeds into another command. |

### Capture Mechanism

For programmatic calls, the `cmd_io` structure owns the capture buffers:

- `out_buf` — 8 KB buffer for stdout capture
- `err_buf` — 2 KB buffer for stderr capture

These are opened as FILE* streams via `fmemopen()` with buffering disabled (`_IONBF`) for immediate writes. After the
command returns, `finalize_cmd_io()` flushes the streams, records the output lengths, null-terminates the buffers, and
attaches them to the `cmd_result`.

This separation means that when JavaScript calls `runCommand("info regs")`, the register dump appears in the result's
`output` field — it does not appear in the xterm.js terminal. When the user types the same command interactively,
output goes directly to the terminal as usual.

## Result Types

Commands return structured results via `cmd_result`, replacing the ambiguous `uint64_t` return value.

### Result Variants

| Type | Meaning | Convenience Macro |
|------|---------|-------------------|
| `RES_OK` | Success, no data payload | `cmd_ok(res)` |
| `RES_INT` | Integer data (e.g., register value, file size) | `cmd_int(res, value)` |
| `RES_STR` | String data | Set `res->type` and `res->as_str` |
| `RES_BOOL` | Boolean (e.g., running state) | `cmd_bool(res, value)` |
| `RES_ERR` | Error with message | `cmd_err(res, fmt, ...)` |

The `cmd_err` macro formats the error message into a 256-byte scratch buffer (`result_buf`) within the result struct,
avoiding dynamic allocation.

For legacy commands using `cmd_fn_simple`, the dispatcher wraps the `uint64_t` return value as `RES_INT` automatically.

## JSON Bridge

The JSON serializer (`cmd_json.c`) converts a `cmd_result` into a JSON object for consumption by the JavaScript
frontend via the WASM shared-heap buffer protocol.

### JSON Format

Successful result:
```json
{
    "status": "ok",
    "type": "int",
    "value": 4227090,
    "output": "PC = $00408012\n"
}
```

Error result:
```json
{
    "status": "error",
    "error": "invalid address: 0xZZZZ"
}
```

The `type` field reflects the result variant (`"int"`, `"str"`, `"bool"`, or `"ok"`). The `output` field contains
captured stdout text (present only for programmatic calls that produced output). The `stderr` field contains captured
stderr text (present only when non-empty).

### WASM Integration

The JSON result is written to a 16 KB static buffer (`g_cmd_json_buffer`) accessible via `get_cmd_json_result()`. The
JS side reads this buffer after command completion to obtain the structured result, replacing the previous protocol
where only a truncated integer was returned.

## Tab Completion

The completion engine (`cmd_complete.c`) provides metadata-driven tab completion using the declarative information in
command registrations.

### Automatic Completion

The following completions work automatically from command metadata, with no per-command code:

| Context | Completes with |
|---------|---------------|
| First token | All registered command names and aliases |
| Subcommand position | Declared subcommand names and their aliases |
| `ARG_ENUM` argument | Declared `enum_values` for that argument |
| `ARG_BOOL` argument | `on`, `off`, `true`, `false` |
| `ARG_PATH` argument | Filesystem directory listing |
| `ARG_SYMBOL` argument | Register names and all 471 Mac low-memory globals |
| `ARG_ADDR` with `$` prefix | Register names (triggered by `$` prefix) |

### Completion Algorithm

1. The input line is split into tokens up to the cursor position.
2. If the cursor is at the first token, command names and aliases are completed.
3. If a command is identified, its `cmd_reg` is looked up.
4. For commands with subcommands, if the cursor is at the subcommand position, subcommand names and aliases are
   offered. If a subcommand has already been typed, the engine switches to that subcommand's arg specs.
5. The argument position is determined and the corresponding `arg_spec` drives type-specific completion.
6. If the command has a custom `complete_fn`, it is also called to provide additional domain-specific suggestions
   (e.g., log category names).

### Custom Completers

Commands can optionally register a `complete_fn` callback for domain-specific completion that cannot be expressed
through `arg_spec` alone. The custom completer is called in addition to (not instead of) the automatic metadata-driven
completion.

## Command Registration

### New-Style Registration

Commands with declarative arguments use `register_command()` with a `cmd_reg` struct:

- `name` — primary command name
- `aliases` — NULL-terminated array of alias strings (e.g., `{"b", NULL}`)
- `category` — for help grouping (e.g., `"Execution"`, `"Breakpoints"`, `"Inspection"`)
- `synopsis` — one-line description
- `fn` — new-style handler (`cmd_fn`)
- `complete` — optional custom completer
- `args` / `nargs` — positional argument specs (for commands without subcommands)
- `subcmds` / `n_subcmds` — subcommand specs (for commands with subcommands)

Aliases are co-registered with the command — the dispatcher resolves aliases during lookup, and the help system lists
them automatically. Subcommand aliases work the same way via `subcmd_spec.aliases`.

### Legacy Registration

The `register_cmd()` convenience function accepts the classic `(name, category, synopsis, fn)` signature and internally
creates a `cmd_reg` with `simple_fn` set. This preserves backward compatibility for commands that have not yet been
migrated to the new framework.

### Command Registry

Commands are stored in a singly-linked list of `cmd_reg_node` entries, searched by name or alias
(case-insensitive). Commands can be registered and unregistered at runtime.

## Command Dispatch

### Entry Points

| Function | Use case |
|----------|----------|
| `handle_command()` | Platform layer entry point. Makes a mutable copy of the input, calls `shell_dispatch()`. |
| `shell_dispatch()` | Interactive dispatch. Returns an integer result for callers that don't need the full `cmd_result`. |
| `dispatch_command()` | Full dispatch with invocation mode and `cmd_result` output. |

### Dispatch Flow

The `execute_cmd()` function handles both command styles:

- **New-style commands** (`fn` set): initializes `cmd_io`, creates `cmd_context`, calls `cmd_parse_args()` to parse
  arguments against the declared specs, invokes the handler, then calls `finalize_cmd_io()` to attach captured output.
- **Simple commands** (`simple_fn` set): invokes the handler directly with `(argc, argv)` and wraps the `uint64_t`
  return in `RES_INT`.

When an unknown command is entered, the dispatcher computes Levenshtein edit distance against all registered command
names and aliases, suggesting the closest match if the distance is below a threshold of 4.

## Auto-Generated Help

The built-in `help` command generates documentation entirely from registration metadata:

- For commands without subcommands, it shows the command name, synopsis, argument names (with `<required>` and
  `[optional]` notation), and aliases.
- For commands with subcommands, it lists each subcommand with its arguments, description, and any subcommand aliases
  shown in parentheses.
- Help categories are displayed in a defined order (Execution, Breakpoints, Inspection, etc.), with any uncategorized
  commands appended at the end.

## Tokenizer

The shell tokenizer splits input lines into argv tokens with support for:

- **Backslash escapes** — `\` escapes the following character
- **Double quotes** — `"quoted string"` preserves spaces
- **Single quotes** — `'quoted string'` preserves spaces
- **UTF-8 curly quotes** — `\u201C`/`\u201D` (common when pasting from rich-text sources)

Maximum token count is 32. The tokenizer operates in-place on a mutable copy of the input line.

## Built-in Commands

The shell registers several built-in commands during `shell_init()`:

- **General** — `help`, `echo`, `add`, `remove` (dynamic command loading demo)
- **Filesystem** — `ls`, `cd`, `mkdir`, `mv`, `cat`, `exists`, `size`, `rm`

These use the simple registration path (`register_cmd()`). Other emulator subsystems (debug, scheduler, SCSI, etc.)
register their own commands from their respective init functions.

## Limits and Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `CMD_MAX_ARGS` | 8 | Maximum positional arguments per command/subcommand |
| `CMD_MAX_SUBCMDS` | 16 | Maximum subcommands per command |
| `CMD_OUT_BUF_SIZE` | 8192 | Stdout capture buffer size |
| `CMD_ERR_BUF_SIZE` | 2048 | Stderr capture buffer size |
| `CMD_RESULT_BUF_SIZE` | 256 | Result string scratch buffer |
| `CMD_MAX_COMPLETIONS` | 64 | Maximum completion suggestions |
| `CMD_JSON_BUF_SIZE` | 16384 | JSON result buffer for WASM bridge |
| `MAXTOK` | 32 | Maximum tokens per command line |

## Future: Pipes and Redirects

The `ctx->out` / `ctx->err` stream design enables a future shell pipeline:

```
disasm $pc 100 | grep JSR > tmp/calls.txt
```

Because commands already write to `ctx->out` instead of stdout, no command code changes would be needed to support
piping. The shell dispatcher would handle pipeline setup by chaining `cmd_io` instances. This is not yet implemented
but the I/O architecture supports it.
