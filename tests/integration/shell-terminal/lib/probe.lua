-- A module the test requires: relative to the requiring file.
local M = {}
function M.double(x) return x * 2 end
M.loaded_from = "lib/probe.lua"
return M
