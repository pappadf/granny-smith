#!/usr/bin/env python3
# Checks the bytes unframed headless wrote for run.sh's statements, in one
# job mode (argv[1]: threaded | inline).  Both modes are held to the same
# expectations, byte for byte.
import sys

mode = sys.argv[1]
out = open(sys.argv[2], "rb").read()
err = open(sys.argv[3], "rb").read()
body = b"[\r][\x08][\x0c][\t][\"][\\][\x01][" + "é😀".encode() + b"]"
fail = []
if b"o" + body + b"\n" not in out:
    fail.append("stdout lacks the echoed text: %r" % out[-300:])
if b"ASSERT FAILED: e" + body + b"\n" not in err:
    fail.append("stderr lacks the error line: %r" % err[-300:])
# A deferred answer's error is decoded from its {"error": ...} document, not
# cut out of it with its escapes left in -- and is the I/O work's own reason
# whether the leaf ran on the worker or there and then (inline mode once
# printed "files.cp: failed").
cp_line = b"line 1: cp: cannot create '/nonexistent-gs-dir/q\"d\\e/out': No such file or directory\n"
if cp_line not in err:
    fail.append("stderr lacks the decoded deferred error line: %r" % err[-300:])
if fail:
    print("FAIL (%s):\n  " % mode + "\n  ".join(fail))
    sys.exit(1)
print("headless-unframed-text (%s): ok" % mode)
