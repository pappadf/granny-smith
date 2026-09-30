#!/usr/bin/env python3
# Checks the --framed record stream run.sh captured.
import json, sys

out_path, err_path = sys.argv[1], sys.argv[2]
jobs = {}  # id -> [(kind, record)]
for line in open(out_path, encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    for kind in ("out", "value_begin", "value", "error"):
        tag = "@" + kind + " "
        if line.startswith(tag):
            rec = json.loads(line[len(tag):])
            jobs.setdefault(rec["id"], []).append((kind, rec))
stderr = open(err_path, encoding="utf-8", errors="replace").read().splitlines()
fail = []

def need(cond, msg):
    if not cond:
        fail.append(msg)

ids = sorted(jobs)
need(len(ids) >= 5, "expected at least five jobs, got %d" % len(ids))

# 1. The block: text, a value, text again -- in that order, the value's text
#    exactly between its markers.
block = jobs[ids[0]]
kinds = [k for k, _ in block]
need(kinds.count("value_begin") == kinds.count("value"), "unbalanced value markers: %s" % kinds)
text, inside, values = "", None, []
for k, r in block:
    if k == "value_begin":
        inside = ""
    elif k == "value":
        values.append((inside, r))
        inside = None
    elif k == "out":
        if inside is not None:
            inside += r["text"]
        else:
            text += r["text"]
pcs = [v for v in values if v[1].get("json") == v[0].strip()]
need(any(t == "0x4080002a\n" for t, _ in values), "the pc value's text is not between its markers: %r" % values)
first_out = next(i for i, (k, _) in enumerate(block) if k == "out")
need("before" in block[first_out][1]["text"], "text before the value comes first")
need(block[-1][0] in ("value", "out"), "stream ends with the last statement")

# 2. An assert failure: one error record, and nothing on stderr (the core
#    writes a job's error once, as the record).
errs = [r for i in ids for k, r in jobs[i] if k == "error"]
need(len(errs) == 1, "expected one error record, got %d" % len(errs))
if errs:
    need(errs[0]["lines"] == ["ASSERT FAILED: boom"], "error lines %r" % errs[0]["lines"])
    need(not any(l in stderr for l in errs[0]["lines"]), "the error is not written to stderr as well")
    need(errs[0]["message"] == "ASSERT FAILED: boom", "error message")

# 3. A 256 KiB string: its value record is the reduced form.
big = [r for i in ids for k, r in jobs[i] if k == "value" and r.get("truncated")]
need(len(big) == 1, "expected one reduced value record, got %d" % len(big))

# 4. 64 KiB of control bytes: all of it arrives, then the next job runs.
cat_text = "".join(r["text"] for i in ids for k, r in jobs[i] if k == "out" and "\x01" in r["text"])
need(cat_text.count("\x01") == 65536, "control-byte output: %d of 65536 bytes" % cat_text.count("\x01"))
need(any(k == "out" and r["text"] == "done\n" for i in ids for k, r in jobs[i]), "the job after files.cat ran")

if fail:
    print("FAIL:\n  " + "\n  ".join(fail))
    sys.exit(1)
print("job-annotations: ok (%d jobs)" % len(ids))
