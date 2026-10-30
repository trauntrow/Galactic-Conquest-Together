-- Wraps the galaxy screens' methods and reports their calls to the DLL. The fleet screen's Update and
-- the AI's turn also run the galaxy capture and the co-op ready check.
local key = capture_state and "_GCT_campaign_capture_v6" or "_GCT_screen_observer_v2"
local emit = ScriptCB_IsFileExist
local add = AddIFScreen
local screen = ifs_freeform_main
-- Without ScriptCB_IsFileExist (the DLL's channel), AddIFScreen and the galaxy, nothing is wrapped.
if type(emit) ~= "function" or type(add) ~= "function" or type(screen) ~= "table" then
    return -1
end
local protected = pcall
-- In Lua 5.0, arg.n and unpack(arg) keep trailing nils, so every argument is passed on.
local unpack_args = unpack
local kind = type
local mod = math.mod
-- The methods wrapped on each screen.
local common = {"Enter", "Exit", "Update", "Input_Accept", "Input_Back"}
local specs = {
    ifs_freeform_main = {"Enter", "Exit", "Update", "SelectPlanet"},
    ifs_freeform_fleet = common,
    ifs_freeform_summary = common,
    ifs_freeform_result = common,
    ifs_freeform_purchase_unit = common,
    ifs_freeform_purchase_tech = common,
    -- The engine assigns Update from these named methods, so they are wrapped instead of Update.
    ifs_freeform_ai = {"Enter", "Exit", "Input_Accept", "Input_Back",
        "CalculatePurchaseItem", "CalculateBuildFleet", "UpdateBuildFleet",
        "CalculateMoveFleet", "UpdateMoveFleet", "UpdateDone", "UpdateCommon"}
}
-- Screens already registered are wrapped in this order.
local order = {"ifs_freeform_main", "ifs_freeform_fleet", "ifs_freeform_ai",
    "ifs_freeform_summary", "ifs_freeform_result", "ifs_freeform_purchase_unit",
    "ifs_freeform_purchase_tech"}
-- A shell rebuild replaces the screen tables and AddIFScreen, and this script runs again. The
-- surviving record keeps ids unique and stops a table from being wrapped twice.
local state = rawget(_G, key)
if type(state) ~= "table" or type(state.tables) ~= "table" then
    state = {tables = setmetatable({}, {__mode = "k"}), next_id = 0, installs = 0}
end
state.installs = (type(state.installs) == "number" and state.installs or 0) + 1
-- Wrappers from earlier runs call this run's capture function too.
state.capture = capture_state
-- Where the capture samples: the fleet screen's Update, and with producer 7 also the AI's turn, so
-- its moves reach a co-op friend as they happen.
local version = capture_state and (capture_version == 7 and 7 or 6)
local function samples(screen_name, name)
    if not state.capture then return false end
    if screen_name == "ifs_freeform_fleet" and name == "Update" then return true end
    return version == 7 and screen_name == "ifs_freeform_ai" and name == "UpdateCommon"
end
-- The co-op ready check (player_hold.lua) runs on the fleet screen's Update.
state.hold = hold_state
local function holds(screen_name, name)
    return state.hold ~= nil and screen_name == "ifs_freeform_fleet" and name == "Update"
end
-- If the previous wrapper is still installed, chain from the function it wrapped, not from itself.
if add == state.add_wrapper and type(state.add_original) == "function" then
    add = state.add_original
end
-- Messages to the DLL. Only their failure is caught; the screens' own methods never run under pcall.
local function send(text)
    protected(emit, "GCOBS:" .. text)
end
-- Wraps one screen table's methods, once per table, and reports the screen under a new id.
local function watch(target, screen_name)
    if kind(target) ~= "table" or state.tables[target] then return end
    state.next_id = state.next_id + 1
    local id = state.next_id
    local methods = specs[screen_name]
    if not methods then return end
    local wrapped = {}
    state.tables[target] = wrapped
    send(id .. ":screen:" .. screen_name)
    -- Wraps one method. Its calls are reported for the first 4, then every 300th call of a per-frame
    -- method, or up to the 64th of any other.
    local function install(name)
        local original = target[name]
        if kind(original) ~= "function" then
            send(id .. ":missing:" .. name)
            return
        end
        local count = 0
        local frequent = name == "Update" or name == "UpdateCommon" or screen_name == "ifs_freeform_ai"
        -- After the original: report, run the capture and the ready check, and return its results.
        local function completed(ticket, ...)
            if ticket <= 4 or (frequent and mod(ticket,300)==0) or (not frequent and ticket<=64) then
                send(id .. ":" .. name .. ":done:" .. ticket)
            end
            if samples(screen_name, name) then
                protected(state.capture,id,ticket,send)
            end
            if holds(screen_name, name) then
                protected(state.hold,ticket,send)
            end
            return unpack_args(arg)
        end
        local wrapper = function(...)
            count = count + 1
            local report = count <= 4 or (frequent and mod(count, 300) == 0)
            if not frequent and count <= 64 then report = true end
            if report then
                send(id .. ":" .. name .. ":begin:" .. count)
                return completed(count, original(unpack_args(arg)))
            end
            if samples(screen_name, name) or holds(screen_name, name) then
                return completed(count,original(unpack_args(arg)))
            end
            return original(unpack_args(arg))
        end
        wrapped[name] = {original = original, wrapper = wrapper}
        target[name] = wrapper
        send(id .. ":wrapped:" .. name)
    end
    for i = 1, table.getn(methods) do install(methods[i]) end
end
-- Registration, wrapped: every return value is passed on, and a screen is watched once the
-- existing chain has finished adjusting it.
local wrapped_add = function(target, name, ...)
    local function completed(...)
        if specs[name] then watch(target, name) end
        return unpack_args(arg)
    end
    return completed(add(target, name, unpack_args(arg)))
end
-- Screens registered before this script are wrapped now; a missing one is reported.
for i = 1, table.getn(order) do
    local name = order[i]
    if kind(_G[name]) == "table" then watch(_G[name], name)
    else send("absent:" .. name) end
end
-- Registrations from now on go through wrapped_add.
state.add_original = add
state.add_wrapper = wrapped_add
AddIFScreen = wrapped_add
rawset(_G, key, state)
-- The install count tells the decoder the runs apart, as each run restarts its sequence numbers.
send(capture_state and ("installed:campaign_v" .. version .. ":" .. state.installs) or "installed:v2")
return 6217431  -- the DLL's success value
