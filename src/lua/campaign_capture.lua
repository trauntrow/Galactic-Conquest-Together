-- Captures the galaxy's saved values as typed text records and sends them to the DLL (GCOBS:state:*).
-- Read only: no game calls and no writes to the galaxy's tables.
local capture_state
-- Producer 7 for live co-op, when the build prepends capture_version = 7; producer 6 otherwise.
local producer = capture_version == 7 and 7 or 6
do
    local raw = rawget
    local kind = type
    local iterate = next
    local fmt, byte, len = string.format, string.byte, string.len
    local floor, mod = math.floor, math.mod
    local sort, insert, getn = table.sort, table.insert, table.getn
    local protect = pcall
    -- Plain fields, and maps of plain keys and values.
    local scalars = {"scenario", "turnNumber", "playerTeam", "planetNext", "winnerTeam", "curScreen"}
    local maps = {"planetTeam", "planetFleet", "planetBase", "teamResources", "lastSelected", "lastFleet"}
    -- Fields read as whatever kind they turn out to be: side and slot candidates, then the
    -- rest of the values ScriptCB_SaveMetagameState receives.
    local probed = {"playerSide", "otherSide", "controllerTeam", "controllerPlayer",
        "teamCode", "teamName", "fleetPtr",
        "custom", "launchMission", "activeBonus", "fleetBattle", "recentPlanets",
        "planetResources", "battleResources", "soakMode"}
    -- The save also reads three fleet screen fields and three globals, prefixed on the wire because
    -- turnNumber and planetNext exist on both tables.
    local fleet_probed = {"turnNumber", "planetStart", "planetNext"}
    local global_probed = {"ifs_purchase_unit_owned", "ifs_purchase_tech_cards",
        "ifs_purchase_tech_using"}
    -- Producer 7 also reads what the host's camera shows, so a friend's camera can follow.
    if producer == 7 then
        insert(probed, "planetSelected")
        insert(fleet_probed, "zoomLevel")
    end
    -- Captured, but changes to these alone do not trigger a snapshot. Producer 7 lets the
    -- cursor (planetNext) trigger one, so a friend's cursor follows the host's.
    local navigation = {curScreen = 1, lastSelected = 1, lastFleet = 1, planetNext = 1}
    if producer == 7 then navigation.planetNext = nil end
    -- Producer 7 samples every 5th tick and sends 32 records a tick; producer 6, every 30th and 8.
    local cadence = producer == 7 and 5 or 30
    local per_tick = producer == 7 and 32 or 8
    local pending, cursor, sequence, attempts, rejections = nil, 1, 0, 0, 0
    local previous, skipped = nil, 0
    -- Text as uppercase hex, "-" when empty.
    local function hexify(text)
        local s = ""
        for i=1,len(text) do s = s .. fmt("%02X", byte(text,i)) end
        return s == "" and "-" or s
    end
    -- A plain value as a kind letter and its wire text: z nil, b boolean, i whole number, s string.
    -- Anything else, or a value out of range, raises VALUE.
    local function encode(value, is_key)
        local t = kind(value)
        if value == nil then return "z", "-" end
        if t == "boolean" and not is_key then return "b", value and "1" or "0" end
        if t == "number" and value == value and value >= -16777215 and value <= 16777215 and value == floor(value) then
            return "i", fmt("%.0f", value)
        end
        if t == "string" and len(value) <= (is_key and 32 or 64) then
            return "s", hexify(value)
        end
        error("VALUE",0)
    end
    -- A nested table as its size and the kinds of its keys and values; its contents are never sent.
    local function summarize(values)
        local n, key_kinds, value_kinds = 0, {}, {}
        local k, v = iterate(values, nil)
        while k ~= nil do
            n = n+1
            if n > 64 then return "n=64+" end
            key_kinds[kind(k)] = 1
            value_kinds[kind(v)] = 1
            k, v = iterate(values, k)
        end
        local function listed(set)
            local names = {}
            for name in pairs(set) do insert(names, name) end
            sort(names)
            local text = ""
            for i=1,getn(names) do
                text = text .. (i > 1 and "+" or "") .. names[i]
            end
            return text == "" and "none" or text
        end
        local summary = "n=" .. n .. " k=" .. listed(key_kinds) .. " v=" .. listed(value_kinds)
        if len(summary) > 48 then return "n=" .. n .. " oversized" end
        return summary
    end
    -- A probed value never fails: a table is summarized (c), and a value encode refuses
    -- reports its kind (u).
    local function encode_probed(value)
        if kind(value) == "table" then return "c", hexify(summarize(value)) end
        local ok, t, v = protect(encode, value, false)
        if ok then return t, v end
        return "u", hexify(kind(value))
    end
    -- A map as a header record, then one record per entry, at most 64.
    local function entries(field, values, records, encode_value)
        insert(records, field .. ":z:-:t:-")
        local k, v = iterate(values, nil)
        local n = 0
        while k ~= nil do
            n = n+1
            if n > 64 then error("LIMIT_" .. field,0) end
            local kt, kv = encode(k, true)
            local vt, vv = encode_value(v)
            insert(records, field .. ":" .. kt .. ":" .. kv .. ":" .. vt .. ":" .. vv)
            k, v = iterate(values, k)
        end
    end
    -- A probed table becomes entry records; anything else, one record.
    local function probe_value(value, label, records)
        if kind(value) == "table" then
            entries(label, value, records, encode_probed)
        else
            local t, v = encode_probed(value)
            insert(records, label .. ":z:-:" .. t .. ":" .. v)
        end
    end
    -- The purchase globals go one level deeper: per field at most 64 outer entries and 128 leaves.
    -- Paths join the encoded keys with '/'.
    local function purchase(value, label, records)
        if kind(value) ~= "table" then return probe_value(value,label,records) end
        insert(records,label .. ":z:-:t:-")
        local outer, leaves = 0, 0
        for k,v in iterate,value do
            outer = outer+1
            if outer > 64 then error("LIMIT_PURCHASE",0) end
            local kt,kv = encode(k,true)
            if kind(v) == "table" then
                insert(records,label .. ":" .. kt .. ":" .. kv .. ":t:-")
                local count = 0
                for child,item in iterate,v do
                    count,leaves = count+1,leaves+1
                    if count > 64 or leaves > 128 then error("LIMIT_PURCHASE",0) end
                    if kind(item) == "table" then error("DEPTH_PURCHASE",0) end
                    local ct,cv = encode(child,true)
                    local vt,vv = encode(item,false)
                    insert(records,label .. ":p:" .. kt .. "/" .. kv .. "/" .. ct .. "/" .. cv .. ":" .. vt .. ":" .. vv)
                end
            else
                local vt,vv = encode_probed(v)
                insert(records,label .. ":" .. kt .. ":" .. kv .. ":" .. vt .. ":" .. vv)
            end
        end
    end
    -- A missing source table fails only its own fields: the fleet screen may not exist yet.
    local function probe_field(source, field, label, records)
        if kind(source) ~= "table" then error("SOURCE",0) end
        if source == _G and string.sub(label,1,7) == "global." then
            purchase(raw(source,field),label,records)
        else
            probe_value(raw(source, field), label, records)
        end
    end
    -- One snapshot: all records sorted, and the sorted subset whose changes trigger a new snapshot.
    local function build()
        local main = raw(_G,"ifs_freeform_main")
        if kind(main) ~= "table" then error("CAMPAIGN",0) end
        local fleet = raw(_G,"ifs_freeform_fleet")
        local records, signal = {}, {}
        -- Adds records to the snapshot, and to the trigger set when watched.
        local function keep(list, watched)
            for i=1,getn(list) do
                insert(records, list[i])
                if watched then insert(signal, list[i]) end
            end
        end
        -- Probed fields are all watched; a failure becomes one error record for that field alone.
        local function collect(source, field, label)
            local group = {}
            local ok, failure = protect(probe_field, source, field, label, group)
            if not ok then
                local reason = kind(failure)=="string" and string.find(failure,"^[A-Za-z_]+$") and failure or "INTERNAL"
                group = {label .. ":z:-:e:" .. hexify(reason)}
            end
            keep(group, true)
        end
        for i=1,getn(scalars) do
            local field = scalars[i]
            local t,v = encode(raw(main,field),false)
            keep({field .. ":z:-:" .. t .. ":" .. v}, not navigation[field])
        end
        for i=1,getn(maps) do
            local field = maps[i]
            local values = raw(main,field)
            if kind(values) ~= "table" then error("MAP_" .. field,0) end
            local group = {}
            entries(field, values, group, function(v) return encode(v,false) end)
            keep(group, not navigation[field])
        end
        for i=1,getn(probed) do collect(main, probed[i], probed[i]) end
        for i=1,getn(fleet_probed) do
            collect(fleet, fleet_probed[i], "fleet." .. fleet_probed[i])
        end
        for i=1,getn(global_probed) do
            collect(_G, global_probed[i], "global." .. global_probed[i])
        end
        sort(records)
        sort(signal)
        return records, signal
    end
    -- Exact comparison of two sorted record lists, with no digest to collide.
    local function unchanged(before, after)
        if before == nil or getn(before) ~= getn(after) then return false end
        for i=1,getn(after) do
            if before[i] ~= after[i] then return false end
        end
        return true
    end
    -- Called from the fleet screen's Update and the AI's turn (screen_observer.lua). A changed
    -- snapshot is queued as begin, item and end records; an unchanged one is only counted.
    capture_state = function(source_id, ticket, send)
        -- Producer 6 stops after 8 snapshots, sent or rejected; producer 7 only after 8 rejections.
        local open = attempts < 8 or (producer == 7 and rejections < 8)
        if pending == nil and open and (ticket == 1 or mod(ticket,cadence) == 0) then
            local ok, records, signal = protect(build)
            if not ok then
                attempts = attempts+1
                rejections = rejections+1
                local reason = kind(records)=="string" and string.find(records,"^[A-Za-z_]+$") and records or "INTERNAL"
                send("state:rejected:" .. attempts .. ":" .. reason) return
            end
            if unchanged(previous,signal) then
                skipped = skipped+1
            else
                attempts = attempts+1
                local n = getn(records)
                sequence = sequence+1
                local queue = {"state:begin:" .. producer .. ":" .. sequence .. ":" .. n .. ":" .. source_id .. ":" .. ticket .. ":" .. skipped}
                for i=1,n do
                    local record = "state:item:" .. sequence .. ":" .. i .. ":" .. records[i]
                    if len(record) > 200 then send("state:rejected:" .. attempts .. ":WIRE_LIMIT") return end
                    insert(queue,record)
                end
                insert(queue,"state:end:" .. sequence .. ":" .. n)
                pending, cursor = queue, 1
                previous, skipped = signal, 0
            end
        end
        -- The queued snapshot goes out per_tick records a call.
        if pending ~= nil then
            local stop = math.min(cursor+per_tick-1,getn(pending))
            while cursor <= stop do send(pending[cursor]) cursor = cursor+1 end
            if cursor > getn(pending) then pending, cursor = nil, 1 end
        end
    end
end
