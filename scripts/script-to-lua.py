#!/usr/bin/env python3
"""script-to-lua.py — convert a shell-v2 `.script` file to Lua 5.4.

Usage:
    script-to-lua.py IN.script [-o OUT.lua] [--lib PATH] [--report]
                               [--module | --no-module]

The old language is documented in docs/internals/core/shell/shell.md; the
target is the Lua bridge (docs/internals/core/lua/lua.md): root children are
globals, `gs.*` helpers, `F"..."` templates, `echo`, `quit`, `vars.NAME`, and
the ported shared library `tests/integration/lib/mac.lua`, reached with
`local mac = dofile(...)` wherever the script included `lib/mac.script`;
the library's functions and run-mode bindings get the `mac.` prefix, and the
`if row_on(L) { row_end(L, try(row(), "FAILED")) }` idiom becomes
`mac.row(L, row)`.

A file that other scripts include (auto-detected: not named test.script and
no top-level `quit`; override with --module/--no-module) is a "module": its
top-level defs and lets are emitted as globals, since `include` shared one
function registry and binding scope in the old language.  An including file
likewise emits as globals the top-level names its included modules use.

Whatever cannot be mapped confidently is emitted with a `-- PORT: <reason>`
marker (and the original line as a comment when it could not be parsed) so a
human can finish it; nothing is dropped silently.  --report lists the markers
and the notable-but-confident rewrites (`/` -> `//`, string `+` -> `..`,
templates spelled with `..`) on stderr.
"""

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------

LUA_KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
}

# Root children of the object tree (globals in Lua) and the root builtins.
ROOTS = {
    "machine", "scheduler", "debug", "files", "shell", "log", "checkpoint",
    "catalog", "appletalk", "pacing", "predecode", "jit",
    "meta", "methods", "objects", "attributes", "help", "time",
}
HEX_NAME_RE = re.compile(r"^[0-9A-F]+$")
UPPER_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")

# Built-in command words -> the method they run.
BUILTIN_COMMANDS = {
    "ls": "files.ls", "pwd": "files.pwd", "cat": "files.cat", "cp": "files.cp",
    "mv": "files.mv", "rm": "files.rm", "mkdir": "files.mkdir", "cd": "files.cd",
    "run": "scheduler.run", "stop": "scheduler.stop", "reset": "machine.reset",
    "step": "debug.step", "disasm": "debug.disasm",
}

# Built-in register aliases ($pc, $d0, ...) -> machine.cpu.<name>.
REG_ALIASES = {"pc", "sp", "sr", "usp", "ssp", "ccr", "msr"} | \
    {"d%d" % i for i in range(8)} | {"a%d" % i for i in range(8)}

# Last path segments that are methods when they appear as a bare statement
# (`machine.restart`).  Any other bare path is an attribute/node read.
METHOD_VERBS = {
    "add", "attach", "attach_cdrom", "attach_hd", "attach_media", "boot",
    "browse", "cat", "cd", "checksum", "clear", "click", "complete",
    "convert", "cp", "create", "default_config", "descriptor", "disasm",
    "down", "download", "eject", "eject_media", "eval", "expand", "export",
    "export_raw", "extract", "fd_create", "feed", "find", "find_media",
    "frame", "gamma", "hd_create", "highlight", "identify", "identify_cdrom",
    "identify_hd", "import", "indices", "insert", "interrupt", "list",
    "load", "ls", "map", "match", "matches", "members", "mkdir", "move",
    "mute", "mv", "needs_continuation", "offer", "partmap", "path_compare",
    "path_exists", "path_size", "peek", "poke", "pram_init", "press",
    "probe", "profile", "pwd", "raw", "read", "receive", "register",
    "remove", "reset", "reset_button", "restart", "restore", "rewind", "rm",
    "run", "save", "send", "sent", "set", "snapshot", "start", "stop", "str",
    "tex_offset", "trace", "translate", "type", "udif_info", "unmount", "up",
    "usage", "validate", "verify", "walk", "quit", "script_run", "info", "off", "pattern",
}

# Methods whose named argument is an OBJ_ARG_TEMPLATE (fire-time template
# evaluated by the old expression engine): (path suffix, argument name).
TEMPLATE_ARGS = {("logpoints.add", "message")}

MAC_LET_NAMES_FALLBACK = {
    "ROW_FILTER", "ROW_SET", "REGEN_MODE", "KEEP_GOING_MODE", "RESULTS_DIR",
    "FIXTURE_DIR", "RUN_TICKS_BUDGET", "rows_run", "rows_failed",
    "failed_labels",
}
# --var names a suite may pre-set before including the library.
VARS_PRESET_NAMES = {"ROW_SET", "ROW", "REGEN", "KEEP_GOING"}

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INT_RE = re.compile(r"^-?(0[xX][0-9A-Fa-f_]+|0[bB][01_]+|[0-9][0-9_]*)(:[0-9]+)?$")
FLOAT_RE = re.compile(r"^-?[0-9][0-9_]*\.[0-9]+$")


class ConvError(Exception):
    pass


# ---------------------------------------------------------------------------
# Quote-aware scanning helpers
# ---------------------------------------------------------------------------

def scan_dq(s, i):
    """s[i] == '"'. Return the index just past the closing quote, honouring
    escapes and nested quotes inside ${...} splices."""
    n = len(s)
    j = i + 1
    while j < n:
        c = s[j]
        if c == "\\":
            j += 2
            continue
        if c == '"':
            return j + 1
        if c == "$" and j + 1 < n and s[j + 1] == "{":
            j = scan_interp(s, j + 1)
            continue
        j += 1
    raise ConvError("unterminated string")


def scan_sq(s, i):
    n = len(s)
    j = i + 1
    while j < n:
        c = s[j]
        if c == "\\" and j + 1 < n and s[j + 1] == "'":
            j += 2
            continue
        if c == "'":
            return j + 1
        j += 1
    raise ConvError("unterminated raw string")


def scan_interp(s, i):
    """s[i] == '{' of a ${...}. Return index just past the matching '}'."""
    n = len(s)
    depth = 0
    j = i
    while j < n:
        c = s[j]
        if c == '"':
            j = scan_dq(s, j)
            continue
        if c == "'":
            j = scan_sq(s, j)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise ConvError("unterminated ${...}")


def scan_bracket(s, i, open_c, close_c):
    """s[i] == open_c. Return index just past the matching close_c."""
    n = len(s)
    depth = 0
    j = i
    while j < n:
        c = s[j]
        if c == '"':
            j = scan_dq(s, j)
            continue
        if c == "'":
            j = scan_sq(s, j)
            continue
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise ConvError("unbalanced %s" % open_c)


def split_comment(line):
    """Split a source line into (code, comment-or-None), quote-aware."""
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '"':
            try:
                i = scan_dq(line, i)
            except ConvError:
                return line, None
            continue
        if c == "'":
            try:
                i = scan_sq(line, i)
            except ConvError:
                return line, None
            continue
        if c == "#":
            return line[:i].rstrip(), line[i + 1:]
        i += 1
    return line.rstrip(), None


# ---------------------------------------------------------------------------
# Expression lexer
# ---------------------------------------------------------------------------

OPS3 = ("<<=", ">>=")
OPS2 = ("..", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>")
OPS1 = "+-*/%&|^~!?:()[],.=<>"


class Tok:
    __slots__ = ("kind", "val", "pos")

    def __init__(self, kind, val, pos):
        self.kind = kind
        self.val = val
        self.pos = pos

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.val)


def lex_expr(s):
    toks = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if c == '"':
            j = scan_dq(s, i)
            toks.append(Tok("STR", ('"', s[i + 1:j - 1]), i))
            i = j
            continue
        if c == "'":
            j = scan_sq(s, i)
            toks.append(Tok("STR", ("'", s[i + 1:j - 1]), i))
            i = j
            continue
        if c == "$":
            m = IDENT_RE.match(s, i + 1)
            if not m:
                raise ConvError("bad $binding at %d" % i)
            toks.append(Tok("VAR", m.group(0), i))
            i = m.end()
            continue
        if c.isdigit():
            m = re.compile(r"0[xX][0-9A-Fa-f_]+|0[bB][01_]+|[0-9][0-9_]*").match(s, i)
            text = m.group(0)
            j = m.end()
            is_float = False
            if j < n and s[j] == "." and j + 1 < n and s[j + 1].isdigit():
                k = j + 1
                while k < n and s[k].isdigit():
                    k += 1
                text = s[i:k]
                j = k
                is_float = True
            width = None
            if not is_float and j < n and s[j] == ":" and j + 1 < n and s[j + 1].isdigit():
                k = j + 1
                while k < n and s[k].isdigit():
                    k += 1
                width = int(s[j + 1:k])
                j = k
            toks.append(Tok("NUM", (text, width, is_float), i))
            i = j
            continue
        if c.isalpha() or c == "_":
            m = IDENT_RE.match(s, i)
            toks.append(Tok("ID", m.group(0), i))
            i = m.end()
            continue
        if s.startswith(OPS2, i):
            toks.append(Tok("OP", s[i:i + 2], i))
            i += 2
            continue
        if c in OPS1:
            toks.append(Tok("OP", c, i))
            i += 1
            continue
        raise ConvError("unexpected character %r" % c)
    toks.append(Tok("EOF", None, n))
    return toks


