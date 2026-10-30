-- Runs in each shared battle's Lua state and wraps its ways out, so the DLL hears of them first.
-- Once per state: 6217431 tells the DLL it ran, 0 that a function it needs is missing.
if rawget(_G, "_GCT_exit_wrapped") then return 6217431 end
local quit, ask = rawget(_G, "ScriptCB_QuitToWindows"), rawget(_G, "ScriptCB_IsFileExist")
local tell, give, kind = pcall, unpack, type
if type(quit) ~= "function" or type(ask) ~= "function" or type(tell) ~= "function" or type(give) ~= "function" then
    return 0
end
-- Exit to Windows: the DLL returns once it has told the other PC, then the stock call runs.
rawset(_G, "ScriptCB_QuitToWindows", function(...)
    tell(ask, "GCOBS:menu:closing")
    return quit(give(arg))
end)
rawset(_G, "_GCT_exit_wrapped", true)
-- Quit and the scoreboard's quit are reported too; a failure here leaves the Exit wrapper alone.
tell(function()
    -- For the log: whether the Remaster's settings global exists, and whether a galaxy is saved.
    local function remaster()
        local saved = rawget(_G, "ScriptCB_IsMetagameStateSaved")
        local ok, yes = false, nil
        if kind(saved) == "function" then ok, yes = tell(saved) end
        return kind(rawget(_G, "rema_database")) .. ":" .. (ok and (yes and "saved" or "none") or "unknown")
    end
    -- Each run of the Remaster's quit wrappers puts its settings in front of the galaxy save, and
    -- the shell takes only one copy out, so a copy already in front is removed first (":again").
    local function one_copy()
        local saved, load, save = rawget(_G, "ScriptCB_IsMetagameStateSaved"),
            rawget(_G, "ScriptCB_LoadMetagameState"), rawget(_G, "ScriptCB_SaveMetagameState")
        if kind(saved) ~= "function" or kind(load) ~= "function" or kind(save) ~= "function" or not saved() then
            return ""
        end
        -- The loader pushes all saved values without growing the stack, so each needs a local.
        local v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22,
            v23, v24, v25, v26, v27, v28, v29, v30, v31, v32 = load()
        -- A save led by the Remaster's settings is saved again as the galaxy's 26 values alone.
        if kind(v1) ~= "table" or not v1.isRemaDatabase then return "" end
        save(v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23,
            v24, v25, v26, v27)
        return ":again"
    end
    -- Wraps a quit to report it to the DLL; with own set, it first says this PC's player quit.
    local function report(name, tag, own)
        local stock = rawget(_G, name)
        if kind(stock) ~= "function" then return end
        rawset(_G, name, function(...)
            if own then tell(ask, "GCOBS:battle:quit_own") end
            local ok, again = tell(one_copy)
            tell(ask, "GCOBS:diag:battle_quit:" .. tag .. ":" .. remaster() .. (ok and again or ":unchecked"))
            return stock(give(arg))
        end)
    end
    report("ScriptCB_QuitToShell", "shell", true)  -- the pause menu's Quit
    report("ScriptCB_QuitFromStats", "stats", false)  -- the scoreboard's quit
    -- The state the battle begins with.
    tell(ask, "GCOBS:diag:battle_remaster:" .. remaster() .. ":" .. kind(rawget(_G, "__thisIsGC__")))
end)
return 6217431
