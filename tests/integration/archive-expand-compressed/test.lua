-- Converted from test.script by scripts/script-to-lua.py
-- A disk image inside an archive is offered (expandable) whether the member
-- is stored or compressed: the listing decodes a compressed member of up to
-- 64 MB to find out.  It used to probe only a stored one, so a disk in a
-- deflated zip member or in a .sit.hqx was never offered.

-- The one entry's expandable flag, named `name`, in the listing of `dir`.
local function expandable(dir, name)
    local flag = "missing"
    for _, e in ipairs(files.list(dir)) do
        if e.name == name then
            flag = F"${e.expandable}"
        end
    end
    return flag
end

local w = vars.WORK_DIR
assert(expandable(F"${w}/stored.zip", "System.dsk") == "true", "a stored zip member is not offered")
assert(expandable(F"${w}/deflated.zip", "System.dsk") == "true", "a deflated zip member is not offered")
assert(files.list(F"${w}/deflated.zip/System.dsk/partition1/System Folder"), "the deflated member's HFS volume")

-- BinHex around StuffIt around the disk: the BinHex layer is peeled, the
-- StuffIt archive listed, and its compressed member probed.
assert(expandable(F"${w}/mactest.sit.hqx", "MacTest Disk.image") == "true", "a disk in a .sit.hqx is not offered")
assert(files.list(F"${w}/mactest.sit.hqx/MacTest Disk.image/partition1"), "the .sit.hqx disk's volume")

-- Over the cap the member is not decoded for a listing, though it is a disk
-- and lists when asked.
assert(expandable(F"${w}/big.zip", "Big.dsk") == "false", "a 65 MB compressed member was probed")
assert(files.list(F"${w}/big.zip/Big.dsk/partition1"), "the large member does not list when asked")

echo("archive-expand-compressed-ok")
quit()
