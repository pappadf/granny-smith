// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// syntax.h
// The command line's lexical layer, shared by the highlighter (highlight.c)
// and tab completion (cmd_complete.c): what shape a command line has (a
// command with argument-mode words, or `PATH = VALUE`), where a path or an
// argument word ends, and which declared argument a word fills -- the
// grammar gs.shell_line (gs_lua_lib.lua) runs a command line by.  Lua lines
// are not read here.  Nothing here touches the object tree; all scanning is
// bounded by an explicit end pointer.

#ifndef GS_SHELL_SYNTAX_H
#define GS_SHELL_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

// The identifier characters of the shell grammar: ASCII letters, digits and
// `_` (never the locale's letters), the same in every scanner.
static inline bool script_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool script_ident_char(char c) {
    return script_ident_start(c) || (c >= '0' && c <= '9');
}

// Quote state for the quote-aware scans (brackets, comments): a backslash
// escapes the next byte inside and outside quotes.
typedef struct {
    char quote; // 0, '"' or '\''
    bool esc;
} script_quote_t;

// Advance the quote state over c.
static inline void script_quote_step(script_quote_t *q, char c) {
    if (q->esc) {
        q->esc = false;
        return;
    }
    if (c == '\\') {
        q->esc = true;
        return;
    }
    if (q->quote) {
        if (c == q->quote)
            q->quote = 0;
        return;
    }
    if (c == '"' || c == '\'')
        q->quote = c;
}

// The first unquoted `c` in [p, end), or NULL.
const char *script_find_unquoted(const char *p, const char *end, char c);

// The shape of a command line.
typedef enum {
    SCRIPT_STMT_EMPTY = 0, // blank
    SCRIPT_STMT_INVALID, // an unbalanced `[` in the head
    SCRIPT_STMT_ASSIGN, // PATH = VALUE, $NAME… = VALUE
    SCRIPT_STMT_COMMAND, // PATH ARG… (argument mode; no arguments is a bare read or call)
    SCRIPT_STMT_EXPR, // anything else (`PATH(`: a Lua call)
} script_stmt_kind_t;

// A classified command line.  Pointers are into the classified text; a span
// not present is NULL.
typedef struct {
    script_stmt_kind_t kind;
    const char *start; // first byte, leading blanks skipped
    const char *head, *head_end; // the leading path token
    const char *eq; // the `=` of an assignment
    const char *rest; // what follows the head or the `=`, leading blanks skipped
    const char *end; // end of the line, trailing blanks dropped
} script_stmt_t;

// Classify the command line in [text, end), without its comment.
void script_classify(const char *text, const char *end, script_stmt_t *out);

// The end of a path token at p: `$`? IDENT ('.' IDENT | '[' … ']')*, with
// bracket contents skipped quote-aware.  p when p does not start a path;
// NULL when a `[` is not closed before `end`.
const char *script_path_end(const char *p, const char *end);

// The `]` matching the `[` at `open` (nested brackets and quoted strings
// skipped), or NULL when it is not closed before `end`.
const char *script_bracket_close(const char *open, const char *end);

// The end of one argument-mode word at p: an optional `name=`, then a quoted
// string (to its closing quote), a parenthesised expression, a `$` path, or
// a bare word (to whitespace, `\` escaping the next byte).  `open`
// (optional) is set when a quote or parenthesis is still open at `end`.
const char *script_arg_end(const char *p, const char *end, bool *open);

// Where the command line [line, end)'s comment starts, or NULL: a `#` or
// `--` at the start or after a blank, outside quotes -- the rule
// gs.shell_line strips it by.
const char *script_comment_start(const char *line, const char *end);

// A command line split into its statement and its comment.
typedef struct {
    script_stmt_t stmt;
    const char *comment; // start of the comment, NULL when none
} script_line_t;

// Split the line [line, end) (no newline inside).
void script_line_split(const char *line, const char *end, script_line_t *out);

// The declared argument slot of method `m` a word fills: the argument named
// `name` (`name_len` bytes) when a name is given, else positional slot `pos`
// (a rest argument absorbs every slot past it).  -1 when none.
int script_arg_slot(const member_t *m, int pos, const char *name, size_t name_len);

#endif // GS_SHELL_SYNTAX_H