# ---------------------------------------------------------------------------
# Expression parser -> AST
#
# Nodes (tuples):
#   ('num', text, width, is_float)
#   ('str', quote, raw)
#   ('bare', text)                     argument-mode bare word (a string)
#   ('var', name)                      $name
#   ('id', name)                       path head / function name
#   ('const', 'true'|'false'|'none')
#   ('member', obj, name)              obj.name  (name may be digits)
#   ('index', obj, expr)
#   ('call', fn, args, named)          named: list of (name, expr)
#   ('unary', op, x)
#   ('binary', op, a, b)
#   ('ternary', c, a, b)
#   ('range', a, b)
#   ('list', items)
#   ('paren', x)
# ---------------------------------------------------------------------------

BIN_PREC = {
    "||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, "<=": 4, ">": 4, ">=": 4,
    "..": 5, "|": 6, "^": 7, "&": 8, "<<": 9, ">>": 9, "+": 10, "-": 10,
    "*": 11, "/": 11, "%": 11,
}


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    def peek(self, k=0):
        return self.toks[self.i + k]

    def next(self):
        t = self.toks[self.i]
        self.i += 1
        return t

    def at_op(self, *ops):
        t = self.peek()
        return t.kind == "OP" and t.val in ops

    def expect_op(self, op):
        t = self.next()
        if not (t.kind == "OP" and t.val == op):
            raise ConvError("expected %r, got %r" % (op, t.val))
        return t

    def parse_expr(self):
        return self.parse_ternary()

    def parse_ternary(self):
        c = self.parse_binary(1)
        if self.at_op("?"):
            self.next()
            a = self.parse_ternary()
            self.expect_op(":")
            b = self.parse_ternary()
            return ("ternary", c, a, b)
        return c

    def parse_binary(self, min_prec):
        left = self.parse_unary()
        while True:
            t = self.peek()
            if t.kind != "OP" or t.val not in BIN_PREC:
                break
            prec = BIN_PREC[t.val]
            if prec < min_prec:
                break
            self.next()
            right = self.parse_binary(prec + 1)
            if t.val == "..":
                left = ("range", left, right)
            else:
                left = ("binary", t.val, left, right)
        return left

    def parse_unary(self):
        if self.at_op("!", "~", "-", "+"):
            op = self.next().val
            x = self.parse_unary()
            return ("unary", op, x)
        return self.parse_postfix(self.parse_primary())

    def parse_postfix(self, node):
        while True:
            t = self.peek()
            if t.kind == "OP" and t.val == ".":
                nt = self.peek(1)
                if nt.kind == "ID":
                    self.next(); self.next()
                    node = ("member", node, nt.val)
                    continue
                if nt.kind == "NUM" and not nt.val[2] and nt.val[1] is None:
                    self.next(); self.next()
                    node = ("member", node, nt.val[0])
                    continue
                break
            if t.kind == "OP" and t.val == "[":
                self.next()
                idx = self.parse_expr()
                self.expect_op("]")
                node = ("index", node, idx)
                continue
            if t.kind == "OP" and t.val == "(":
                self.next()
                args, named = self.parse_args()
                node = ("call", node, args, named)
                continue
            break
        return node

    def parse_args(self):
        args = []
        named = []
        if self.at_op(")"):
            self.next()
            return args, named
        while True:
            t = self.peek()
            if t.kind == "ID" and self.peek(1).kind == "OP" and self.peek(1).val == "=":
                self.next(); self.next()
                named.append((t.val, self.parse_expr()))
            else:
                if named:
                    raise ConvError("positional argument after a named one")
                args.append(self.parse_expr())
            if self.at_op(","):
                self.next()
                if self.at_op(")"):
                    self.next()
                    break
                continue
            self.expect_op(")")
            break
        return args, named

    def parse_primary(self):
        t = self.next()
        if t.kind == "NUM":
            return ("num", t.val[0], t.val[1], t.val[2])
        if t.kind == "STR":
            return ("str", t.val[0], t.val[1])
        if t.kind == "VAR":
            return ("var", t.val)
        if t.kind == "ID":
            if t.val in ("true", "false", "none"):
                return ("const", t.val)
            return ("id", t.val)
        if t.kind == "OP" and t.val == "(":
            x = self.parse_expr()
            self.expect_op(")")
            return ("paren", x)
        if t.kind == "OP" and t.val == "[":
            items = []
            if self.at_op("]"):
                self.next()
                return ("list", items)
            while True:
                items.append(self.parse_expr())
                if self.at_op(","):
                    self.next()
                    if self.at_op("]"):
                        self.next()
                        break
                    continue
                self.expect_op("]")
                break
            return ("list", items)
        raise ConvError("unexpected token %r" % (t.val,))


def parse_expression(text):
    p = Parser(lex_expr(text))
    node = p.parse_expr()
    if p.peek().kind != "EOF":
        raise ConvError("trailing input after expression: %r" % text[p.peek().pos:])
    return node


# ---------------------------------------------------------------------------
# Argument-mode tokenizer
# ---------------------------------------------------------------------------

