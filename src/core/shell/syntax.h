// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// syntax.h
// The shell grammar's lexical layer, shared by the parser (script.c), the
// highlighter (highlight.c) and tab completion (cmd_complete.c): what kind
// of statement a text is, where a path or an argument word ends, how a line
// splits into a block's parts, and which declared argument a word fills.
// The parser builds its statement tree from these answers, so the two
// console views read a line the way it runs.  Nothing here touches the
// object tree; all scanning is bounded by an explicit end pointer.

#ifndef GS_SHELL_SYNTAX_H
#define GS_SHELL_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

// What a statement is, decided by its first word(s).
typedef enum {
    SCRIPT_STMT_EMPTY = 0, // blank
    SCRIPT_STMT_INVALID, // a shape the parser rejects (`error` says why)
    SCRIPT_STMT_LET, // let NAME = EXPR
    SCRIPT_STMT_ALIAS, // alias NAME = PATH
    SCRIPT_STMT_COMMAND_DEF, // command NAME = PATH
    SCRIPT_STMT_ASSIGN, // PATH = EXPR, $NAME… = EXPR
    SCRIPT_STMT_COMMAND, // PATH ARG… (argument mode; no arguments is a bare read or call)
    SCRIPT_STMT_EXPR, // an expression statement
    SCRIPT_STMT_IF,
    SCRIPT_STMT_ELIF,
    SCRIPT_STMT_ELSE,
    SCRIPT_STMT_WHILE,
    SCRIPT_STMT_FOR,
    SCRIPT_STMT_DEF,
    SCRIPT_STMT_BREAK,
    SCRIPT_STMT_CONTINUE,
    SCRIPT_STMT_RETURN,
    SCRIPT_STMT_ASSERT,
    SCRIPT_STMT_INCLUDE,
} script_stmt_kind_t;

// A classified statement.  Pointers are into the classified text; a span
// not present is NULL.
typedef struct {
    script_stmt_kind_t kind;
    const char *start; // first byte of the statement, leading blanks skipped
    const char *head, *head_end; // the keyword, or the leading path token
    const char *name, *name_end; // the declared name (let, alias, command, for, def)
    const char *eq; // the `=` of a declaration or assignment
    const char *rest; // what follows the head or the `=`, leading blanks skipped
    const char *end; // end of the statement, trailing blanks dropped
    const char *error; // SCRIPT_STMT_INVALID: the parser's message
} script_stmt_t;

// Classify the statement in [text, end): one statement, without a comment
// and without a block's braces (see script_line_split).  Block forms (if,
// elif, else, while, for, def) are recognised by their keyword; their
// headers are checked by the parser.
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

// The first unquoted `#` in [line, end) (a comment), or NULL.
const char *script_comment_start(const char *line, const char *end);

// One source line split the way the parser reads it: an optional leading
// `}` (closing a multi-line block), a statement, and for a block form its
// `{` and, on the same line, an inline block's single statement and its
// closing `}`.  Pointers are into the line; a part not present is NULL.
typedef struct {
    const char *closer; // leading `}`
    script_stmt_t stmt; // the statement, or a block form's header
    const char *open; // the `{` of a block form
    script_stmt_t body; // an inline block's statement (EMPTY when none)
    const char *close; // the `}` ending an inline block
    const char *comment; // `#` starting a comment
} script_line_t;

// Split the line [line, end) (no newline inside).
void script_line_split(const char *line, const char *end, script_line_t *out);

// The declared argument slot of method `m` a word fills: the argument named
// `name` (`name_len` bytes) when a name is given, else positional slot `pos`
// (a rest argument absorbs every slot past it).  -1 when none.
int script_arg_slot(const member_t *m, int pos, const char *name, size_t name_len);

#endif // GS_SHELL_SYNTAX_H
