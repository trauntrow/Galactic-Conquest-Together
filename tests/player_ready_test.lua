-- Tests the shell-readiness query from src/lua_query.cpp, read from the source text so the shipped
-- query is what runs. Run from the repository root.
local file = assert(io.open("src/lua_query.cpp", "r"))
local source = file:read("*a"); file:close()
-- Joins the pieces of the query's C string literal into one Lua chunk.
local _, _, literal = string.find(source, "ready_query%[%] =%s*(.-);")
assert(literal, "ready_query not found in src/lua_query.cpp")
local query = ""
for part in string.gfind(literal, '"([^"]*)"') do query = query .. part end
local check = assert(loadstring(query))

-- Clears every global the query checks.
local function reset()
    ScriptCB_IsFileExist, AddIFScreen, ifs_freeform_main, ifs_main = nil, nil, nil, nil
end
-- Only the full set of globals, with tables for both screens, reports ready (6217431).
reset()
assert(check() == 0, "nothing registered must report not ready")
ScriptCB_IsFileExist = function() end; AddIFScreen = function() end
assert(check() == 0, "missing screens must report not ready")
ifs_main = {}
assert(check() == 0, "missing freeform screen must report not ready")
ifs_freeform_main = {}
assert(check() == 6217431, "all present must report ready")
ifs_main = 5
assert(check() == 0, "a non-table screen must report not ready")
reset(); ifs_main = {}; ifs_freeform_main = {}; AddIFScreen = function() end
assert(check() == 0, "missing ScriptCB_IsFileExist channel must report not ready")
print("PASS: player readiness query ready/not-ready outcomes, read from the shipped source")