def tokenize_args(s):
    """Return a list of (name_or_None, node)."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        if s[i].isspace():
            i += 1
            continue
        name = None
        m = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(?!=)").match(s, i)
        if m:
            name = m.group(1)
            i = m.end()
        i, node = read_arg_value(s, i)
        out.append((name, node))
    return out


def read_arg_value(s, i):
    n = len(s)
    c = s[i] if i < n else ""
    if c == '"':
        j = scan_dq(s, i)
        return j, ("str", '"', s[i + 1:j - 1])
    if c == "'":
        j = scan_sq(s, i)
        return j, ("str", "'", s[i + 1:j - 1])
    if c == "(":
        j = scan_bracket(s, i, "(", ")")
        return j, ("paren", parse_expression(s[i + 1:j - 1]))
    if c == "$":
        j = i + 1
        while j < n and not s[j].isspace():
            if s[j] == "[":
                j = scan_bracket(s, j, "[", "]")
                continue
            if s[j] == "(":
                j = scan_bracket(s, j, "(", ")")
                continue
            if s[j] == '"':
                j = scan_dq(s, j)
                continue
            j += 1
        return j, parse_expression(s[i:j])
    # bare word (backslash escapes a character)
    j = i
    word = []
    while j < n and not s[j].isspace():
        if s[j] == "\\" and j + 1 < n:
            word.append(s[j + 1])
            j += 2
            continue
        word.append(s[j])
        j += 1
    w = "".join(word)
    if w in ("true", "false", "none"):
        return j, ("const", w)
    if INT_RE.match(w) or FLOAT_RE.match(w):
        try:
            return j, parse_expression(w)
        except ConvError:
            pass
    return j, ("bare", w)


# ---------------------------------------------------------------------------
# Statement parsing -> list of statement dicts
# ---------------------------------------------------------------------------

def scan_head(code):
    """Scan a path head ($name or ident with .seg/[idx] continuations) at the
    start of code. Return the end index (0 when none)."""
    n = len(code)
    i = 0
    if i < n and code[i] == "$":
        i += 1
    m = IDENT_RE.match(code, i)
    if not m:
        return 0
    i = m.end()
    while i < n:
        if code[i] == "." and i + 1 < n and (code[i + 1].isalpha() or code[i + 1] == "_" or code[i + 1].isdigit()):
            m = re.compile(r"[A-Za-z0-9_]+").match(code, i + 1)
            i = m.end()
            continue
        if code[i] == "[":
            i = scan_bracket(code, i, "[", "]")
            continue
        break
    return i


class StmtParser:
    def __init__(self, lines, filename):
        self.lines = lines
        self.filename = filename
        self.i = 0

    def error_stmt(self, lineno, indent, text, reason, comment=None):
        return {"kind": "raw", "line": lineno, "indent": indent, "text": text,
                "reason": reason, "comment": comment}

    def parse_block(self, until_close):
        stmts = []
        while self.i < len(self.lines):
            raw = self.lines[self.i]
            lineno = self.i + 1
            stripped = raw.strip()
            indent = raw[:len(raw) - len(raw.lstrip())]
            code, comment = split_comment(raw)
            code_s = code.strip()
            if code_s == "" and comment is None:
                self.i += 1
                stmts.append({"kind": "blank", "line": lineno, "indent": ""})
                continue
            if code_s == "":
                self.i += 1
                stmts.append({"kind": "comment", "line": lineno, "indent": indent, "comment": comment})
                continue
            if code_s.startswith("}"):
                if not until_close:
                    self.i += 1
                    stmts.append(self.error_stmt(lineno, indent, stripped, "stray closing brace", comment))
                    continue
                # closer belongs to the enclosing block parser
                return stmts, code_s, comment, indent
            self.i += 1
            try:
                st = self.parse_statement(code_s, comment, indent, lineno)
            except ConvError as e:
                st = self.error_stmt(lineno, indent, stripped, str(e), None)
            stmts.append(st)
        if until_close:
            raise ConvError("missing closing brace")
        return stmts, None, None, ""

    def parse_statement(self, code, comment, indent, lineno):
        st = {"line": lineno, "indent": indent, "comment": comment, "src": code}
        # block forms
        if code.endswith("{"):
            head = code[:-1].strip()
            return self.parse_block_stmt(head, st, inline_body=None)
        if code.endswith("}") and "{" in code:
            # inline block: HEAD { stmt }
            j = find_open_brace(code)
            if j is not None:
                head = code[:j].strip()
                body = code[j + 1:-1].strip()
                if head.split(" ", 1)[0] in ("if", "while", "for", "def", "elif", "else"):
                    return self.parse_block_stmt(head, st, inline_body=body)
        m = re.match(r"(let|alias|command)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", code)
        if m:
            kw, name, rhs = m.groups()
            st["kind"] = "command_decl" if kw == "command" else kw
            st["name"] = name
            st["expr"] = parse_expression(rhs)
            return st
        first = code.split(None, 1)[0]
        rest = code[len(first):].strip()
        if first in ("let", "alias"):
            raise ConvError("malformed %s" % first)
        if first == "break":
            st["kind"] = "break"
            return st
        if first == "continue":
            st["kind"] = "continue"
            return st
        if first == "return":
            st["kind"] = "return"
            st["expr"] = parse_expression(rest) if rest else None
            return st
        if first == "assert":
            st["kind"] = "assert"
            p = Parser(lex_expr(rest))
            st["expr"] = p.parse_expr()
            t = p.peek()
            if t.kind == "STR":
                st["msg"] = ("str", t.val[0], t.val[1])
                p.next()
            else:
                st["msg"] = None
            if p.peek().kind != "EOF":
                raise ConvError("trailing input after assert")
            return st
        if first == "include":
            st["kind"] = "include"
            st["expr"] = parse_expression(rest)
            return st
        if first == "echo":
            st["kind"] = "echo"
            st["args"] = tokenize_args(rest)
            return st
        if first == "quit" and rest == "":
            st["kind"] = "quit"
            return st
        if first in ("if", "elif", "else", "while", "for", "def"):
            raise ConvError("block keyword without a block")
        # expression statement?
        if code[0] in "\"'([" or code[0].isdigit() or first in ("true", "false", "none"):
            st["kind"] = "expr"
            st["expr"] = parse_expression(code)
            return st
        h = scan_head(code)
        if h == 0:
            raise ConvError("unrecognised statement")
        head = code[:h]
        after = code[h:]
        after_s = after.lstrip()
        if after_s.startswith("=") and not after_s.startswith("=="):
            st["kind"] = "assign"
            st["target"] = parse_expression(head)
            st["expr"] = parse_expression(after_s[1:].strip())
            return st
        if after.startswith("("):
            st["kind"] = "expr"
            st["expr"] = parse_expression(code)
            return st
        if after_s == "" and head.startswith("$"):
            st["kind"] = "expr"
            st["expr"] = parse_expression(code)
            return st
        # command form
        st["kind"] = "command"
        st["head"] = parse_expression(head)
        st["args"] = tokenize_args(after_s)
        return st

    def parse_block_stmt(self, head, st, inline_body):
        kw = head.split(" ", 1)[0]
        rest = head[len(kw):].strip()
        st["inline"] = inline_body is not None
        if kw == "def":
            m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$", rest)
            if not m:
                raise ConvError("malformed def")
            st["kind"] = "def"
            st["name"] = m.group(1)
            params = [p.strip() for p in m.group(2).split(",") if p.strip()]
            st["params"] = params
        elif kw == "if":
            st["kind"] = "if"
            st["cond"] = self.parse_cond(rest, st)
            st["elifs"] = []
            st["else"] = None
        elif kw == "while":
            st["kind"] = "while"
            st["cond"] = self.parse_cond(rest, st)
        elif kw == "for":
            m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.*)$", rest)
            if not m:
                raise ConvError("malformed for")
            st["kind"] = "for"
            st["var"] = m.group(1)
            st["iter"] = self.parse_cond(m.group(2), st)
        else:
            raise ConvError("unexpected block head %r" % kw)
        if inline_body is not None:
            sub = StmtParser([inline_body], self.filename)
            try:
                body = [sub.parse_statement(inline_body, None, "", st["line"])]
            except ConvError as e:
                body = [sub.error_stmt(st["line"], "", inline_body, str(e))]
            st["body"] = body
            st["close_comment"] = None
            st["close_indent"] = st["indent"]
            return st
        body, closer, ccomment, cindent = self.parse_block(True)
        st["body"] = body
        st["close_comment"] = ccomment
        st["close_indent"] = cindent
        # `} elif COND {` / `} else {` chains
        while closer != "}":
            if st["kind"] != "if":
                raise ConvError("else/elif after a non-if block")
            if closer == "} else {" or re.match(r"}\s*else\s*{$", closer):
                self.i += 1
                ebody, closer, ccomment, cindent = self.parse_block(True)
                st["else"] = ebody
                st["close_comment"] = ccomment
                st["close_indent"] = cindent
                continue
            m = re.match(r"}\s*elif\s+(.*)\{$", closer)
            if m:
                self.i += 1
                cond = self.parse_cond(m.group(1).strip(), st)
                ebody, closer, ccomment, cindent = self.parse_block(True)
                st["elifs"].append((cond, ebody))
                st["close_comment"] = ccomment
                st["close_indent"] = cindent
                continue
            raise ConvError("unrecognised block closer %r" % closer)
        self.i += 1
        return st

    def parse_cond(self, text, st):
        try:
            return parse_expression(text)
        except ConvError as e:
            return ("unparsed", text, str(e))


def find_open_brace(code):
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == '"':
            i = scan_dq(code, i)
            continue
        if c == "'":
            i = scan_sq(code, i)
            continue
        if c == "{":
            return i
        i += 1
    return None


# ---------------------------------------------------------------------------
# AST walking helpers
# ---------------------------------------------------------------------------

def walk_expr(node):
    """Yield every node in an expression tree."""
    if not isinstance(node, tuple):
        return
    yield node
    k = node[0]
    if k in ("member", "paren", "unary"):
        yield from walk_expr(node[-1] if k != "member" else node[1])
    elif k == "index":
        yield from walk_expr(node[1]); yield from walk_expr(node[2])
    elif k == "call":
        yield from walk_expr(node[1])
        for a in node[2]:
            yield from walk_expr(a)
        for _, a in node[3]:
            yield from walk_expr(a)
    elif k == "binary":
        yield from walk_expr(node[2]); yield from walk_expr(node[3])
    elif k in ("ternary",):
        yield from walk_expr(node[1]); yield from walk_expr(node[2]); yield from walk_expr(node[3])
    elif k == "range":
        yield from walk_expr(node[1]); yield from walk_expr(node[2])
    elif k == "list":
        for a in node[1]:
            yield from walk_expr(a)
    elif k == "str" and node[1] == '"':
        for part in split_interp(node[2]):
            if part[0] == "expr":
                try:
                    yield from walk_expr(parse_expression(part[1]))
                except ConvError:
                    pass
            elif part[0] == "name":
                yield ("var", part[1])


def stmt_exprs(st):
    """Expressions directly held by a statement (not its sub-blocks)."""
    k = st["kind"]
    if k in ("let", "alias", "command_decl", "return", "include", "assign"):
        if st.get("expr") is not None:
            yield st["expr"]
        if k == "assign":
            yield st["target"]
    elif k == "command":
        yield st["head"]
        for _, a in st["args"]:
            yield a
    elif k == "echo":
        for _, a in st["args"]:
            yield a
    elif k == "expr":
        yield st["expr"]
    elif k == "assert":
        yield st["expr"]
        if st["msg"] is not None:
            yield st["msg"]
    elif k == "if":
        yield st["cond"]
        for c, _ in st["elifs"]:
            yield c
    elif k == "while":
        yield st["cond"]
    elif k == "for":
        yield st["iter"]


def walk_stmts(stmts):
    for st in stmts:
        yield st
        if "body" in st:
            yield from walk_stmts(st["body"])
        for _, b in st.get("elifs", []):
            yield from walk_stmts(b)
        if st.get("else"):
            yield from walk_stmts(st["else"])


def iter_all_exprs(stmts):
    for st in walk_stmts(stmts):
        for e in stmt_exprs(st):
            yield from walk_expr(e)


# ---------------------------------------------------------------------------
# String interpolation splitting
# ---------------------------------------------------------------------------

FMT_RE = re.compile(r"^%?[-+ 0#]*\d*(\.\d+)?[a-zA-Z]$")


def split_interp(raw):
    """Split the body of a "..." string into parts:
    ('text', s) | ('expr', src, fmt) | ('name', name)."""
    parts = []
    buf = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == "\\" and i + 1 < n:
            buf.append(raw[i:i + 2])
            i += 2
            continue
        if c == "$" and i + 1 < n and raw[i + 1] == "{":
            j = scan_interp(raw, i + 1)
            inner = raw[i + 2:j - 1]
            src, fmt = split_fmt(inner)
            if buf:
                parts.append(("text", "".join(buf)))
                buf = []
            parts.append(("expr", src, fmt))
            i = j
            continue
        if c == "$" and i + 1 < n and (raw[i + 1].isalpha() or raw[i + 1] == "_"):
            m = IDENT_RE.match(raw, i + 1)
            if buf:
                parts.append(("text", "".join(buf)))
                buf = []
            parts.append(("name", m.group(0)))
            i = m.end()
            continue
        buf.append(c)
        i += 1
    if buf:
        parts.append(("text", "".join(buf)))
    return parts


def split_fmt(inner):
    """Split `EXPR:FMT` at a top-level colon whose tail looks like a format."""
    depth = 0
    i = 0
    n = len(inner)
    last_colon = None
    while i < n:
        c = inner[i]
        if c == '"':
            i = scan_dq(inner, i); continue
        if c == "'":
            i = scan_sq(inner, i); continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ":" and depth == 0:
            last_colon = i
        i += 1
    if last_colon is not None:
        tail = inner[last_colon + 1:].strip()
        if FMT_RE.match(tail):
            return inner[:last_colon].strip(), tail
    return inner.strip(), None


# ---------------------------------------------------------------------------
# The converter
# ---------------------------------------------------------------------------

LUA_PREC = {
    "or": 1, "and": 2, "==": 3, "~=": 3, "<": 3, "<=": 3, ">": 3, ">=": 3,
    "|": 4, "~": 5, "&": 6, "<<": 7, ">>": 7, "..": 8, "+": 9, "-": 9,
    "*": 10, "/": 10, "//": 10, "%": 10,
}
UNARY_PREC = 11
ATOM = 100


def lua_str_escape_text(text):
    """Fix the escapes of a "..." text segment for a Lua string."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "\\" and i + 1 < n:
            d = text[i + 1]
            if d == "$":
                out.append("$")
            elif d == "0" and i + 2 < n and text[i + 2].isdigit():
                out.append("\\000")
            else:
                out.append(c + d)
            i += 2
            continue
        if c == "\n":
            out.append("\\n")
        else:
            out.append(c)
        i += 1
    return "".join(out)


