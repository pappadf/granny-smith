#!/usr/bin/env python3
"""Socket and stdin probes for the headless daemon's statement streaming."""

import os
import random
import socket
import subprocess
import sys
import time

HEADLESS, ROM, RESULTS = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


def start_daemon():
    for _ in range(20):
        port = random.randint(20000, 60000)
        log = open(os.path.join(RESULTS, "daemon.log"), "w")
        proc = subprocess.Popen([HEADLESS, "--daemon", f"--port={port}", f"rom={ROM}", "--speed=max"],
                                stdout=subprocess.PIPE, stderr=log, text=True)
        line = proc.stdout.readline()
        while line and "READY" not in line:
            line = proc.stdout.readline()
        if "READY" in line:
            return proc, port
        proc.kill()
    sys.exit("daemon did not start")


def session(port, sends, gap=0.0, half_close=True, timeout=20.0):
    """Send each chunk (gap seconds apart), optionally half-close, read to EOF."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    for i, chunk in enumerate(sends):
        if i and gap:
            time.sleep(gap)
        s.sendall(chunk.encode())
    if half_close:
        s.shutdown(socket.SHUT_WR)
    out = b""
    try:
        while True:
            data = s.recv(65536)
            if not data:
                break
            out += data
    except socket.timeout:
        out += b"\n<timeout>"
    s.close()
    return out.decode(errors="replace")


proc, port = start_daemon()
try:
    # 60 statements in one 2.4 KB send: all run (the old read stopped at 2 KB).
    stmts = "".join(f'echo "marker-{i:02d}-{"x" * 20}"\n' for i in range(60))
    check(len(stmts) > 2048, "the batch is over 2 KB")
    out = session(port, [stmts])
    check(all(f"marker-{i:02d}-" in out for i in range(60)), "60 statements in one send all run")

    # Two sends 50 ms apart: both run (the old read stopped at the first).
    out = session(port, ['echo "first-send"\n', 'echo "second-send"\n'], gap=0.05)
    check("first-send" in out and "second-send" in out, "two sends 50 ms apart both run")

    # A statement split mid-line across sends is not dispatched half-read.
    out = session(port, ['echo "split-', 'statement"\n'], gap=0.05)
    check("split-statement" in out, "a statement split across sends runs whole")

    # Pipelined: the run, then a read, in one send -- the second after the first.
    out = session(port, ['scheduler.run 5000000\nlet c = machine.cpu.instr_count\necho "after-run=${$c}"\n'])
    got = [l for l in out.splitlines() if l.startswith("after-run=")]
    check(bool(got) and int(got[0].split("=")[1]) >= 5000000, "a pipelined read runs after the run finishes")

    # A half-closing client's run completes (EOF used to cancel it).
    out = session(port, ['scheduler.run 20000000\nlet c = machine.cpu.instr_count\necho "half-closed=${$c}"\n'])
    check("half-closed=" in out, "a half-closing client's scheduler.run completes")

    # An unterminated block at end of input is reported, not dropped.
    out = session(port, ['if true {\necho "never"\n'])
    check("incomplete block" in out and "never" not in out, "an unterminated block is reported")

    # A client that disconnects mid-run cancels it.
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"scheduler.run 4000000000\n")
    time.sleep(0.5)
    s.close()
    deadline = time.time() + 10
    stopped = False
    while time.time() < deadline:
        out = session(port, ['let r = scheduler.running\necho "running=${$r}"\n'], timeout=5.0)
        if "running=false" in out:
            stopped = True
            break
        time.sleep(0.5)
    check(stopped, "a client that disconnects mid-run cancels it")

    # The cancellation must not leak: the run above was a top-level
    # statement, so nothing consumed the interrupt it raised, and the first
    # loop on the next connection aborted at once with "interrupted" (#171).
    out = session(port, ['let n = 0\nwhile $n < 3 {\nscheduler.run 100000\n$n = $n + 1\n}\necho "n=${$n}"\n'])
    check("n=3" in out and "interrupted" not in out, "a cancelled run does not interrupt the next connection's loop")
finally:
    proc.kill()
    proc.wait()

# stdin: an 1100-character line is one statement (a 1024-byte fgets split it).
long_text = "y" * 1100
script = f'echo "{long_text}"\nquit\n'
res = subprocess.run([HEADLESS, f"rom={ROM}", "--script-stdin"], input=script, capture_output=True, text=True,
                     timeout=60)
lines = res.stdout.splitlines()
check(any(l == long_text for l in lines), "an 1100-character stdin line is one statement")

print("daemon-statements: " + ("FAIL" if failures else "all probes passed"))
sys.exit(1 if failures else 0)
