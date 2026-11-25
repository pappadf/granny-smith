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

-- gs.truthy(fn): fn()'s result read as the old language read a value in
-- a condition: false when fn raised, and false for nil, false, 0, "" and an
-- empty table (what `!call(...)` tested).
function gs.truthy(fn)
  local ok, r = pcall(fn)
  if not ok then passthrough(r); return false end
  if r == nil or r == false or r == 0 or r == "" or r == gs.none then return false end
  if type(r) == "table" and next(r) == nil then return false end
  return true
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
      parts[#parts + 1] = string.format("gs.str(%s)", expr)
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

-- gs.str(v): a value as the old `${…}` spliced it.  A list is `[a, b]`
-- with its items as plain text; a map is its compact JSON (strings quoted,
-- nested lists JSON too); nil is the empty string.  pairs keeps a tree map's order.
local function is_sequence(t)
  local n = #t
  for k in next, t do
    if type(k) ~= "number" or k < 1 or k > n or k % 1 ~= 0 then return false end
  end
  return true
end

local MAPMT = luadebug.getregistry()["gs.map"] -- a tree map, or one gs.map tagged

local str
local function json(v)
  local tv = type(v)
  if v == nil or v == gs.none then return "null" end
  if tv == "string" then return string.format("%q", v):gsub("\\\n", "\\n") end
  if tv == "table" then
    if getmetatable(v) ~= MAPMT and is_sequence(v) then
      local parts = {}
      for i = 1, #v do parts[i] = json(v[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local parts = {}
    for k, x in pairs(v) do parts[#parts + 1] = string.format("%q", tostring(k)) .. ":" .. json(x) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return tostring(v)
end

function str(v)
  if v == nil or v == gs.none then return "" end
  if type(v) ~= "table" then return tostring(v) end
  if getmetatable(v) ~= MAPMT and is_sequence(v) then
    local parts = {}
    for i = 1, #v do parts[i] = str(v[i]) end
    return "[" .. table.concat(parts, ", ") .. "]"
  end
  return json(v)
end
gs.str = str

-- echo(map) and tostring(map) read as the JSON text too.
MAPMT.__tostring = str

-- gs.contains(haystack, needle): a plain substring test over two strings,
-- or, over a list haystack (files.identify(p).fits), whether one of its
-- items is `needle`.
function gs.contains(haystack, needle)
  if type(haystack) == "table" then
    for _, v in ipairs(haystack) do
      if v == needle then return true end
    end
    return false
  end
  return string.find(haystack, needle, 1, true) ~= nil
end

-- gs.is_none(v): true for nil and for the gs.none a list carries in place
-- of a VK_NONE item.
function gs.is_none(v)
  return v == nil or v == gs.none
end

-- --- The shell spelling: conditions and templates the debugger stores -------
--
-- A breakpoint condition and a logpoint message are text a person typed, in
-- the spelling the terminal shares with the old shell: `$name` names an
-- alias (`$pc`) or a variable, `&&` `||` `!=` `!` are the C operators.  They
-- are translated once into Lua and compiled; docs/internals/core/lua/lua.md.

-- `$name` -> the alias's path, else the bare name (a global).
local function dollar(name)
  return gs._alias(name) or name
end

-- Translate the code parts of `s` (outside string literals) with `fn`.
local function outside_strings(s, fn)
  local out, i, n, start = {}, 1, #s, 1
  while i <= n do
    local c = s:sub(i, i)
    if c == '"' or c == "'" then
      out[#out + 1] = fn(s:sub(start, i - 1))
      local j = i + 1
      while j <= n and s:sub(j, j) ~= c do
        if s:sub(j, j) == "\\" then j = j + 1 end
        j = j + 1
      end
      out[#out + 1] = s:sub(i, j)
      i, start = j + 1, j + 1
    else
      i = i + 1
    end
  end
  out[#out + 1] = fn(s:sub(start))
  return table.concat(out)
end

-- `$name` read when the code runs (an alias may come and go after a
-- condition or a template was compiled): the aliased path's value, else the
-- environment's `name`.
local alias_readers = {}
function gs._dref(env, name)
  local p = gs._alias(name)
  if not p then return env[name] end
  local f = alias_readers[p]
  if not f then
    f = assert(load("return " .. p, "=alias", "t"))
    alias_readers[p] = f
  end
  return f()
end

-- The text `$name` splices into a template: an alias to an attribute keeps
-- the tree's presentation (hex for an address); a logpoint's `value` and
-- `addr` read as hex.
function gs._dtext(env, name)
  local p = gs._alias(name)
  if p then
    local ok, text = pcall(gs.fmt, p)
    if ok then return text end
    return gs.str(gs._dref(env, name))
  end
  local v = env[name]
  if (name == "value" or name == "addr") and math.type(v) == "integer" then
    return string.format("0x%x", v)
  end
  return gs.str(v)
end

-- The shell's spelling of an expression, as Lua.
local function shell_expr(code)
  return outside_strings(code, function(t)
    t = t:gsub("%$([%a_][%w_]*)", function(name) return 'gs._dref(_ENV, "' .. name .. '")' end)
    t = t:gsub("&&", " and "):gsub("||", " or "):gsub("!=", "~=")
    t = t:gsub("!", " not ")
    return t
  end)
end
gs.shell_expr = shell_expr

local conditions = {}

-- gs.condition(text): a breakpoint condition compiled to a function (nil
-- and the message when it does not compile).
function gs.condition(text)
  local f = conditions[text]
  if f then return f end
  local err
  f, err = load("return (" .. shell_expr(text) .. ")", "=condition", "t")
  if not f then return nil, err end
  conditions[text] = f
  return f
end

-- The truth of a condition's value: false, nil, 0 and none are false.
function gs.condition_true(v)
  return not (v == nil or v == false or v == 0 or v == gs.none)
end

-- True when `body` is a bare path into the tree (`machine.cpu.pc`,
-- `machine.nubus.slot[9].card.id`): its text then keeps the presentation
-- the tree gives it (hex for an address), as the terminal prints it.
local function tree_path(body)
  if not body:match("^[%a_][%w_]*[%w_%.%[%]]*$") then return false end
  local root = body:match("^[%a_][%w_]*")
  if rawget(_G, root) ~= nil then return false end
  return gs.ok(function() gs.node(root) end) and body:find("[%.%[]") ~= nil
end

local shell_templates = {}

-- gs.shell_template(text): a template in the shell's spelling compiled to
-- a function of an environment.  `$name` is `${name}`; inside `${}` the
-- body is a shell expression; a bare tree path keeps the tree's text; the
-- bindings `value` and `addr` (a logpoint's) read as hex.
function gs.shell_template(text)
  local f = shell_templates[text]
  if f then return f end
  -- `$name` outside `${...}` is `${name}`.
  local t, pos, out = text, 1, {}
  while true do
    local a = t:find("%$", pos)
    if not a then out[#out + 1] = t:sub(pos); break end
    out[#out + 1] = t:sub(pos, a - 1)
    if t:sub(a + 1, a + 1) == "{" then
      local depth, j = 1, a + 2
      while j <= #t and depth > 0 do
        local c = t:sub(j, j)
        if c == "{" then depth = depth + 1 elseif c == "}" then depth = depth - 1 end
        j = j + 1
      end
      local body = t:sub(a + 2, j - 2)
      local expr, spec = body:match("^(.-):(%%?[-+ 0#]*%d*%.?%d*[dixXsfeEgGoc])$")
      expr = expr or body
      expr = expr:gsub("^%s+", ""):gsub("%s+$", "")
      local name = expr:match("^%$([%a_][%w_]*)$")
      if not spec and name then
        expr = 'gs._dtext(_ENV, "' .. name .. '")'
      elseif not spec and (expr == "value" or expr == "addr") then
        expr = 'gs._dtext(_ENV, "' .. expr .. '")'
      elseif not spec and tree_path(expr) then
        expr = string.format("gs.fmt(%q)", expr)
      else
        expr = shell_expr(expr)
      end
      out[#out + 1] = "${" .. expr .. (spec and (":" .. spec) or "") .. "}"
      pos = j
    else
      local name = t:match("^[%a_][%w_]*", a + 1)
      if name then
        out[#out + 1] = '${gs._dtext(_ENV, "' .. name .. '")}'
        pos = a + 1 + #name
      else
        out[#out + 1] = "$"
        pos = a + 1
      end
    end
  end
  f = gs.template(table.concat(out))
  shell_templates[text] = f
  return f
end

-- gs.expand(text, bindings): the shell template filled: the bindings, then
-- the globals.  A failure reads as its message.
function gs.expand(text, bindings)
  local ok, r = pcall(function()
    return gs.shell_template(text)(setmetatable(bindings or {}, { __index = _G }))
  end)
  if ok then return r end
  passthrough(r)
  return gs.errtext(r)
end

-- --- The terminal: one line, a command or a Lua chunk ------------------------
--
-- A line whose head is a tree path (`machine.cpu.pc`, `files.ls /tmp`), a
-- command word (`ls`, `run`) or `$alias` is a COMMAND LINE: a bare head is
-- read (a method is called), `head = value` assigns, and anything else is
-- the method's arguments in argument mode -- positional words and
-- `name=value`, "..." interpolating `${expr}`, 'raw', (lua expr), $name, and
-- bare words (true, false, none, numbers like 0x10 or -3, else strings).
-- Results print as the tree formats them.  Any other line is Lua: tried as
-- `return <line>` first so an expression prints its value, `$name` reading
-- the alias or global of that name.  A method followed by `(` is a Lua call
-- (`machine.memory.poke.l(a, v)`), and an attribute followed by anything
-- but `=` an expression (`machine.cpu.pc + 4`).

local function trim(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- `$name` -> the alias's path or the global's name, outside string literals.
local function dollar_rewrite(s)
  return outside_strings(s, function(t)
    return (t:gsub("%$([%a_][%w_]*)", dollar))
  end)
end
gs.dollar_rewrite = dollar_rewrite

-- The index of the bracket closing the one at `i` (strings skipped).
local function close_bracket(s, i, open, close)
  local depth, j, q = 1, i + 1, nil
  while j <= #s do
    local d = s:sub(j, j)
    if q then
      if d == "\\" then j = j + 1 elseif d == q then q = nil end
    elseif d == '"' or d == "'" then q = d
    elseif d == open then depth = depth + 1
    elseif d == close then
      depth = depth - 1
      if depth == 0 then return j end
    end
    j = j + 1
  end
  return nil
end

local function eval_expr(src)
  local f, err = load("return (" .. shell_expr(src) .. ")", "=stdin", "t")
  if not f then error(err, 0) end
  return f()
end

-- A path continuation at `i`: `.name` and `[...]` segments, the bracket
-- evaluated (`[0]`, `["key"]`, `[$i + 1]`) when `eval`.  Returns the text and
-- the index after it.
local function scan_continuation(s, i, eval)
  local out = {}
  while true do
    local c = s:sub(i, i)
    if c == "." then
      local seg = s:match("^[%a_][%w_]*", i + 1)
      if not seg then return nil end
      out[#out + 1] = "." .. seg
      i = i + 1 + #seg
    elseif c == "[" then
      local j = close_bracket(s, i, "[", "]")
      if not j then return nil end
      local inner = s:sub(i + 1, j - 1)
      if eval and not inner:match("^%s*%d+%s*$") and not inner:match('^%s*"[^"]*"%s*$') then
        local v = eval_expr(inner)
        if math.type(v) == "integer" then inner = tostring(v)
        elseif type(v) == "string" then inner = string.format("%q", v)
        else error("index '" .. inner .. "' is not an integer or a key", 0) end
      end
      out[#out + 1] = "[" .. trim(inner) .. "]"
      i = j + 1
    else
      return table.concat(out), i
    end
  end
end

-- The head of a command line at the start of `line`: the tree path it
-- dispatches to, and the index after it; nil when the line is not one.
local function command_head(line, eval)
  if line:sub(1, 1) == "$" then
    local name = line:match("^%$([%a_][%w_]*)")
    if not name then return nil end
    local cont, i = scan_continuation(line, 2 + #name, eval)
    if not cont then return nil end
    local base = gs._alias(name)
    if not base then
      local g = rawget(_G, name)
      if not gs.is_node(g) then return nil end
      base = tostring(g)
    end
    return base .. cont, i
  end
  local root = line:match("^[%a_][%w_]*")
  if not root or rawget(_G, root) ~= nil then return nil end
  local cont, i = scan_continuation(line, 1 + #root, eval)
  if not cont then return nil end
  local path = root .. cont
  if gs._kind(path) then return path, i end
  if cont == "" then
    local target = gs._command(root)
    if target then return target, i end
  end
  return nil
end

-- One "..." body: escapes decoded, `${}` filled from the globals.
local function expand_dq(body)
  local held = {}
  body = body:gsub("\\(.)", function(c)
    if c == "n" then return "\n" elseif c == "t" then return "\t" elseif c == "r" then return "\r"
    elseif c == "$" then held[#held + 1] = "$"; return "\1" .. #held .. "\2"
    else return c end
  end)
  local text = gs.shell_template(body)(setmetatable({}, { __index = _G }))
  return (text:gsub("\1(%d+)\2", function(k) return held[tonumber(k)] end))
end

-- The end of a "..." string opening at `i` (a `${...}` may hold quotes).
local function close_dq(s, i)
  local j = i + 1
  while j <= #s do
    local c = s:sub(j, j)
    if c == "\\" then j = j + 2
    elseif c == '"' then return j
    elseif c == "$" and s:sub(j + 1, j + 1) == "{" then
      local k = close_bracket(s, j + 1, "{", "}")
      if not k then return nil end
      j = k + 1
    else j = j + 1 end
  end
  return nil
end

local LDQ, RDQ = "\226\128\156", "\226\128\157" -- the curly quotes a paste brings

-- One argument at `i`: its value and the index after it.  `raw` keeps a
-- "..." body as typed (a template slot: the subsystem expands it later).
local function arg_value(s, i, raw)
  local c = s:sub(i, i)
  if c == '"' then
    local j = close_dq(s, i)
    if not j then error("unterminated string literal", 0) end
    local body = s:sub(i + 1, j - 1)
    return raw and body or expand_dq(body), j + 1
  end
  if s:sub(i, i + 2) == LDQ then
    local j = s:find(RDQ, i + 3, true)
    if not j then error("unterminated curly-quoted string", 0) end
    local body = s:sub(i + 3, j - 1)
    return raw and body or expand_dq(body), j + 3
  end
  if c == "'" then
    local out, j = {}, i + 1
    while j <= #s and s:sub(j, j) ~= "'" do
      if s:sub(j, j + 1) == "\\'" then out[#out + 1] = "'"; j = j + 2
      else out[#out + 1] = s:sub(j, j); j = j + 1 end
    end
    if j > #s then error("unterminated raw string", 0) end
    return table.concat(out), j + 1
  end
  if c == "(" then
    local j = close_bracket(s, i, "(", ")")
    if not j then error("expected ')' after expression argument", 0) end
    return eval_expr(s:sub(i + 1, j - 1)), j + 1
  end
  if c == "$" then
    local name = s:match("^%$([%a_][%w_]*)", i)
    if not name then error("expected a name after '$'", 0) end
    local cont, j = scan_continuation(s, i + 1 + #name, true)
    if not cont then error("bad path after '$" .. name .. "'", 0) end
    return eval_expr(dollar(name) .. cont), j
  end
  -- A bare word: up to whitespace, a backslash escaping the next character.
  local out, j = {}, i
  while j <= #s and not s:sub(j, j):match("%s") do
    if s:sub(j, j) == "\\" and j < #s then j = j + 1 end
    out[#out + 1] = s:sub(j, j)
    j = j + 1
  end
  local word = table.concat(out)
  if word == "true" then return true, j end
  if word == "false" then return false, j end
  if word == "none" then return nil, j end
  if word:match("^[-+%d]") then
    local n = gs._literal(word)
    if n ~= nil then return n, j end
  end
  return word, j
end

-- Run `path`'s method with the argument-mode text `rest`.
local function run_command(path, rest)
  local pos, n, named, any_named = {}, 0, {}, false
  local i = 1
  while true do
    i = rest:find("%S", i)
    if not i then break end
    local name = rest:match("^([%a_][%w_]*)=", i)
    if name and rest:sub(i + #name + 1, i + #name + 1) == "=" then name = nil end
    if name then
      i = i + #name + 1
      if i > #rest or rest:sub(i, i):match("%s") then
        error("empty value for argument '" .. name .. "' (omit the argument instead)", 0)
      end
    end
    local v
    v, i = arg_value(rest, i, gs._template_slot(path, not name and n or nil, name))
    if name then
      named[name] = v
      any_named = true
    else
      if any_named then error("positional argument after named argument", 0) end
      n = n + 1
      pos[n] = v
    end
  end
  if gs._kind(path) ~= "method" then
    if n == 0 and not any_named then return gs._show(path) end
    error("'" .. path .. "' is not a method (it takes no arguments)", 0)
  end
  if any_named then
    pos[n + 1] = named
    return gs._call_show(path, table.unpack(pos, 1, n + 1))
  end
  return gs._call_show(path, table.unpack(pos, 1, n))
end

-- `path = rhs`: a bare word that names nothing is a string (an enum label:
-- `scheduler.mode = turbo`); anything else is an expression.
local function run_assign(path, rhs)
  rhs = trim(rhs)
  if rhs == "" then error("expected a value after '='", 0) end
  local v
  if rhs:match("^[%a_][%w_]*$") and rhs ~= "true" and rhs ~= "false" and rhs ~= "nil"
      and rawget(_G, rhs) == nil and not gs.ok(function() gs.node(rhs) end) then
    v = rhs
  else
    local f = load("return (" .. shell_expr(rhs) .. ")", "=stdin", "t")
    if f then v = f()
    else
      v = gs._literal(rhs)
      if v == nil then error("cannot read '" .. rhs .. "' as a value", 0) end
    end
  end
  local set = assert(load("local v = ...; " .. path .. " = v", "=stdin", "t"))
  set(v)
end

-- A Lua line: an expression's results print; a statement runs.
local function run_lua(line)
  local src = dollar_rewrite(line)
  local f = load("return " .. src, "=stdin", "t")
  if f then
    gs._print_values(f())
    return
  end
  local err
  f, err = load(src, "=stdin", "t")
  if not f then error(err, 0) end
  f()
end

-- gs.classify(line): "command" or "lua" (nothing evaluated).
function gs.classify(text)
  local line = trim(text)
  if line:find("\n") then return "lua" end
  local ok, path, i = pcall(command_head, line, false)
  if not ok or not path then return "lua" end
  local rest = trim(line:sub(i))
  if rest == "" or rest:match("^=[^=]") or rest == "=" then return "command" end
  -- A method followed by anything but a Lua call's `(` or `:` takes
  -- arguments (`files.ls /tmp`, `scheduler.run 20000000`); after an attribute
  -- the rest is an expression (`machine.cpu.pc + 4`).
  if gs._kind(path) == "method" and not rest:match("^[(:]") then return "command" end
  return "lua"
end

-- gs.shell_line(text): run one line the terminal or the daemon sent.
function gs.shell_line(text)
  local line = trim(text)
  if line == "" then return end
  if gs.classify(line) == "command" then
    local path, i = command_head(line, true)
    local rest = trim(line:sub(i))
    if rest == "" then
      if gs._kind(path) == "method" then return gs._call_show(path) end
      return gs._show(path)
    end
    if rest:sub(1, 1) == "=" then return run_assign(path, rest:sub(2)) end
    return run_command(path, rest)
  end
  -- A bare word naming a global function (`quit`) is a call; one that names
  -- nothing is a mistyped command, not a nil to print.
  if line:match("^[%a_][%w_]*$") then
    local g = rawget(_G, line)
    if type(g) == "function" then return g() end
    if g == nil then error("unknown command or name '" .. line .. "'", 0) end
  end
  return run_lua(line)
end

-- gs.needs_continuation(text): true while `text` is an incomplete Lua chunk
-- (its error is at the end of the input); a command line is always whole.
function gs.needs_continuation(text)
  if trim(text) == "" or gs.classify(text) == "command" then return false end
  local src = dollar_rewrite(text)
  if load("return " .. src, "=stdin", "t") then return false end
  local f, err = load(src, "=stdin", "t")
  if f then return false end
  return err:sub(-5) == "<eof>"
end
