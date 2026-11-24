-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: a bare UFS volume mounts through the image VFS.
-- The runner cd's into tests/integration/; WORK_DIR is absolute.

local bare = F"${vars.WORK_DIR}/aux-root.ufs"
local disk = "../../data/aux/aux_3.0.1/APPLE_AUX_3-0-1_RETAIL.iso"

-- No partition map: one synthetic partition, as for a bare HFS floppy.
-- Unwired, the volume was "not a recognised image" (ENOTDIR).
assert(#files.list(bare) == 1, "a bare UFS volume must expose one partition")
echo(F"cached mounts: ${files.mounts.count}")

-- Its root is the same directory as partition 6 of the CD it came from.
local from_bare = files.list(F"${bare}/partition1")
local from_disk = files.list(F"${disk}/partition6")
assert(#from_bare > 0, "empty root on the bare volume")
assert(#from_bare == #from_disk, "bare and in-disk roots differ in size")
for i = 0, #from_disk - 1 do
    assert(from_bare[i + 1].name == from_disk[i + 1].name, F"root entry ${i} differs")
end

-- And files read through it.
assert(files.path_exists(F"${bare}/partition1/etc/passwd"), "etc/passwd missing on the bare volume")
files.cat(F"${bare}/partition1/etc/motd")

quit()
