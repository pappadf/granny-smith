#!/usr/bin/env python3
"""test-matrix.py — extract the integration-test coverage matrix.

Statically parses tests/integration/*/config.mk and test.script (plus any
*.sh TEST_RUNNER scripts) and emits:

  - a per-test inventory table   (--tests)
  - a machine x system pivot     (--pivot, default)
  - a machine x video pivot      (--video)

as GitHub-flavored markdown.  Scripts are segmented at `machine.boot`
statements so multi-boot matrix tests attribute media/video to the machine
that actually boots them.  Heuristic by design: once suites self-report
coverage rows at runtime (proposal-integration-test-rework.md §5.6), that
JSONL replaces the script-side guesswork here; the pivots and rendering
stay.

Usage:  scripts/test-matrix.py [--tests] [--video] [--pivot] [tests/integration]
"""

import re
import sys
from pathlib import Path

MACHINES = {"plus", "se30", "iix", "iicx", "iici", "iisi", "iifx",
            "q700", "q900", "q950", "lisa", "macxl"}

# first entry of each ROM's compatible-model list (src/core/memory/rom.c)
ROM_MACHINE = [
    ("plus-", "plus"), ("iix-iicx-se30", "se30"), ("iici-", "iici"),
    ("iisi-", "iisi"), ("iifx-", "iifx"), ("q700-q900", "q700"),
    ("q950-", "q950"), ("lisa2-", "lisa"), ("macxl-", "macxl"),
]

# media path fragment -> system-software label
MEDIA_SYSTEM = [
    ("SSW-2.0", "2.0"), ("SSW-3.2", "3.2"), ("SSW-4.2", "4.2"),
    ("SSW-6.08", "6.0.8"), ("SSW-7.0", "7.0"), ("SSW-7.1", "7.1"),
    ("SSW-7.5", "7.5"), ("SSW-7.6", "7.6"),
    ("System_2_0", "2.0.1"), ("System_3_0", "3.0"), ("System_3_2", "3.2"),
    ("System_3_3", "3.3"), ("System_4_0", "4.0"), ("System_4_1", "4.1"),
    ("System_4_2", "4.2"), ("System_4_3", "4.3"), ("System_6_0_0", "6.0"),
    ("System_6_0_3", "6.0.3"), ("System_6_0_5", "6.0.5"),
    ("System_6_0_8", "6.0.8"), ("System_7_0_1", "7.0.1"),
    ("System_7_1_0", "7.1"),
    ("system_6_0_8", "6.0.8"), ("system_7_1", "7.1"),
    ("hd1.", "6.x (hd1)"),
    ("aux", "A/UX 3.0.1"), ("AUX", "A/UX 3.0.1"),
    ("LOS-3.1", "LOS 3.1"), ("Xenix", "Xenix 3.0"),
    ("MacWorks", "MacWorks 3.0"),
    ("MacTest", "app:MacTest"), ("Marathon", "app:Marathon"),
    ("marathon", "app:Marathon"), ("MusicWorks", "app:MusicWorks"),
    ("Norton", "app:Norton"),
]

NAMED_ARG = re.compile(r'(\w+)=("[^"]*"|\'[^\']*\'|\S+)')
BAD_VALUES = {"bogus", "nonesuch", "none", ""}


def unquote(v):
    return v.strip('"\'').rstrip("),")


def classify_media(path):
    for frag, label in MEDIA_SYSTEM:
        if frag in path:
            return label
    return None


class Test:
    """One test directory.  Facts are (machine, value) pairs so multi-boot
    scripts attribute each row to the machine in effect at that line."""

    def __init__(self, name, default_machine):
        self.name = name
        self.default_machine = default_machine or "?"
        self.machine = self.default_machine     # current segment context
        self.ms = set()      # (machine, system)
        self.mv = set()      # (machine, video mode/sense/custom)
        self.mc = set()      # (machine, card)
        self.rams = set()
        self.resolutions = set()
        self.goldens = 0
        self.sound_goldens = 0
        self.cycles = 0
        self.runner = False

    @property
    def machines(self):
        found = {m for m, _ in self.ms | self.mv | self.mc}
        return found or {self.default_machine}

    def systems(self):
        return {s for _, s in self.ms}

    def video(self):
        return {v for _, v in self.mv | self.mc}

    def note_media(self, path, kind):
        s = classify_media(path)
        if s:
            self.ms.add((self.machine, s))

    def note_args(self, text, is_boot):
        args = dict((k, unquote(v)) for k, v in NAMED_ARG.findall(text))
        model = args.get("model")
        if is_boot:
            # new segment: model= switches context, else default persists
            if model in MACHINES:
                self.machine = model
            elif model:            # rejected-boot validation (boot-config)
                return
        elif model in MACHINES:
            self.machine = model
        for key, v in args.items():
            if v in BAD_VALUES:
                continue
            if key == "ram" and v.isdigit():
                self.rams.add(v)
            elif key in ("video_card", "card_id"):
                self.mc.add((self.machine, v))
            elif key == "video_mode":
                self.mv.add((self.machine, v))
            elif key == "video_sense" and v.isdigit():
                self.mv.add((self.machine, f"sense={v}"))
            elif key == "custom_mode":
                self.mv.add((self.machine, f"custom={v}"))
            elif key in ("fd", "fd0", "fd1"):
                self.note_media(v, "fd")
            elif key == "hd":
                self.note_media(v, "hd")
            elif key == "cdrom":
                self.note_media(v, "cd")


