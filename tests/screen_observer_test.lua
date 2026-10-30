-- Tests src/lua/screen_observer.lua, which wraps the galaxy screens and reports their calls to the DLL.
-- Telemetry lands in events; fail_emit makes the DLL channel raise.
local events = {}
local fail_emit = false
ScriptCB_IsFileExist = function(text)
    if fail_emit then error("telemetry failure") end
    table.insert(events, text)
end
local function pack(...) return arg end
-- A screen callback that records its arguments and returns nils among its results.
local calls = 0
local received
local function original(...)
    calls = calls + 1
    received = arg
    return "first", nil, false, nil
end
-- The engine's AddIFScreen, also returning nils among its results.
local registration
AddIFScreen = function(...)
    registration = arg
    return 7, nil, 9, nil
end
-- Installing twice keeps one wrapper, which passes every argument and result through, nils included.
ifs_freeform_main = {Enter=original, Exit=original, Update=original, SelectPlanet=original}
local path = "src/lua/screen_observer.lua"
assert(dofile(path) == 6217431)
local first = ifs_freeform_main.Enter
assert(dofile(path) == 6217431 and ifs_freeform_main.Enter == first)
local results = pack(ifs_freeform_main:Enter(42,nil,false,nil))
assert(calls == 1 and received.n == 5 and received[1] == ifs_freeform_main)
assert(received[2] == 42 and received[3] == nil and received[4] == false and received[5] == nil)
assert(results.n == 4 and results[1] == "first" and results[2] == nil and results[3] == false and results[4] == nil)
-- A failing telemetry channel never breaks the call.
fail_emit = true
results = pack(ifs_freeform_main:Exit(nil))
assert(calls == 2 and results.n == 4)
fail_emit = false
-- A screen registered later through AddIFScreen is wrapped once; arguments and results pass through.
local replacement = {Enter=original, Update=original}
results = pack(AddIFScreen(replacement, "ifs_freeform_main", nil, 33, nil))
assert(results.n == 4 and results[1] == 7 and results[3] == 9)
assert(registration.n == 5 and registration[3] == nil and registration[4] == 33 and registration[5] == nil)
assert(replacement.Enter ~= original)
first = replacement.Enter
AddIFScreen(replacement, "ifs_freeform_main")
assert(replacement.Enter == first)
-- An error from a callback reaches the caller unchanged, after its begin record.
local token = {}
local broken = {Enter = function() error(token) end}
AddIFScreen(broken,"ifs_freeform_main")
local ok, err = pcall(broken.Enter)
assert(not ok and err == token)
assert(events[table.getn(events)] == "GCOBS:3:Enter:begin:1")
-- Update is reported for its first four calls and every 300th, begin and done each.
local before = table.getn(events)
for i=1, 600 do ifs_freeform_main:Update(0.016) end
assert(table.getn(events)-before == 12)
print("PASS: Lua 5.0 wrapper arguments/returns, errors, telemetry failure, idempotence, replacement registration and throttling")
-- Child screens are throttled the same way, and nested calls get their own invocation ids.
local fleet = {Enter=original, Exit=original, Update=original}
fleet.Input_Accept = function(self, depth)
    if depth > 0 then return self:Input_Accept(depth-1) end
    return "nested", nil
end
AddIFScreen(fleet, "ifs_freeform_fleet")
before = table.getn(events)
for i=1,600 do fleet:Update(0.016) end
assert(table.getn(events)-before == 12)
results = pack(fleet:Input_Accept(2))
assert(results.n == 2 and results[1] == "nested")
assert(events[table.getn(events)] == "GCOBS:4:Input_Accept:done:1")
assert(events[table.getn(events)-1] == "GCOBS:4:Input_Accept:done:2")
-- The AI screen swaps Update between phase functions; each is wrapped once and keeps its results.
local ai = {}
local phase_calls = 0
ai.UpdateCommon = function(self, dt) assert(dt == 0.25) end
ai.UpdateMoveFleet = function(...)
    phase_calls = phase_calls + 1
    assert(arg.n == 3 and arg[1] == ai and arg[2] == 0.25 and arg[3] == nil)
    ai:UpdateCommon(arg[2])
    return "phase", nil, false, nil