def lua_raw_string(raw):
    out = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == "\\":
            if i + 1 < n and raw[i + 1] == "'":
                out.append("\\'")
                i += 2
                continue
            out.append("\\\\")
            i += 1
            continue
        out.append(c)
        i += 1
    return "'" + "".join(out) + "'"


def lua_plain_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


class Scope:
    def __init__(self, parent=None, is_function=False):
        self.names = set()
        self.parent = parent
        self.is_function = is_function

    def is_outer_local(self, name):
        """True when `name` is a local of an enclosing function (an upvalue
        of the current one): F"..." cannot read it unless the code names it."""
        s = self
        crossed = False
        while s is not None:
            if name in s.names:
                return crossed
            if s.is_function:
                crossed = True
            s = s.parent
        return False

    def has(self, name):
        s = self
        while s is not None:
            if name in s.names:
                return True
            s = s.parent
        return False


class Converter:
    def __init__(self, filename, text, lib_path=None, lib_names=None, module=None):
        self.filename = filename
        self.module_flag = module
        self.lines = text.split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()
        self.lib_path = lib_path
        self.mac_defs, self.mac_lets = lib_names if lib_names else (set(), set())
        self.ports = []          # (lineno, reason)
        self.notes = []          # (lineno, note) non-PORT observations
        self.vars_names = set()  # undeclared $names mapped to vars.NAME
        self.out = []
        self.forward = set()
        self.mac_user = False
        self.mac_direct = False
        self.defs = []           # (name, lineno)
        self.def_names = set()
        self.included_defs = set()
        self.included_lets = set()
        self.declared = set()    # every let/for/param/alias name in the file
        self.vars_preset = set() # names written as vars.NAME before the include
        self.origin = {}         # binding name -> 'list' | 'object' | 'scalar' | 'unknown'
        self.loop_vars_1based = []   # stack of loop vars converted to 1-based
        self.cur_line = 0
        self.mac_include_line = None
        self.module = False          # included by other files: top-level names are globals
        self.includer_defs = set()   # names the including files define (module mode)
        self.includer_lets = set()
        self.included_refs = set()   # names the included modules reference
        self.shared = set()          # own top-level names that included modules use
        self.top_lets = set()

    # -- diagnostics --------------------------------------------------------
    def port(self, reason):
        self.ports.append((self.cur_line, reason))
        return "-- PORT: " + reason

    # -- analysis ------------------------------------------------------------
    def analyze(self, stmts):
        order = []
        first_include_line = None
        has_quit = any(st["kind"] == "quit" for st in stmts)
        if self.module_flag is None:
            self.module = os.path.basename(self.filename) != "test.script" and not has_quit
        else:
            self.module = self.module_flag
        if self.module:
            self.includer_defs, self.includer_lets = find_includer_names(self.filename)
        for st in stmts:
            if st["kind"] == "include" and first_include_line is None:
                first_include_line = st["line"]
            if st["kind"] == "let":
                self.top_lets.add(st["name"])
        for st in walk_stmts(stmts):
            k = st["kind"]
            if k == "def":
                order.append((st["name"], st["line"]))
                self.def_names.add(st["name"])
                for p in st["params"]:
                    self.declared.add(p)
            elif k == "let":
                top = st in stmts
                if top and first_include_line is not None and st["line"] < first_include_line \
                        and st["name"] in (VARS_PRESET_NAMES | self.mac_lets):
                    self.vars_preset.add(st["name"])
                    st["vars_preset"] = True
                else:
                    self.declared.add(st["name"])
                    self.origin[st["name"]] = merge_origin(self.origin.get(st["name"]), self.expr_origin(st["expr"]))
            elif k == "for":
                self.declared.add(st["var"])
                self.origin.setdefault(st["var"], "unknown")
            elif k in ("alias", "command_decl"):
                self.declared.add(st["name"])
                self.origin[st["name"]] = "object"
            elif k == "include":
                self.analyze_include(st)
        self.defs = order
        # forward references: calls to a def that is defined later
        pos = {name: line for name, line in order}
        # top-level statements calling a def defined later in the file, and
        # def bodies calling later defs
        def calls_in(stmts_):
            names = set()
            for e in iter_all_exprs(stmts_):
                if e[0] == "call" and e[1][0] == "id":
                    names.add(e[1][1])
            return names
        for st in stmts:
            if st["kind"] == "def":
                for n in calls_in(st["body"]):
                    if n in pos and pos[n] > st["line"]:
                        self.forward.add(n)
            else:
                sub = [st]
                for n in calls_in(sub):
                    if n in pos and pos[n] > st["line"]:
                        self.forward.add(n)
        # does the file use the mac library?
        if not self.mac_direct:
            used = calls_in(stmts)
            if used & self.mac_defs - self.def_names - self.included_defs:
                self.mac_user = True
                self.mac_indirect = True
        self.own_refs = set()
        for e in iter_all_exprs(stmts):
            if e[0] == "call" and e[1][0] == "id":
                self.own_refs.add(e[1][1])
            elif e[0] == "var":
                self.own_refs.add(e[1])
        for st in walk_stmts(stmts):
            if st["kind"] == "command" and st["head"][0] == "id":
                self.own_refs.add(st["head"][1])
        # own top-level names an included module references: globals
        self.shared = (self.included_refs & (self.def_names | self.top_lets)) - self.mac_defs
        self.forward -= self.shared

    def analyze_include(self, st):
        e = st["expr"]
        path = None
        if e[0] == "str" and "${" not in e[2] and "$" not in e[2]:
            path = e[2]
        st["path"] = path
        if path is None:
            return
        if os.path.basename(path) == "mac.script":
            self.mac_user = True
            self.mac_direct = True
            self.mac_include_line = st["line"]
            return
        full = os.path.join(os.path.dirname(os.path.abspath(self.filename)), path)
        if os.path.exists(full):
            try:
                with open(full, encoding="utf-8") as f:
                    inc_text = f.read()
                d, l, inc_mac, refs, is_mod = collect_names(inc_text, full, depth=1)
                self.included_defs |= d
                self.included_lets |= l
                self.included_refs |= refs
                st["module"] = is_mod
                st["names"] = d | l
                if inc_mac:
                    self.mac_user = True
            except Exception as ex:  # pragma: no cover
                self.notes.append((st["line"], "could not read include %s: %s" % (path, ex)))

    def expr_origin(self, e):
        k = e[0]
        if k == "list":
            return "list"
        if k == "call":
            fn = e[1]
            if fn[0] == "id" and fn[1] in ("try",):
                return self.expr_origin(e[2][0]) if e[2] else "unknown"
            if fn[0] == "id" and fn[1] in ("len", "contains", "error"):
                return "scalar"
            return "list"
        if k == "index":
            return "list"
        if k == "var":
            return self.origin.get(e[1], "unknown")
        if k in ("member", "id"):
            return "object"
        if k == "paren":
            return self.expr_origin(e[1])
        if k in ("num", "str", "const", "binary", "unary", "bare"):
            return "scalar"
        return "unknown"

    # -- names -----------------------------------------------------------------
    def lua_name(self, name):
        if name in LUA_KEYWORDS:
            return name + "_"
        return name

    def resolve_var(self, name, scope):
        """$name read -> Lua expression text."""
        if scope.has(name) or name in self.declared or name in self.included_lets \
                or name in self.includer_lets:
            return self.lua_name(name)
        if self.mac_user and name in self.mac_lets and not self.before_mac_include():
            return "mac." + name
        if name in REG_ALIASES:
            return "machine.cpu." + name
        self.vars_names.add(name)
        if not UPPER_NAME_RE.match(name):
            self.port("undeclared binding '%s' read as a --var" % name)
        return "vars." + name

    def before_mac_include(self):
        """A mac-library name read before the library is included is the
        --var the library itself reads (`let ROW_SET = try($ROW_SET, …)`)."""
        return self.mac_include_line is not None and self.cur_line < self.mac_include_line

    def is_known_var(self, name, scope):
        return scope.has(name) or name in self.declared or name in self.included_lets \
            or name in self.includer_lets or (self.mac_user and name in self.mac_lets) \
            or name in REG_ALIASES

    def resolve_fn(self, name, scope):
        """Called function name -> (Lua text, is_user_fn)."""
        if name in self.def_names or name in self.included_defs or scope.has(name) or name in self.declared \
                or name in self.includer_defs:
            return self.lua_name(name), True
        if self.mac_user and name in self.mac_defs:
            return "mac." + name, True
        if name in BUILTIN_COMMANDS:
            return BUILTIN_COMMANDS[name], False
        if name in ROOTS:
            return name, False
        return name, None

    # -- expressions -------------------------------------------------------------
    def expr(self, node, scope):
        """Return (text, prec)."""
        k = node[0]
        if k == "unparsed":
            return "nil --[[ PORT: unparsed: %s ]]" % node[1].replace("]]", "] ]"), ATOM
        if k == "num":
            _, text, width, is_float = node
            t = text.replace("_", "")
            if t[:2].lower() == "0b":
                t = str(int(t[2:], 2))
            if width is not None:
                return "gs.bytes(%s, %d)" % (t, width), ATOM
            return t, ATOM
        if k == "str":
            return self.string(node, scope), ATOM
        if k == "bare":
            return lua_plain_string(node[1]), ATOM
        if k == "const":
            return {"true": "true", "false": "false", "none": "nil"}[node[1]], ATOM
        if k == "var":
            return self.resolve_var(node[1], scope), ATOM
        if k == "id":
            name = node[1]
            if name in ROOTS or name in ("try", "error", "range", "len", "contains"):
                return name, ATOM
            text, kind = self.resolve_fn(name, scope)
            if kind is None:
                if self.module:
                    self.notes.append((self.cur_line, "'%s' resolved as a global (defined by an including file)" % name))
                else:
                    self.port("unknown name '%s' (not a binding, def, library name or root)" % name)
            return text, ATOM
        if k == "paren":
            t, _ = self.expr(node[1], scope)
            return "(" + t + ")", ATOM
        if k == "member":
            obj, _ = self.expr_atom(node[1], scope)
            name = node[2]
            if name.isdigit():
                return "%s[%s]" % (obj, name), ATOM
            if name in LUA_KEYWORDS:
                return '%s["%s"]' % (obj, name), ATOM
            return "%s.%s" % (obj, name), ATOM
        if k == "index":
            return self.index(node, scope), ATOM
        if k == "call":
            return self.call(node, scope), ATOM
        if k == "list":
            items = [self.expr(x, scope)[0] for x in node[1]]
            return "{" + ", ".join(items) + "}", ATOM
        if k == "unary":
            op, x = node[1], node[2]
            xt, xp = self.expr(x, scope)
            if xp < UNARY_PREC:
                xt = "(" + xt + ")"
            if op == "!":
                # `!call(...)`: the old evaluator read a method's error as
                # false, and 0 / "" / an empty list as false; gs.truthy does
                # both (the tree raises in Lua).
                if x[0] == "call":
                    return "not gs.truthy(function() return %s end)" % xt, UNARY_PREC
                return "not " + xt, UNARY_PREC
            if op == "+":
                return xt, xp
            return op + xt, UNARY_PREC
        if k == "binary":
            return self.binary(node, scope)
        if k == "ternary":
            c, _ = self.expr(node[1], scope)
            a, _ = self.expr(node[2], scope)
            b, _ = self.expr(node[3], scope)
            self.port("ternary: '(c) and (a) or (b)' is wrong when a is false/nil")
            return "(%s) and (%s) or (%s)" % (c, a, b), 1
        if k == "range":
            a, _ = self.expr(node[1], scope)
            b, _ = self.expr(node[2], scope)
            self.port("range expression outside a for loop")
            return "gs.range(%s, %s)" % (a, b), ATOM
        raise ConvError("cannot emit %r" % (k,))

    def expr_atom(self, node, scope):
        t, p = self.expr(node, scope)
        if p < ATOM:
            t = "(" + t + ")"
        return t, ATOM

    def try_none_test(self, node):
        """try(E, none) == none / != none -> E raised / E succeeded."""
        op, a, b = node[1], node[2], node[3]
        if op not in ("==", "!=") or b != ("const", "none"):
            return None
        if a[0] == "call" and a[1] == ("id", "try") and len(a[2]) == 2 and not a[3] and a[2][1] == ("const", "none"):
            return op, a[2][0]
        return None

    def binary(self, node, scope):
        op, a, b = node[1], node[2], node[3]
        tn = self.try_none_test(node)
        if tn is not None:
            op2, e = tn
            t = "gs.ok(function() return %s end)" % self.expr(e, scope)[0]
            if op2 == "==":
                return "not " + t, UNARY_PREC
            return t, ATOM
        # A list or map compared with its text (`== "[a, b]"`, `== "{...}"`):
        # the old evaluator rendered the collection; gs.str does the same.
        if op in ("==", "!=") and b[0] == "str" and b[2][:1] in ("[", "{"):
            at, _ = self.expr(a, scope)
            return "gs.str(%s) %s %s" % (at, {"!=": "~="}.get(op, op), self.expr(b, scope)[0]), LUA_PREC["=="]
        if op in ("==", "!=") and a[0] == "str" and a[2][:1] in ("[", "{"):
            bt, _ = self.expr(b, scope)
            return "%s %s gs.str(%s)" % (self.expr(a, scope)[0], {"!=": "~="}.get(op, op), bt), LUA_PREC["=="]
        lua_op = {"&&": "and", "||": "or", "!=": "~=", "^": "~", "/": "//"}.get(op, op)
        if op == "+" and (self.is_string_expr(a) or self.is_string_expr(b)):
            lua_op = ".."
            self.notes.append((self.cur_line, "string '+' mapped to '..'"))
        if op == "/":
            self.notes.append((self.cur_line, "'/' mapped to integer division '//'"))
        prec = LUA_PREC[lua_op]
        at, ap = self.expr(a, scope)
        bt, bp = self.expr(b, scope)
        if ap < prec or (ap == prec and lua_op == ".."):
            at = "(" + at + ")"
        if bp <= prec and not (bp == prec and lua_op == ".."):
            bt = "(" + bt + ")"
        return "%s %s %s" % (at, lua_op, bt), prec

    def is_string_expr(self, node):
        return node[0] == "str" or node[0] == "bare"

    def index(self, node, scope):
        obj, idx = node[1], node[2]
        obj_t, _ = self.expr_atom(obj, scope)
        idx_t, _ = self.expr(idx, scope)
        if self.is_list_index(obj, scope) and idx[0] != "str":
            # a 1-based Lua sequence
            if idx[0] == "var" and idx[1] in self.loop_vars_1based:
                return "%s[%s]" % (obj_t, idx_t)
            if idx[0] == "num" and not idx[3] and idx[2] is None:
                n = int(idx[1].replace("_", ""), 0)
                self.port("1-based index")
                return "%s[%d]" % (obj_t, n + 1)
            self.port("1-based index")
            if idx[0] in ("binary", "unary", "ternary"):
                idx_t = "(" + idx_t + ")"
            return "%s[%s + 1]" % (obj_t, idx_t)
        return "%s[%s]" % (obj_t, idx_t)

    def is_list_index(self, obj, scope):
        """Is `obj[...]` an index into a list value (vs. a tree path)?"""
        if obj[0] == "call":
            return True
        if obj[0] == "index":
            return self.is_list_index(obj[1], scope)
        if obj[0] == "var":
            o = self.origin.get(obj[1], "unknown")
            if o == "list":
                return True
            if o in ("object", "scalar"):
                return False
            self.port("index on a binding of unknown origin: treated as a list")
            return True
        return False

    def call(self, node, scope):
        fn, args, named = node[1], node[2], node[3]
        # builtins
        if fn[0] == "id":
            name = fn[1]
            if name == "len" and len(args) == 1 and not named:
                t, p = self.expr(args[0], scope)
                if p < ATOM:
                    t = "(" + t + ")"
                return "#" + t
            if name == "contains" and not named:
                return "gs.contains(%s)" % ", ".join(self.expr(a, scope)[0] for a in args)
            if name == "error":
                return "error(%s)" % ", ".join(self.expr(a, scope)[0] for a in args)
            if name == "try" and len(args) == 2 and not named:
                e, fb = args
                if e[0] == "var" and not (scope.has(e[1]) or e[1] in self.declared or e[1] in self.included_lets) \
                        and not (self.mac_user and e[1] in self.mac_lets and not self.before_mac_include()):
                    # try($VAR, fallback) on a --var: nil when unset
                    self.vars_names.add(e[1])
                    return "(vars.%s or %s)" % (e[1], self.expr(fb, scope)[0])
                return "gs.try(function() return %s end, %s)" % (self.expr(e, scope)[0], self.expr(fb, scope)[0])
            if name == "range" and not named:
                self.port("range() builtin used as a value")
                return "range(%s)" % ", ".join(self.expr(a, scope)[0] for a in args)
            if name in ("row_end", "row_end_milestone") and self.mac_user and name not in self.def_names:
                return self.row_end_call(name, args, named, scope)
        fn_t, _ = self.expr_atom(fn, scope)
        is_user = fn[0] == "id" and (fn[1] in self.def_names or fn[1] in self.included_defs
                                     or (self.mac_user and fn[1] in self.mac_defs))
        if fn[0] == "id" and fn[1] in ROOTS and fn[1] not in ("methods", "meta"):
            is_user = False
        arg_ts = [self.expr(a, scope)[0] for a in args]
        if named:
            fields = []
            for n, v in named:
                if self.is_template_arg(fn, n) and v[0] == "str" and v[1] == '"' and "$" in v[2]:
                    self.port("template argument kept in the old ${...} syntax (evaluated at fire time)")
                    vt = '"' + v[2] + '"'
                else:
                    vt = self.expr(v, scope)[0]
                key = n if n not in LUA_KEYWORDS else '["%s"]' % n
                fields.append("%s = %s" % (key, vt))
            table = "{" + ", ".join(fields) + "}"
            if is_user:
                self.port("named arguments passed to a user function as a table")
            if arg_ts:
                return "%s(%s, %s)" % (fn_t, ", ".join(arg_ts), table)
            return "%s%s" % (fn_t, table)
        return "%s(%s)" % (fn_t, ", ".join(arg_ts))

    def is_template_arg(self, fn, argname):
        path = self.path_text(fn)
        for suffix, n in TEMPLATE_ARGS:
            if n == argname and path.endswith(suffix):
                return True
        return False

    def path_head(self, node):
        while node[0] in ("member", "index", "call"):
            node = node[1]
        return node[1] if node[0] in ("id", "var") else None

    def path_text(self, node):
        if node[0] == "id":
            return node[1]
        if node[0] == "member":
            return self.path_text(node[1]) + "." + node[2]
        if node[0] == "index":
            return self.path_text(node[1]) + "[]"
        if node[0] == "var":
            return "$" + node[1]
        return "?"

    def row_end_call(self, name, args, named, scope):
        target = "mac.row" if name == "row_end" else "mac.milestone"
        if len(args) == 2 and not named:
            label = self.expr(args[0], scope)[0]
            r = args[1]
            if r[0] == "call" and r[1] == ("id", "try") and len(r[2]) == 2 \
                    and r[2][1][0] == "str" and r[2][1][2] == "FAILED":
                inner = r[2][0]
                if inner[0] == "call" and inner[1][0] == "id" and not inner[2] and not inner[3]:
                    fn_t, _ = self.expr(inner[1], scope)
                    return "%s(%s, %s)" % (target, label, fn_t)
                return "%s(%s, function() return %s end)" % (target, label, self.expr(inner, scope)[0])
            self.port("%s: row result is not try(row_fn(), \"FAILED\")" % name)
            return "%s(%s, function() return %s end)" % (target, label, self.expr(r, scope)[0])
        self.port("%s with unexpected arguments" % name)
        return "%s(%s)" % (target, ", ".join(self.expr(a, scope)[0] for a in args))

    def string(self, node, scope):
        quote, raw = node[1], node[2]
        if quote == "'":
            return lua_raw_string(raw)
        parts = split_interp(raw)
        if not any(p[0] in ("expr", "name") for p in parts):
            return '"' + lua_str_escape_text(raw) + '"'
        # a single whole-string splice of a plain binding -> the binding
        parts = [("text", "$" + p[1]) if p[0] == "name" and HEX_NAME_RE.match(p[1]) and not self.is_known_var(p[1], scope) else p
                 for p in parts]
        if not any(p[0] in ("expr", "name") for p in parts):
            return '"' + lua_str_escape_text("".join(p[1] for p in parts)) + '"'
        if len(parts) == 1 and parts[0][0] == "name":
            return self.resolve_var(parts[0][1], scope)
        if len(parts) == 1 and parts[0][0] == "expr" and parts[0][2] is None:
            try:
                e = parse_expression(parts[0][1])
            except ConvError:
                e = None
            if e is not None and e[0] == "var":
                return self.resolve_var(e[1], scope)
        # F"..." reads the caller's locals and the function's upvalues; a
        # file-level local that this function names only inside the template
        # is not an upvalue, so such a string is spelled with `..` instead.
        if self.splices_outer_local(parts, scope):
            return self.concat_string(parts, scope)
        pieces = []
        for p in parts:
            if p[0] == "text":
                pieces.append(lua_str_escape_text(p[1]))
            elif p[0] == "name":
                pieces.append("${" + self.resolve_var(p[1], scope) + "}")
            else:
                src, fmt = p[1], p[2]
                try:
                    e = parse_expression(src)
                    t, _ = self.expr(e, scope)
                except ConvError as ex:
                    self.port("unparsed splice ${%s}: %s" % (src, ex))
                    t = src
                t = t.replace("\\", "\\\\").replace('"', '\\"')
                pieces.append("${" + t + (":" + fmt if fmt else "") + "}")
        return 'F"' + "".join(pieces) + '"'

    def splices_outer_local(self, parts, scope):
        for p in parts:
            if p[0] == "name":
                if scope.is_outer_local(p[1]):
                    return True
            elif p[0] == "expr":
                try:
                    e = parse_expression(p[1])
                except ConvError:
                    continue
                for n in walk_expr(e):
                    if n[0] == "var" and scope.is_outer_local(n[1]):
                        return True
        return False

    def concat_string(self, parts, scope):
        pieces = []
        for p in parts:
            if p[0] == "text":
                if p[1]:
                    pieces.append('"' + lua_str_escape_text(p[1]) + '"')
            elif p[0] == "name":
                pieces.append("tostring(%s)" % self.resolve_var(p[1], scope))
            else:
                src, fmt = p[1], p[2]
                try:
                    t, _ = self.expr(parse_expression(src), scope)
                except ConvError as ex:
                    self.port("unparsed splice ${%s}: %s" % (src, ex))
                    t = src
                if fmt:
                    spec = fmt if fmt.startswith("%") else "%" + fmt
                    pieces.append('("%s"):format(%s)' % (spec, t))
                else:
                    pieces.append("tostring(%s)" % t)
        self.notes.append((self.cur_line, "template spelled with '..' (names an enclosing function's local)"))
        return " .. ".join(pieces) if pieces else '""'

    # -- statements --------------------------------------------------------------
    def emit(self, text, indent=""):
        self.out.append(indent + text if text else "")

    def convert(self):
        parser = StmtParser(self.lines, self.filename)
        try:
            stmts, _, _, _ = parser.parse_block(False)
        except ConvError as e:
            # fall back: treat the rest as raw
            stmts = [{"kind": "raw", "line": parser.i + 1, "indent": "", "text": self.lines[parser.i] if parser.i < len(self.lines) else "", "reason": str(e), "comment": None}]
        self.analyze(stmts)
        self.emit("-- Converted from %s by scripts/script-to-lua.py" % os.path.basename(self.filename))
        scope = Scope()
        if getattr(self, "mac_indirect", False):
            rel = self.lib_rel_path()
            self.cur_line = 1
            self.emit('local mac = dofile("%s") -- NOTE: no direct mac.script include (this file is included by another); the library runs once per chunk' % rel)
        if self.forward:
            self.cur_line = 1
            self.emit("local %s %s" % (", ".join(sorted(self.forward)), self.port("forward-declared: called before their definition in file order")))
        self.emit_block(stmts, scope, top=True)
        return "\n".join(self.out) + "\n"

    def lib_rel_path(self):
        d = os.path.dirname(os.path.abspath(self.filename))
        if self.lib_path:
            rel = os.path.relpath(os.path.abspath(self.lib_path), d)
            return re.sub(r"\.script$", ".lua", rel)
        # guess: the lib dir beside the suite dir
        if os.path.basename(d) == "lib":
            return "mac.lua"
        return "../lib/mac.lua"

    def with_comment(self, text, st):
        c = st.get("comment")
        if c is not None:
            return text + " --" + c
        return text

    def emit_block(self, stmts, scope, top=False, loop=None):
        n = len(stmts)
        for idx, st in enumerate(stmts):
            self.cur_line = st["line"]
            # is this the last real statement of the block?
            last = all(s["kind"] in ("blank", "comment") for s in stmts[idx + 1:])
            self.emit_stmt(st, scope, last=last, top=top, loop=loop)

    def emit_stmt(self, st, scope, last, top, loop):
        k = st["kind"]
        ind = st["indent"]
        if k == "blank":
            self.emit("")
            return
        if k == "comment":
            self.emit("--" + st["comment"], ind)
            return
        if k == "raw":
            self.emit("-- " + st["text"].replace("\n", " "), ind)
            self.emit(self.port(st["reason"]), ind)
            return
        try:
            if k == "let":
                self.emit_let(st, scope)
            elif k == "assign":
                self.emit_assign(st, scope)
            elif k in ("alias", "command_decl"):
                t, _ = self.expr(st["expr"], scope)
                scope.names.add(st["name"])
                self.emit(self.with_comment("local %s = %s %s" % (self.lua_name(st["name"]), t, self.port("%s declaration mapped to a local" % k.replace("_decl", ""))), st), ind)
            elif k == "break":
                self.emit(self.with_comment("break", st), ind)
            elif k == "continue":
                if loop is not None:
                    loop["continue"] = True
                self.emit(self.with_comment("goto continue", st), ind)
            elif k == "return":
                if st["expr"] is None:
                    t = "return"
                else:
                    t = "return " + self.expr(st["expr"], scope)[0]
                if not last:
                    t = "do " + t + " end"
                self.emit(self.with_comment(t, st), ind)
            elif k == "assert":
                e, _ = self.expr(st["expr"], scope)
                if st["msg"] is not None:
                    m = self.string(st["msg"], scope)
                    self.emit(self.with_comment("assert(%s, %s)" % (e, m), st), ind)
                else:
                    self.emit(self.with_comment("assert(%s)" % e, st), ind)
            elif k == "include":
                self.emit_include(st, scope)
            elif k == "echo":
                args = [self.expr(a, scope)[0] for _, a in st["args"]]
                for name, _ in st["args"]:
                    if name:
                        self.port("named argument to echo")
                self.emit(self.with_comment("echo(%s)" % ", ".join(args), st), ind)
            elif k == "quit":
                self.emit(self.with_comment("quit()", st), ind)
            elif k == "expr":
                self.emit_expr_stmt(st, scope)
            elif k == "command":
                self.emit_command(st, scope)
            elif k == "def":
                self.emit_def(st, scope)
            elif k == "if":
                self.emit_if(st, scope, loop)
            elif k == "while":
                self.emit_while(st, scope)
            elif k == "for":
                self.emit_for(st, scope)
            else:
                raise ConvError("unhandled statement kind %s" % k)
        except ConvError as e:
            self.emit("-- " + st.get("src", "?"), ind)
            self.emit(self.port("could not convert: %s" % e), ind)

    def emit_let(self, st, scope):
        ind = st["indent"]
        t, _ = self.expr(st["expr"], scope)
        if st.get("vars_preset"):
            self.emit(self.with_comment("vars.%s = %s" % (st["name"], t), st), ind)
            return
        scope.names.add(st["name"])
        name = self.lua_name(st["name"])
        if name != st["name"]:
            self.port("binding '%s' renamed: Lua keyword" % st["name"])
        if scope.parent is None and (self.module or st["name"] in self.shared):
            why = "global: used by an included file" if st["name"] in self.shared else "global: included file"
            self.emit(self.with_comment("%s = %s -- %s" % (name, t, why), st), ind)
            return
        self.emit(self.with_comment("local %s = %s" % (name, t), st), ind)

    def emit_assign(self, st, scope):
        ind = st["indent"]
        tgt = st["target"]
        if tgt[0] == "var":
            name = tgt[1]
            if name in self.vars_preset or (not scope.has(name) and name not in self.declared and name not in self.included_lets):
                if self.mac_user and name in self.mac_lets:
                    tt = "mac." + name
                elif name in REG_ALIASES:
                    tt = "machine.cpu." + name
                else:
                    tt = "vars." + name
                    self.vars_names.add(name)
            else:
                tt = self.lua_name(name)
        else:
            tt, _ = self.expr(tgt, scope)
        v, _ = self.expr(st["expr"], scope)
        self.emit(self.with_comment("%s = %s" % (tt, v), st), ind)

    def emit_include(self, st, scope):
        ind = st["indent"]
        path = st.get("path")
        if path is None:
            t, _ = self.expr(st["expr"], scope)
            self.emit(self.with_comment("dofile(%s) %s" % (t, self.port("dynamic include path; .script -> .lua rename not applied")), st), ind)
            return
        lua_path = re.sub(r"\.script$", ".lua", path)
        if os.path.basename(path) == "mac.script":
            self.emit(self.with_comment('local mac = dofile("%s")' % lua_path, st), ind)
            return
        if st.get("module") or not (st.get("names", set()) & self.own_refs):
            self.emit(self.with_comment('dofile("%s")' % lua_path, st), ind)
            return
        self.emit(self.with_comment('dofile("%s") %s' % (lua_path, self.port("include of a standalone script: its local functions/bindings are not visible here")), st), ind)

    def emit_expr_stmt(self, st, scope):
        ind = st["indent"]
        e = st["expr"]
        t, _ = self.expr(e, scope)
        if e[0] == "call" and not t.startswith("#") and not t.startswith("(vars."):
            self.emit(self.with_comment(t, st), ind)
        elif e[0] in ("id", "member", "index", "var"):
            self.emit(self.with_comment("local _ = %s" % t, st), ind)
        else:
            self.emit(self.with_comment("local _ = %s %s" % (t, self.port("expression statement")), st), ind)

    def emit_command(self, st, scope):
        ind = st["indent"]
        head = st["head"]
        args = st["args"]
        pos = [a for n, a in args if n is None]
        named = [(n, a) for n, a in args if n is not None]
        node = ("call", head, pos, named)
        if not args and head[0] in ("member", "id", "var", "index"):
            # bare path: a method call or an attribute read
            last = head[2] if head[0] == "member" else (head[1] if head[0] == "id" else None)
            if head[0] == "id":
                name = head[1]
                if name in self.def_names or name in self.included_defs or (self.mac_user and name in self.mac_defs) \
                        or name in BUILTIN_COMMANDS or scope.has(name) or name in self.declared:
                    pass
                elif name in ROOTS:
                    self.emit(self.with_comment("local _ = %s" % name, st), ind)
                    return
            elif head[0] == "index" or (last is not None and last not in METHOD_VERBS):
                # `files.images[0]` or `machine.cpu.pc` alone: a read
                t, _ = self.expr(head, scope)
                self.emit(self.with_comment("local _ = %s" % t, st), ind)
                return
        t = self.call(node, scope)
        self.emit(self.with_comment(t, st), ind)

    def emit_def(self, st, scope):
        ind = st["indent"]
        name = st["name"]
        inner = Scope(scope, is_function=True)
        for p in st["params"]:
            inner.names.add(p)
        params = ", ".join(self.lua_name(p) for p in st["params"])
        glob = scope.parent is None and (self.module or name in self.shared)
        if glob:
            head = "function %s(%s)" % (self.lua_name(name), params)
            st = dict(st, comment=(st.get("comment") or "") + (" global: used by an included file" if name in self.shared else " global: included file"))
        elif name in self.forward:
            head = "function %s(%s)" % (self.lua_name(name), params)
        else:
            head = "local function %s(%s)" % (self.lua_name(name), params)
        scope.names.add(name)
        if st["inline"]:
            saved = self.out
            self.out = []
            self.emit_block(st["body"], inner)
            body = self.out
            self.out = saved
            self.emit(self.with_comment(head + " " + "; ".join(b.strip() for b in body) + " end", st), ind)
            return
        self.emit(self.with_comment(head, st), ind)
        self.emit_block(st["body"], inner)
        self.emit(self.close("end", st), st["close_indent"])

    def close(self, text, st):
        c = st.get("close_comment")
        if c is not None:
            return text + " --" + c
        return text

    def match_row_idiom(self, st):
        """if row_on(L) { row_end(L, try(f(), "FAILED")) } -> mac.row(L, f)."""
        if not self.mac_user or "row_on" in self.def_names or "row_end" in self.def_names:
            return None
        cond = st["cond"]
        if not (cond[0] == "call" and cond[1] == ("id", "row_on") and len(cond[2]) == 1 and not cond[3]):
            return None
        if st["elifs"] or st["else"]:
            return None
        body = [s for s in st["body"] if s["kind"] not in ("blank",)]
        if len(body) != 1:
            return None
        b = body[0]
        if b["kind"] == "expr":
            call = b["expr"]
        elif b["kind"] == "command":
            call = ("call", b["head"], [a for n, a in b["args"] if n is None], [(n, a) for n, a in b["args"] if n])
        else:
            return None
        if not (call[0] == "call" and call[1][0] == "id" and call[1][1] in ("row_end", "row_end_milestone")):
            return None
        if len(call[2]) != 2 or call[2][0] != cond[2][0]:
            return None
        return b, call

    def emit_if(self, st, scope, loop):
        ind = st["indent"]
        m = self.match_row_idiom(st)
        if m is not None:
            b, call = m
            t = self.call(call, scope)
            c = st.get("comment") or b.get("comment")
            self.emit(t + (" --" + c if c is not None else ""), ind)
            return
        cond, _ = self.expr(st["cond"], scope)
        if st["inline"]:
            saved = self.out
            self.out = []
            inner = Scope(scope)
            self.emit_block(st["body"], inner, loop=loop)
            body = self.out
            self.out = saved
            text = "if %s then %s end" % (cond, "; ".join(b.strip() for b in body if b.strip()))
            self.emit(self.with_comment(text, st), ind)
            return
        self.emit(self.with_comment("if %s then" % cond, st), ind)
        self.emit_block(st["body"], Scope(scope), loop=loop)
        for c, body in st["elifs"]:
            ct, _ = self.expr(c, scope)
            self.emit("elseif %s then" % ct, ind)
            self.emit_block(body, Scope(scope), loop=loop)
        if st["else"] is not None:
            self.emit("else", ind)
            self.emit_block(st["else"], Scope(scope), loop=loop)
        self.emit(self.close("end", st), st["close_indent"])

    def emit_loop_body(self, st, scope, head, inline):
        ind = st["indent"]
        loop = {"continue": False}
        inner = Scope(scope)
        saved = self.out
        self.out = []
        self.emit_block(st["body"], inner, loop=loop)
        body = self.out
        self.out = saved
        if inline and not loop["continue"]:
            self.emit(self.with_comment("%s %s end" % (head, "; ".join(b.strip() for b in body if b.strip())), st), ind)
            return
        self.emit(self.with_comment(head, st), ind)
        self.out.extend(body)
        if loop["continue"]:
            self.emit("::continue::", ind + "    ")
        self.emit(self.close("end", st), st["close_indent"] if not inline else ind)

    def emit_while(self, st, scope):
        cond, _ = self.expr(st["cond"], scope)
        self.emit_loop_body(st, scope, "while %s do" % cond, st["inline"])

    def emit_for(self, st, scope):
        var = self.lua_name(st["var"])
        it = st["iter"]
        inner_scope = Scope(scope)
        inner_scope.names.add(st["var"])
        one_based = False
        if it[0] == "range":
            a, b = it[1], it[2]
            at, _ = self.expr(a, scope)
            # for i in 0..len($l) over a list the body indexes with $i
            if a[0] == "num" and a[1] == "0" and b[0] == "call" and b[1] == ("id", "len") and len(b[2]) == 1 \
                    and b[2][0][0] == "var" and self.only_index_uses(st["body"], st["var"]):
                one_based = True
                head = "for %s = 1, #%s do" % (var, self.resolve_var(b[2][0][1], scope))
            else:
                head = "for %s = %s, %s do" % (var, at, self.minus_one(b, scope))
        elif it[0] == "call" and it[1] == ("id", "range") and not it[3] and 1 <= len(it[2]) <= 3:
            r = it[2]
            if len(r) == 1:
                head = "for %s = 0, %s do" % (var, self.minus_one(r[0], scope))
            else:
                head = "for %s = %s, %s%s do" % (var, self.expr(r[0], scope)[0], self.minus_one(r[1], scope),
                                                 (", " + self.expr(r[2], scope)[0]) if len(r) == 3 else "")
                if len(r) == 3:
                    self.port("range() with a step: the end bound assumes a positive step")
        else:
            t, _ = self.expr(it, scope)
            sure = it[0] == "list" or it[0] == "call" or (it[0] == "var" and self.origin.get(it[1]) == "list")
            if it[0] in ("member", "index", "id") and self.path_head(it) in ROOTS:
                t = "gs.list(%s)" % t   # a collection proxy is not a Lua sequence
            head = "for _, %s in ipairs(%s) do" % (var, t)
            if not sure:
                trailer = self.port("list or map? (ipairs assumed; use pairs for a map)")
                if st["inline"]:
                    st = dict(st, comment=(st.get("comment") or "") + " " + trailer)
                else:
                    head += " " + trailer
        if one_based:
            self.loop_vars_1based.append(st["var"])
        self.emit_loop_body(st, inner_scope, head, st["inline"])
        if one_based:
            self.loop_vars_1based.pop()

    def only_index_uses(self, body, var):
        """True when every $var use in the body is as an index of a list binding."""
        uses = 0
        index_uses = 0
        for e in iter_all_exprs(body):
            if e[0] == "var" and e[1] == var:
                uses += 1
            if e[0] == "index" and e[2] == ("var", var) and e[1][0] == "var" \
                    and self.origin.get(e[1][1]) == "list":
                index_uses += 1
        return uses > 0 and uses == index_uses

    def minus_one(self, b, scope):
        if b[0] == "num" and not b[3] and b[2] is None:
            return str(int(b[1].replace("_", ""), 0) - 1)
        t, p = self.expr(b, scope)
        if p < ATOM:
            t = "(" + t + ")"
        return t + " - 1"


