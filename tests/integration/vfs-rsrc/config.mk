# Integration test: synthetic /rsrc/<TYPE>/<id> path tree (PR 1 of the
# resource-fork-as-VFS-tree proposal).  Exercises type enumeration, id
# enumeration with .info sidecars, the _raw escape hatch, and the
# round-trip of resource bytes reassembled from the per-resource files.

TEST_NAME := VFS resource-fork tree
TEST_DESC := Walk /rsrc/<TYPE>/<id> and .info sidecars on a System 6 floppy
TEST_ROM := roms/Plus_v3.rom