end
ai.CalculatePurchaseItem = function(self, dt, ...)
    self.Update = self.UpdateMoveFleet
    return self:Update(dt, unpack(arg))
end
ai.Enter = function(self) self.Update = self.CalculatePurchaseItem end
AddIFScreen(ai, "ifs_freeform_ai")
ai:Enter()
assert(ai.Update == ai.CalculatePurchaseItem)
results = pack(ai:Update(0.25,nil))
assert(phase_calls == 1 and ai.Update == ai.UpdateMoveFleet)
assert(results.n == 4 and results[1] == "phase" and results[3] == false)
local saved_phase = ai.UpdateMoveFleet
AddIFScreen(ai, "ifs_freeform_ai")
assert(ai.UpdateMoveFleet == saved_phase and ai.Update == saved_phase)
ai:Update(0.25,nil)
assert(phase_calls == 2)
-- Screens the observer does not watch are left alone.
local unrelated = {Enter=original, Update=original}
AddIFScreen(unrelated,"ifs_main")
assert(unrelated.Enter == original and unrelated.Update == original)
print("PASS: child updates, recursive invocation IDs, AI phase assignment/returns, and unrelated registration")
-- A shell rebuild replaces the screens and AddIFScreen: running again wraps the new tables, hooks
-- AddIFScreen again and keeps invocation ids rising.
local surviving_id = 0
for i=1,table.getn(events) do
    local _,_,id = string.find(events[i],"^GCOBS:(%d+):screen:")
    if id and tonumber(id) > surviving_id then surviving_id = tonumber(id) end
end
local engine_add = function(...) registration = arg return 7, nil, 9, nil end
AddIFScreen = engine_add
ifs_freeform_main = {Enter=original, Exit=original, Update=original, SelectPlanet=original}
ifs_freeform_fleet = {Enter=original, Exit=original, Update=original,
    Input_Accept=original, Input_Back=original}
before = table.getn(events)
assert(dofile(path) == 6217431)
assert(ifs_freeform_main.Enter ~= original, "a rebuilt screen must be wrapped again")
assert(ifs_freeform_fleet.Update ~= original, "a rebuilt child screen must be wrapped again")
assert(AddIFScreen ~= engine_add, "the registration hook must be reinstalled")
local rebuilt_id = 0
for i=before+1,table.getn(events) do
    local _,_,id = string.find(events[i],"^GCOBS:(%d+):screen:")
    if id then rebuilt_id = tonumber(id) end
end
assert(rebuilt_id > surviving_id, "invocation ids must continue across a rebuild")
-- Installing twice in a row never wraps the hook itself: a later registration is reported once.
assert(dofile(path) == 6217431)
before = table.getn(events)
AddIFScreen({Enter=original, Exit=original, Update=original, Input_Accept=original,
    Input_Back=original}, "ifs_freeform_summary")
local announcements = 0
for i=before+1,table.getn(events) do
    if string.find(events[i],"screen:ifs_freeform_summary",1,true) then
        announcements = announcements + 1
    end
end
assert(announcements == 1, "a rebuilt hook must not chain onto itself")
print("PASS: re-entrant install after a shell rebuild, with continuing ids and no self-chaining")
-- A fleet screen that survives a rebuild switches to the new capture_state without a second wrapper.
local old_capture_calls, new_capture_calls = 0, 0
capture_state = function() old_capture_calls = old_capture_calls + 1 end
assert(dofile(path) == 6217431)
ifs_freeform_fleet:Update(0.016)
assert(old_capture_calls == 1)
local surviving_update = ifs_freeform_fleet.Update
capture_state = function() new_capture_calls = new_capture_calls + 1 end
assert(dofile(path) == 6217431)
assert(ifs_freeform_fleet.Update == surviving_update)
ifs_freeform_fleet:Update(0.016)
assert(old_capture_calls == 1 and new_capture_calls == 1)
capture_state = nil
print("PASS: surviving wrappers switch to the current capture generation")
