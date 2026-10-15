-- SPDX-License-Identifier: MIT
-- Copyright (c) pappadf
-- gs_lua_lib.lua -- the script standard library's Lua half, run once when
-- the state is made (gs_lua.c).  The C half made `gs` (bytes, node, fmt,
-- path, is_node, none, quit_signal, interrupt_signal), `echo`, `print`,
-- `quit` and `vars`.  docs/internals/core/lua/lua.md documents all of it.

local gs = gs
local debug = luadebug -- Lua's debug library; `debug` itself is the tree's debugger
local QUIT, INTERRUPT = gs.quit_signal, gs.interrupt_signal

-- A quit or an interrupt must unwind the whole script: re-raise them past
-- any gs.try / gs.ok.
local function passthrough(e)
  if e == QUIT or e == INTERRUPT then error(e, 0) end
end

-- gs.try(fn, fallback): fn's result, or `fallback` (called with the error
-- when it is a function) if fn raised.
function gs.try(fn, fallback)
  local ok, r = pcall(fn)
  if ok then return r end
  passthrough(r)
  if type(fallback) == "function" then return fallback(r) end
  return fallback
end

-- gs.ok(fn): true when fn ran without raising.
function gs.ok(fn)
  local ok, e = pcall(fn)
  if not ok then passthrough(e) end
  return ok
end

-- gs.hex(v [, width]): zero-padded lowercase hex, no prefix.
function gs.hex(v, width)
  return string.format("%0" .. (width or 8) .. "x", v)
end

-- gs.errtext(e): an error's text, whatever raised it.
function gs.errtext(e)
  return tostring(e)
end

-- --- Templates: "a ${expr} b ${expr:08x}" -----------------------------------
--
-- A template is compiled once into a function of an environment: literal
-- runs are kept, `${expr}` becomes tostring(expr) and `${expr:spec}` a
-- string.format with the spec (printf letters; a leading % is optional).
-- The spec is the rightmost `:` at the top level of the body, when what
-- follows it is a spec.

local compiled = {}

-- Split one `${...}` body into its expression and its format spec.
local function split_spec(body)
  local depth, quote, last = 0, nil, nil
  local i = 1
  while i <= #body do
    local c = body:sub(i, i)
    if quote then
      if c == "\\" then i = i + 1
      elseif c == quote then quote = nil end
    elseif c == '"' or c == "'" then quote = c
    elseif c == "(" or c == "[" or c == "{" then depth = depth + 1
    elseif c == ")" or c == "]" or c == "}" then depth = depth - 1
    elseif c == ":" and depth == 0 then last = i end
    i = i + 1
  end
  if not last then return body, nil end
  local spec = body:sub(last + 1)
  if not spec:match("^%%?[-+ 0#]*%d*%.?%d*[dixXsfeEgGoc]$") then return body, nil end
  if spec:sub(1, 1) ~= "%" then spec = "%" .. spec end
  return body:sub(1, last - 1), spec
end

-- Compile a template into `function(_ENV) ... end`.
local function compile(s)
  local parts = {}
  local pos = 1
  while true do
    local a, b = s:find("${", pos, true)
    if not a then
      parts[#parts + 1] = string.format("%q", s:sub(pos))
      break
    end
    parts[#parts + 1] = string.format("%q", s:sub(pos, a - 1))
    -- the matching close brace
    local depth, j = 1, b + 1
    while j <= #s and depth > 0 do
      local c = s:sub(j, j)
      if c == "{" then depth = depth + 1 elseif c == "}" then depth = depth - 1 end
      j = j + 1
    end
    if depth ~= 0 then error("template: unclosed ${ in " .. s, 3) end
    local expr, spec = split_spec(s:sub(b + 1, j - 2))
    if spec then
      parts[#parts + 1] = string.format("string.format(%q, (%s))", spec, expr)
    else
      parts[#parts + 1] = string.format("tostring(%s)", expr)
    end
    pos = j
  end
  local src = "return function(_ENV) return " .. table.concat(parts, " .. ") .. " end"
  local chunk, err = load(src, "=template", "t")
  if not chunk then error("template: " .. tostring(err) .. " in " .. s, 3) end
  return chunk()
end

-- gs.template(s): the compiled template, a function of an environment table.
function gs.template(s)
  local f = compiled[s]
  if not f then
    f = compile(s)
    compiled[s] = f
  end
  return f
end

-- The environment a template evaluates in: the caller's locals and upvalues,
-- then the globals (and through them the tree).
local function caller_env(level)
  local env = {}
  local info = debug.getinfo(level, "f")
  if info and info.func then
    local i = 1
    while true do
      local name, value = debug.getupvalue(info.func, i)
      if not name then break end
      if name ~= "_ENV" then env[name] = value end
      i = i + 1
    end
  end
  local i = 1
  while true do
    local name, value = debug.getlocal(level, i)
    if not name then break end
    if name:sub(1, 1) ~= "(" then env[name] = value end
    i = i + 1
  end
  return setmetatable(env, { __index = _G })
end

-- F"text ${expr:spec}": the template filled from the caller's scope.
function F(s)
  return gs.template(s)(caller_env(3))
end

-- gs.contains(haystack, needle): plain substring test.
function gs.contains(haystack, needle)
  return string.find(haystack, needle, 1, true) ~= nil
end

-- gs.is_none(v): true for nil and for the gs.none a list carries in place
-- of a VK_NONE item.
function gs.is_none(v)
  return v == nil or v == gs.none
end
