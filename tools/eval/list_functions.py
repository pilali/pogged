#!/usr/bin/env python3
"""Function inventory for the Pogged DSP core.

Walks the C/C++ sources and emits every function and method it finds, grouped
by file and (for methods) by class, with the line number and whether the
definition sits behind a preprocessor conditional. No external dependencies —
ctags is not installed on the MOD / Buildroot toolchains, and the audit has to
run anywhere `make` does.

The parser is deliberately conservative: it recognises a definition by a
declarator followed by `(`, a matching `)`, an optional trailing specifier run
(`const`, `noexcept`, `override`, ...) and then `{`. Declarations (`;`), calls
and control-flow keywords are excluded. It is a documentation aid, not a
compiler front end — `make eval` reports the count so a silent parser
regression shows up as a jump in the total.

Usage:
    tools/eval/list_functions.py [--format md|text] [paths...]
"""

import argparse
import os
import re
import sys

DEFAULT_PATHS = ["src", "juce"]

# Keywords that can be followed by `(` ... `)` `{` without being a function.
KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "else", "do",
    "sizeof", "alignof", "static_assert", "decltype", "noexcept", "throw",
    "constexpr", "and", "or", "not",
}

# `name(` at the start of a declarator, with an optional `Class::` qualifier.
FUNC_RE = re.compile(
    r"""^[ \t]*                       # leading indent only: no expressions
        (?P<sig>
          (?:[A-Za-z_~][\w:<>,\ \*&\[\]]*?[\ \*&])?   # return type (may be absent)
          (?P<name>(?:[A-Za-z_]\w*::)?~?[A-Za-z_]\w*) # (Class::)name
        )
        [ \t]*\(""",
    re.VERBOSE,
)

CLASS_RE = re.compile(r"^\s*(?:template\s*<.*>\s*)?(?:class|struct)\s+(\w+)")
COND_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")


def strip_comments(lines):
    """Blank out // and /* */ comment bodies, keeping line numbering intact."""
    out, in_block = [], False
    for line in lines:
        buf, i, n = [], 0, len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = n
                else:
                    in_block = False
                    i = end + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def balanced(text, open_ch="(", close_ch=")"):
    """Index just past the paren matching the first one in `text`, or -1."""
    depth = 0
    for i, ch in enumerate(text):
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


SPECIFIER_RE = re.compile(
    r"^(const|volatile|noexcept|override|final|mutable|&&|&|"
    r"->\s*[\w:<>,\*&\[\]\ ]+)")


def strip_specifiers(text):
    """Drop the trailing specifier run after a parameter list.

    Only a fixed whitelist is consumed, so a stray `)` — the signature of a
    CALL nested inside a condition, not a definition — stops the walk and the
    caller rejects the match.
    """
    text = text.lstrip()
    while True:
        m = SPECIFIER_RE.match(text)
        if not m:
            return text
        text = text[m.end():].lstrip()
        if text.startswith("("):        # noexcept(expr), attribute payload
            close = balanced(text)
            if close < 0:
                return text
            text = text[close:].lstrip()


def scan(path):
    """Yield (line_no, class_or_None, name, guards) for each definition."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        raw = fh.readlines()
    code = strip_comments(raw)

    # Track the enclosing class by brace depth, and the #if guard stack.
    depth = 0
    class_stack = []          # (name, depth_at_open)
    guard_stack = []
    pending_class = None

    for idx, line in enumerate(code):
        cond = COND_RE.match(line)
        if cond:
            kind, rest = cond.group(1), cond.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                guard_stack.append(f"#{kind} {rest}".strip())
            elif kind in ("elif", "else"):
                if guard_stack:
                    guard_stack[-1] = f"#{kind} {rest}".strip()
            elif kind == "endif" and guard_stack:
                guard_stack.pop()
            continue

        cls = CLASS_RE.match(line)
        if cls and "{" not in line.split(cls.group(1), 1)[0]:
            pending_class = cls.group(1)

        hit = None
        m = FUNC_RE.match(line)
        if m and m.group("name") not in KEYWORDS:
            # The joined text from `(` lets a signature span several lines.
            tail = "".join(code[idx:idx + 12])[m.end() - 1:]
            close = balanced(tail)
            if close > 0:
                after = strip_specifiers(tail[close:])
                # `{` = a body; `:` (not `::`) = a constructor's member-init
                # list, which is still a definition.
                ctor_init = after.startswith(":") and not after.startswith("::")
                if after.startswith("{") or ctor_init:
                    hit = m.group("name")
            # A member-init list's own entries look exactly like a definition
            # (`member(args)` then `{`). The give-away is the previous code
            # line ending in `,` or `:` — a real definition never follows one.
            if hit and idx > 0:
                prev = next((c.rstrip() for c in reversed(code[:idx]) if c.strip()), "")
                if prev.endswith((",", ":")) and not prev.endswith("::"):
                    hit = None

        if hit:
            cur = class_stack[-1][0] if class_stack else None
            if "::" in hit:
                cur, hit = hit.split("::", 1)
            yield idx + 1, cur, hit, list(guard_stack)

        opens = line.count("{")
        closes = line.count("}")
        if opens and pending_class is not None:
            class_stack.append((pending_class, depth))
            pending_class = None
        depth += opens - closes
        while class_stack and depth <= class_stack[-1][1]:
            class_stack.pop()


def collect(paths):
    files = []
    for root in paths:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in sorted(filenames):
                if fn.endswith((".c", ".cc", ".cpp", ".h", ".hpp")):
                    files.append(os.path.join(dirpath, fn))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", default=DEFAULT_PATHS)
    ap.add_argument("--format", choices=("md", "text"), default="text")
    args = ap.parse_args()

    total = 0
    md = args.format == "md"
    if md:
        print("# Function inventory\n")
        print("Generated by `tools/eval/list_functions.py` — do not edit by hand.\n")

    for path in collect(args.paths or DEFAULT_PATHS):
        entries = list(scan(path))
        if not entries:
            continue
        total += len(entries)
        print(f"\n## {path}  ({len(entries)})\n" if md else f"\n{path}  ({len(entries)})")
        if md:
            print("| Line | Scope | Function | Guard |")
            print("|---:|---|---|---|")
        last_cls = object()
        for line_no, cls, name, guards in entries:
            guard = guards[-1] if guards else ""
            if md:
                print(f"| {line_no} | {cls or '—'} | `{name}` | "
                      f"{'`' + guard + '`' if guard else ''} |")
            else:
                if cls != last_cls:
                    print(f"  [{cls}]" if cls else "  [free functions]")
                    last_cls = cls
                suffix = f"   <{guard}>" if guard else ""
                print(f"    {line_no:5d}  {name}{suffix}")

    print(f"\n{'**' if md else ''}Total: {total} definitions{'**' if md else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
