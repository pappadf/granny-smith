#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# migrate_shell_v2.py — mechanical v1 → v2 shell-script migration
# (proposal-shell-control-flow-and-functions.md §9).
#
# Rules applied per script:
#   1. `NAME = ${expr}` / `NAME = literal`  →  `let NAME = expr'`
#   2. `assert ${pred} rest`                →  `assert pred' rest'`
#   3. whole-token `${expr}` command args:
#        no :fmt  →  `(expr')`
#        :fmt     →  `"${expr':fmt}"`
#   4. partial-token `${…}` args (`${WORK_DIR}/x`) → quoted string,
#      `${VAR}` collapsed to `$VAR` for known variables
#   5. inside dq strings: `${VAR…}` bodies get `$` on known variables
#   6. `$XXXX` hex literals (leading digit) → `0xXXXX`
#
# Known variables = names assigned in the script itself plus the
# harness-provided WORK_DIR / TMP_DIR.
#
# Usage: migrate_shell_v2.py FILE...   (rewrites in place)

import re
import sys

HARNESS_VARS = {"WORK_DIR", "TMP_DIR"}
IDENT = re.compile(r"[A-Za-z_]\w*")


def find_matching_brace(s: str, i: int) -> int:
    """s[i] == '{'; return index of matching '}' (or -1). Quote-aware."""
    depth = 0
    j = i
    while j < len(s):
        c = s[j]
        if c in "\"'":
            q = c
            j += 1
            while j < len(s) and s[j] != q:
                j += 2 if s[j] == "\\" else 1
            j += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def split_fmt(body: str):
    """Split `expr:fmt` at the rightmost top-level colon (v1 rule)."""
    depth = 0
    in_str = False
    last = -1
    i = 0
    while i < len(body):
        c = body[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ":" and depth == 0:
            last = i
        i += 1
    if last < 0:
        return body, None
    return body[:last], body[last + 1 :]


def rewrite_expr(expr: str, known: set) -> str:
    """Prefix `$` on bare identifiers that are known variables; recurse
    into nested dq strings' ${...} regions."""
    out = []
    i = 0
    n = len(expr)
    while i < n:
        c = expr[i]
        if c == '"':
            # nested string: copy, but rewrite ${...} bodies inside it
            j = i + 1
            out.append('"')
            while j < n and expr[j] != '"':
                if expr[j] == "\\":
                    out.append(expr[j : j + 2])
                    j += 2
                    continue
                if expr.startswith("${", j):
                    k = find_matching_brace(expr, j + 1)
                    if k < 0:
                        out.append(expr[j:])
                        j = n
                        break
                    inner, fmt = split_fmt(expr[j + 2 : k])
                    rw = rewrite_expr(inner, known)
                    out.append("${" + rw + (":" + fmt if fmt else "") + "}")
                    j = k + 1
                    continue
                out.append(expr[j])
                j += 1
            if j < n:
                out.append('"')
                j += 1
            i = j
            continue
        if c == "$":
            # $name (alias/binding) or v1 $HEX — leave; hex handled later
            m = IDENT.match(expr, i + 1)
            out.append(expr[i : (m.end() if m else i + 1)])
            i = m.end() if m else i + 1
            continue
        m = IDENT.match(expr, i)
        if m and (i == 0 or expr[i - 1] not in "$."):
            word = m.group(0)
            if word in known:
                out.append("$" + word)
            else:
                out.append(word)
            i = m.end()
            continue
        out.append(c)
        i += 1
    return "".join(out)


def hexify(text: str) -> str:
    """`$4170` (leading digit) → `0x4170`, outside strings."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
            continue
        m = re.match(r"\$([0-9][0-9A-Fa-f]*)\b", text[i:])
        if m:
            out.append("0x" + m.group(1))
            i += m.end()
            continue
        out.append(c)
        i += 1
    return "".join(out)


def simplify_dollar_brace(s: str) -> str:
    """`${$name}` → `$name` (post-pass cosmetic cleanup inside strings)."""
    return re.sub(r"\$\{\$([A-Za-z_]\w*)\}", r"$\1", s)


def tokenise(line: str):
    """Split a command line into whitespace tokens, keeping quoted and
    ${...} regions intact. Returns list of (token, start, end)."""
    toks = []
    i = 0
    n = len(line)
    while i < n:
        while i < n and line[i] in " \t":
            i += 1
        if i >= n:
            break
        start = i
        while i < n and line[i] not in " \t":
            c = line[i]
            if c in "\"'":
                q = c
                i += 1
                while i < n and line[i] != q:
                    i += 2 if line[i] == "\\" else 1
                i += 1
                continue
            if line.startswith("${", i):
                k = find_matching_brace(line, i + 1)
                i = (k + 1) if k >= 0 else n
                continue
            i += 1
        toks.append((line[start:i], start, i))
    return toks


def migrate_token(tok: str, known: set, ctx: str) -> str:
    """Rewrite one command-argument token."""
    if tok.startswith('"') or tok.startswith("'"):
        # quoted string: rewrite ${...} bodies inside
        return simplify_dollar_brace(rewrite_expr_in_string(tok, known))
    if tok.startswith("${") and tok.endswith("}") and find_matching_brace(tok, 1) == len(tok) - 1:
        body, fmt = split_fmt(tok[2:-1])
        rw = rewrite_expr(body, known)
        if fmt:
            return '"${' + rw + ":" + fmt + '}"'
        if ctx == "echo":
            return '"${' + rw + '}"'
        return "(" + rw + ")"
    if "${" in tok:
        # partial token (e.g. ${WORK_DIR}/x) → quote it
        rebuilt = []
        i = 0
        while i < len(tok):
            if tok.startswith("${", i):
                k = find_matching_brace(tok, i + 1)
                if k < 0:
                    rebuilt.append(tok[i:])
                    break
                body, fmt = split_fmt(tok[i + 2 : k])
                rw = rewrite_expr(body, known)
                rebuilt.append("${" + rw + (":" + fmt if fmt else "") + "}")
                i = k + 1
            else:
                rebuilt.append(tok[i])
                i += 1
        return simplify_dollar_brace('"' + "".join(rebuilt) + '"')
    return hexify(tok)


def rewrite_expr_in_string(tok: str, known: set) -> str:
    """Rewrite ${...} bodies inside a quoted token."""
    if not tok.startswith('"'):
        return tok
    out = ['"']
    i = 1
    n = len(tok)
    while i < n:
        if tok[i] == "\\":
            out.append(tok[i : i + 2])
            i += 2
            continue
        if tok.startswith("${", i):
            k = find_matching_brace(tok, i + 1)
            if k < 0:
                out.append(tok[i:])
                break
            body, fmt = split_fmt(tok[i + 2 : k])
            rw = rewrite_expr(body, known)
            out.append("${" + rw + (":" + fmt if fmt else "") + "}")
            i = k + 1
            continue
        out.append(tok[i])
        i += 1
    return "".join(out)


def migrate_line(line: str, known: set) -> str:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return line

    indent = line[: len(line) - len(line.lstrip())]
    # Split trailing comment (quote-aware)
    code = stripped
    comment = ""
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n and code[i] != q:
                i += 2 if code[i] == "\\" else 1
            i += 1
            continue
        if code.startswith("${", i):
            k = find_matching_brace(code, i + 1)
            i = (k + 1) if k >= 0 else n
            continue
        if c == "#":
            comment = "  " + code[i:]
            code = code[:i].rstrip()
            break
        i += 1

    # Rule 1: bare-name assignment → let
    m = re.match(r"^([A-Za-z_]\w*)\s*=\s*(.+)$", code)
    if m and "." not in m.group(1) and not code.startswith(("machine", "scheduler", "debug", "appletalk")):
        name, rhs = m.group(1), m.group(2)
        known.add(name)
        if rhs.startswith("${") and rhs.endswith("}"):
            body, _ = split_fmt(rhs[2:-1])
            rhs = rewrite_expr(body, known)
        else:
            rhs = migrate_token(rhs, known, "let")
        return f"{indent}let {name} = {rhs}{comment}\n"

    # Rule 2: assert
    m = re.match(r"^assert\s+(.*)$", code)
    if m:
        rest = m.group(1)
        if rest.startswith("${"):
            k = find_matching_brace(rest, 1)
            if k >= 0:
                pred = rewrite_expr(rest[2:k], known)
                tail = rest[k + 1 :].strip()
                tail = " " + rewrite_expr_in_string(tail, known) if tail else ""
                return f"{indent}assert {pred}{tail}{comment}\n"
        # predicate not wrapped — still rewrite tokens/strings
        toks = tokenise(rest)
        parts = [migrate_token(t, known, "assert") for t, _, _ in toks]
        return f"{indent}assert {' '.join(parts)}{comment}\n"

    # General command line: rewrite tokens
    toks = tokenise(code)
    if not toks:
        return line
    head = toks[0][0]
    parts = [head] + [migrate_token(t, known, "echo" if head == "echo" else "cmd") for t, _, _ in toks[1:]]
    return f"{indent}{' '.join(parts)}{comment}\n"


def migrate_file(path: str) -> None:
    with open(path) as f:
        lines = f.readlines()
    known = set(HARNESS_VARS)
    # Pre-pass: collect assigned names so uses before/after all rewrite
    for line in lines:
        s = line.strip()
        m = re.match(r"^([A-Za-z_]\w*)\s*=\s*", s)
        if m and "." not in m.group(1) and not s.startswith("#"):
            known.add(m.group(1))
    out = [migrate_line(line, known) for line in lines]
    with open(path, "w") as f:
        f.writelines(out)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        migrate_file(p)
        print(f"migrated {p}")