def merge_origin(old, new):
    if old is None:
        return new
    if old == new:
        return old
    return "unknown"


def collect_names(text, filename, depth=0):
    """def names, top-level let names and whether mac.script is reached, of an
    included file (recursively)."""
    lines = text.split("\n")
    p = StmtParser(lines, filename)
    try:
        stmts, _, _, _ = p.parse_block(False)
    except ConvError:
        stmts = []
    defs = set()
    lets = set()
    mac = False
    refs = set()
    is_mod = os.path.basename(filename) != "test.script" and not any(st["kind"] == "quit" for st in stmts)
    declared = set()
    for e in iter_all_exprs(stmts):
        if e[0] == "call" and e[1][0] == "id":
            refs.add(e[1][1])
        elif e[0] == "var":
            refs.add(e[1])
    for st in walk_stmts(stmts):
        if st["kind"] == "command" and st["head"][0] == "id":
            refs.add(st["head"][1])
        if st["kind"] in ("let", "alias", "command_decl"):
            declared.add(st["name"])
        elif st["kind"] == "for":
            declared.add(st["var"])
        elif st["kind"] == "def":
            declared.update(st["params"])
    for st in walk_stmts(stmts):
        if st["kind"] == "def":
            defs.add(st["name"])
        elif st["kind"] == "let" and st in stmts:
            lets.add(st["name"])
        elif st["kind"] == "include" and depth < 4:
            e = st["expr"]
            if e[0] == "str" and "$" not in e[2]:
                if os.path.basename(e[2]) == "mac.script":
                    mac = True
                    continue
                full = os.path.join(os.path.dirname(os.path.abspath(filename)), e[2])
                if os.path.exists(full):
                    with open(full, encoding="utf-8") as f:
                        d, l, m, r, _ = collect_names(f.read(), full, depth + 1)
                    defs |= d
                    lets |= l
                    refs |= r
                    mac = mac or m
    refs -= defs | declared
    return defs, lets, mac, refs, is_mod