def parse_script(test, text):
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        if not line.strip() or "try(" in line:
            continue
        if "machine.boot" in line:
            test.note_args(line, is_boot=True)
            continue
        m = re.search(r'\.insert\s+("[^"]+"|\S+)', line)
        if m:
            test.note_media(unquote(m.group(1)), "fd")
        m = re.search(r'attach_hd\s+("[^"]+"|\S+)', line)
        if m:
            test.note_media(unquote(m.group(1)), "hd")
        if re.search(r'\bscreen\.match\b', line):
            test.goldens += 1
        if re.search(r'\bsound\.match\b', line):
            test.sound_goldens += 1
        m = re.search(r'scheduler\.run\s+(\d+)', line)
        if m:
            test.cycles += int(m.group(1))
        mw = re.search(r'screen\.width\s*==\s*(\d+)', line)
        if mw:
            test.resolutions.add((test.machine, mw.group(1)))


def parse_test(d):
    cfg = (d / "config.mk").read_text(errors="replace")
    rom = ""
    fields = {}
    for line in cfg.splitlines():
        m = re.match(r'\s*TEST_(ROM|ARGS|SETUP|RUNNER)\s*:?=\s*(.*)', line)
        if m:
            fields[m.group(1)] = m.group(2)
            if m.group(1) == "ROM":
                rom = m.group(2).strip()
    default = next((mach for frag, mach in ROM_MACHINE if frag in rom), None)
    # model= in TEST_ARGS overrides the ROM-derived default
    args_model = re.search(r'model=(\S+)', fields.get("ARGS", ""))
    if args_model and unquote(args_model.group(1)) in MACHINES:
        default = unquote(args_model.group(1))
    test = Test(d.name, default)
    test.runner = "RUNNER" in fields
    test.note_args(fields.get("ARGS", ""), is_boot=False)
    if "SETUP" in fields:
        s = classify_media(fields["SETUP"])
        if s:
            test.ms.add((test.machine, s))
    for script in sorted(d.glob("*.script")) + sorted(d.glob("*.sh")):
        test.machine = test.default_machine     # each file starts fresh
        parse_script(test, script.read_text(errors="replace"))
    return test


def fmt(s, empty="—"):
    return ", ".join(sorted(s)) if s else empty


def emit_tests(tests):
    print("| test | machine | system | video | Mcycles | goldens |")
    print("|---|---|---|---|---|---|")
    for t in tests:
        g = f"{t.goldens}px" + (f"+{t.sound_goldens}wav" if t.sound_goldens else "")
        print(f"| {t.name} | {fmt(t.machines)} | {fmt(t.systems())} "
              f"| {fmt(t.video())} | {t.cycles // 1_000_000} | {g} |")


def emit_pivot(tests, pairs_of, title):
    pair_tests = {}
    for t in tests:
        for pair in pairs_of(t):
            pair_tests.setdefault(pair, []).append(t.name)
    rows = sorted({m for m, _ in pair_tests})
    cols = sorted({c for _, c in pair_tests})
    print(f"\n### {title}\n")
    print("| " + " | ".join([""] + cols) + " |")
    print("|" + "---|" * (len(cols) + 1))
    for r in rows:
        cells = [str(len(pair_tests[(r, c)])) if (r, c) in pair_tests else "·"
                 for c in cols]
        print("| " + " | ".join([r] + cells) + " |")


def main():
    argv = sys.argv[1:]
    flags = {a for a in argv if a.startswith("--")}
    paths = [a for a in argv if not a.startswith("--")]
    root = Path(paths[0]) if paths else Path("tests/integration")
    tests = [parse_test(d) for d in sorted(root.iterdir())
             if (d / "config.mk").exists() and (d / "test.script").exists()]
    if "--tests" in flags:
        emit_tests(tests)
    if "--video" in flags:
        emit_pivot(tests, lambda t: t.mv | t.mc,
                   "machine x video card/mode (test count)")
    if "--pivot" in flags or not flags:
        emit_pivot(tests, lambda t: t.ms, "machine x system (test count)")


if __name__ == "__main__":
    main()
