-- A function that decodes and checks a received galaxy state (binary data, never Lua source).
-- This copy then saves it; the verify, live and patch scripts share everything before the save.
return function(wire)
    assert(type(wire) == 'string' and string.len(wire) <= 4096)
    local cursor, nodes = 1, 0
    -- The next byte of the wire data; reading past its end fails.
    local function byte()
        assert(cursor <= string.len(wire))
        local value = string.byte(wire, cursor)
        cursor = cursor + 1
        return value
    end
    -- Decodes one value by its tag: 0 nil, 1 boolean, 3 number, 4 string, 5 table. Keys are numbers
    -- or strings, nil is never a key or member, and depth and node count are bounded.
    local function read(depth, key, member)
        nodes = nodes + 1
        assert(depth <= 16 and nodes <= 4096)
        local tag = byte()
        if key then assert(tag == 3 or tag == 4) end
        if tag == 0 then assert(not key and not member); return nil end
        if tag == 1 then local b = byte(); assert(b <= 1); return b == 1 end
        -- A little-endian 32-bit float, the game's Lua number type; only whole numbers up to
        -- 16777215, and no infinity or NaN.
        if tag == 3 then
            local a,b,c,d = byte(),byte(),byte(),byte()
            local exponent = math.mod(d,128)*2 + math.floor(c/128)
            local mantissa = math.mod(c,128)*65536 + b*256 + a
            assert(exponent ~= 255)
            local value
            if exponent == 0 then value = math.ldexp(mantissa,-149)
            else value = math.ldexp(mantissa+8388608,exponent-150) end
            if d >= 128 then value = -value end
            assert(math.abs(value) <= 16777215 and math.floor(value) == value)
            return value
        end
        -- A zero-terminated string.
        if tag == 4 then
            local first = cursor
            while byte() ~= 0 do end
            return string.sub(wire,first,cursor-2)
        end
        -- A table: key and value pairs up to a zero byte, at most 128, no key twice.
        assert(tag == 5 and not key)
        local result, seen, count = {}, {}, 0
        while true do
            assert(cursor <= string.len(wire))
            if string.byte(wire,cursor) == 0 then cursor = cursor + 1; return result end
            count = count + 1
            assert(count <= 128)
            local k = read(depth+1,true,false)
            assert(not seen[k]); seen[k] = true
            result[k] = read(depth+1,false,true)
        end
    end
    -- The 26 values ScriptCB_SaveMetagameState takes, and nothing after them.
    local n = byte()
    assert(n == 26)
    local expected = {n=n}
    for i=1,n do expected[i] = read(0,false,false) end
    assert(cursor == string.len(wire)+1)
    -- The galaxy's tables, a scenario this game can start, none of the battle values the host leaves
    -- out, one camera and no battle result waiting.
    for _,i in ipairs({3,4,5,6,7,9,10,11,12}) do assert(type(expected[i]) == 'table') end
    assert(type(expected[2]) == 'string' and type(_G['ifs_freeform_start_' .. expected[2]]) == 'function')
    assert(expected[15] == nil and expected[19] == nil and expected[21] == nil and expected[22] == nil)
    assert(type(ScriptCB_GetNumCameras) == 'function' and ScriptCB_GetNumCameras() == 1)
    assert(type(ScriptCB_GetLastBattleVictory) == 'function' and ScriptCB_GetLastBattleVictory() < 0)
    -- Value 3 maps profile names to teams: exactly one entry, for this PC's profile.
    assert(type(ScriptCB_GetProfileName) == 'function' and type(ScriptCB_ununicode) == 'function')
    local profile = ScriptCB_ununicode(ScriptCB_GetProfileName(1))
    assert(type(profile) == 'string' and expected[3][profile] == expected[13])
    local profiles = 0
    for name,team in pairs(expected[3]) do
        profiles = profiles + 1
        assert(type(name) == 'string' and (team == 1 or team == 2))
    end
    assert(profiles == 1 and type(ScriptCB_SaveMetagameState) == 'function')
    -- Save only; the copy is verified before anything loads it.
    ScriptCB_SaveMetagameState(unpack(expected))
    return 6217431  -- the DLL's success value
end