def find_includer_names(path):
    """def and top-level let names of every script that includes `path`
    (searched below the nearest `integration` ancestor, else two levels up)."""
    base = os.path.basename(path)
    d = os.path.dirname(os.path.abspath(path))
    root = d
    for _ in range(4):
        if os.path.basename(root) == "integration" or os.path.dirname(root) == root:
            break
        root = os.path.dirname(root)
    if os.path.basename(root) != "integration":
        root = os.path.dirname(os.path.dirname(d))
    pat = re.compile(r'^\s*include\s+"([^"]*)"')
    defs, lets = set(), set()
    for dp, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".script"):
                continue
            full = os.path.join(dp, fn)
            if os.path.abspath(full) == os.path.abspath(path):
                continue
            try:
                with open(full, encoding="utf-8") as f:
                    text = f.read()
            except OSError:
                continue
            hit = False
            for line in text.split("\n"):
                m = pat.match(line)
                if m and os.path.basename(m.group(1)) == base and \
                        os.path.abspath(os.path.join(dp, m.group(1))) == os.path.abspath(path):
                    hit = True
                    break
            if not hit:
                continue
            dd, ll, _, _, _ = collect_names(text, full, depth=1)
            defs |= dd
            lets |= ll
    return defs, lets


def load_lib_names(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    defs, lets, _, _, _ = collect_names(text, path)
    return defs, lets


def find_lib(script_path):
    """Locate lib/mac.script from the script's include lines or its tree."""
    d = os.path.dirname(os.path.abspath(script_path))
    try:
        with open(script_path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r'\s*include\s+"([^"]*mac\.script)"', line)
                if m:
                    cand = os.path.join(d, m.group(1))
                    if os.path.exists(cand):
                        return cand
    except OSError:
        pass
    cur = d
    for _ in range(6):
        cand = os.path.join(cur, "lib", "mac.script")
        if os.path.exists(cand):
            return cand
        cand = os.path.join(cur, "mac.script")
        if os.path.exists(cand) and os.path.basename(cur) == "lib":
            return cand
        cur = os.path.dirname(cur)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert a shell-v2 .script file to Lua 5.4.")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output file (default: stdout)")
    ap.add_argument("--lib", help="path to lib/mac.script (default: auto-detect)")
    ap.add_argument("--report", action="store_true", help="print PORT markers and notes to stderr")
    ap.add_argument("--module", dest="module", action="store_true", default=None,
                    help="the file is included by others: top-level defs/lets become globals "
                         "(default: auto, when the file is not test.script and has no top-level quit)")
    ap.add_argument("--no-module", dest="module", action="store_false")
    args = ap.parse_args(argv)

    lib = args.lib or find_lib(args.input)
    lib_names = load_lib_names(lib) if lib else (set(), set(MAC_LET_NAMES_FALLBACK))
    with open(args.input, encoding="utf-8") as f:
        text = f.read()
    conv = Converter(args.input, text, lib_path=lib, lib_names=lib_names, module=args.module)
    lua = conv.convert()
    if args.output:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(lua)
    else:
        sys.stdout.write(lua)
    if args.report:
        for line, reason in conv.ports:
            sys.stderr.write("%s:%d: PORT: %s\n" % (args.input, line, reason))
        for line, note in conv.notes:
            sys.stderr.write("%s:%d: note: %s\n" % (args.input, line, note))
        if conv.vars_names:
            sys.stderr.write("%s: vars.* names: %s\n" % (args.input, ", ".join(sorted(conv.vars_names))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
