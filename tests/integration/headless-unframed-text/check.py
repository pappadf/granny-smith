#!/usr/bin/env python3
# Checks the bytes unframed headless wrote for run.sh's two statements.
import sys

out = open(sys.argv[1], "rb").read()
err = open(sys.argv[2], "rb").read()
body = b"[\r][\x08][\x0c][\t][\"][\\][\x01][" + "é😀".encode() + b"]"
fail = []
if b"o" + body + b"\n" not in out:
    fail.append("stdout lacks the echoed text: %r" % out[-300:])
if b"ASSERT FAILED: e" + body + b"\n" not in err:
    fail.append("stderr lacks the error line: %r" % err[-300:])
if fail:
    print("FAIL:\n  " + "\n  ".join(fail))
    sys.exit(1)
print("headless-unframed-text: ok")
