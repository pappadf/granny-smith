-- plus-moof-weak-bits, step 1: the System 6.0.8 disk as a MOOF, laid down
-- by the emulator's own writer (files.convert to a .moof name).
assert(files.convert(vars.SRC, vars.DST), "files.convert to .moof failed")
quit()
