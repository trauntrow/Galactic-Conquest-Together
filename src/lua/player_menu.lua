-- The Online menu for Galactic Conquest, and the shell's side of shared galaxies and battles.
-- ScriptCB_IsFileExist is the channel to the DLL: GCOBS:* messages, and GCOBS:ask:* questions.
local ask = ScriptCB_IsFileExist
local kind = type
local protected = pcall
local unpack_args = unpack
-- A call's results with their count (arg.n), nils included, so unpack gives them all back.
local function packed(...) return arg end
-- The shell functions this script needs; without them it installs nothing (-1).
local needed = {"custom_GetGCButtonList", "custom_PressedGCButton", "AddIFScreen", "AddVerticalButtons",
    "NewIFShellScreen", "NewIFContainer", "RoundIFButtonLabel_fnSetUString", "IFText_fnSetUString",
    "ScriptCB_tounicode"}
if kind(ask) ~= "function" then return -1 end
for i = 1, table.getn(needed) do
    if kind(rawget(_G, needed[i])) ~= "function" then return -1 end
end
-- The DLL runs this on every shell build, and a rebuild replaces custom_GetGCButtonList: one install
-- per build. 6217431 is the success value the DLL checks for.
local state = rawget(_G, "_GCT_menu_v1")
if kind(state) == "table" and custom_GetGCButtonList == state.list then return 6217431 end
state = {}
rawset(_G, "_GCT_menu_v1", state)

-- Unicode text for the menus, a message to the DLL, and a question the DLL answers.
local function text(s) return ScriptCB_tounicode(s) end
local function send(what) protected(ask, "GCOBS:" .. what) end
local function asked(what) return ask("GCOBS:ask:" .. what) == 1 end
-- A global function or table, or nil.
local function global(name)
    local value = rawget(_G, name)
    if kind(value) == "function" or kind(value) == "table" then return value end
    return nil
end
-- The galaxy save's first three values. The loader pushes every saved value at once without growing
-- the stack, so this frame has a local for each, as the stock LoadState does.
local function saved_values(read)
    local v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22,
        v23, v24, v25, v26, v27, v28, v29, v30, v31, v32 = read()
    return v1, v2, v3
end
-- Without its settings (rema_database) the Remaster's SetState and EnterMission wrappers fail, and a
-- galaxy cannot quit or start a battle. The Remaster's own defaults then stand in.
local function remaster_settings(where)
    if rawget(_G, "rema_database") ~= nil then return end
    local defaults = global("swbf2Remaster_getDefaultSettings")
    if not defaults then return end
    local ok, settings = protected(defaults)
    if ok and kind(settings) == "table" then
        rawset(_G, "rema_database", settings)
        send("diag:remaster_defaults:" .. where)
    else
        send("diag:remaster_defaults_failed:" .. where)
    end
end
-- For the DLL's log: the types of the save's first three values ("db": the Remaster's settings).
local function save_shape()
    local read = global("ScriptCB_LoadMetagameState")
    if not read then return "unread" end
    local first, second, third = saved_values(read)
    local head = kind(first) == "table" and first.isRemaDatabase and "db" or kind(first)
    return head .. ":" .. kind(second) .. ":" .. kind(third)
end
-- Logs an error's text through the DLL.
local function failed(where, problem)
    send("diag:" .. where .. "_error:" .. string.sub(tostring(problem), 1, 120))
end

-- The stock Galactic Conquest tab's scenarios, and Load.
local scenarios = {
    {tag = "1", string = "ifs.meta.Configs.1", start = "ifs_freeform_start_all"},
    {tag = "2", string = "ifs.meta.Configs.2", start = "ifs_freeform_start_cis"},
    {tag = "3", string = "ifs.meta.Configs.3", start = "ifs_freeform_start_rep"},
    {tag = "4", string = "ifs.meta.Configs.4", start = "ifs_freeform_start_imp"},
    {tag = "load", string = "ifs.meta.load.btnload"},
}

-- A popup's text, laid out as the stock popups do: the popup is sized to it, with its button below.
-- Setting the title directly leaves the text over the OK button.
local function popup_text(popup, s)
    local layout = global("gPopup_fnSetTitleUStr")
    if layout and protected(layout, popup, text(s)) then return end
    IFText_fnSetUString(popup.title, text(s))
end
-- The busy popup's text, with its elapsed-time line left empty: these waits have their own timeouts,
-- and longer texts then stay clear of the Cancel button.
local function busy_text(popup, s)
    popup.bCallerSetsString = 1
    if kind(popup.BusyTimeStr) == "table" then IFText_fnSetUString(popup.BusyTimeStr, text("")) end
    IFText_fnSetUString(popup.title, text(s))
end

-- The game's OK popup, showing this script's text.
local function message(s)
    local popup = global("Popup_Ok")
    if not popup or kind(popup.fnActivate) ~= "function" or popup.title == nil then return end
    popup.fnDone = nil
    popup:fnActivate(1)
    popup_text(popup, s)
end

-- Pushes a screen with the shell's movie transition.
local function push(screen)
    local push_screen = global("ifs_movietrans_PushScreen")
    if push_screen and screen then push_screen(screen) end
end

-- Starts or loads a galaxy exactly as the Galactic Conquest tab does.
local function start(tag)
    -- Versus: a new game goes to the side screen first; Load loads as Co-op's does.
    if tag == "vs_new" then
        if not state.sides then return message("Versus is not available.") end
        return push(state.sides)
    end
    if tag == "vs_load" then tag = "load" end
    if tag == "load" then
        local load = global("ifs_freeform_load")
        if not load then return message("Galactic Conquest loading is not available.") end
        load.Mode = "Load"
        return push(load)
    end
    local main = global("ifs_freeform_main")
    for i = 1, table.getn(scenarios) do
        local scenario = scenarios[i]
        local begin = scenario.start and global(scenario.start)
        if scenario.tag == tag and main and begin then
            if global("ScriptCB_SetQuitPlayer") then ScriptCB_SetQuitPlayer(1) end
            begin(main)
            return push(main)
        end
    end
    message("Galactic Conquest is not available.")
end

local differs_text = "Your friend's game differs from yours: the game version, the Remaster (both of you need it, or neither) or the co-op DLL. Both of you need the same dinput8.dll."
-- Explains why the co-op game could not start, from what the DLL reports.
local function explain()
    if asked("build") then
        message(differs_text)
    elseif asked("guest") then
        message("You are already in a friend's co-op game. Restart the game to host your own.")
    else
        message("Steam could not start the co-op game. Check that Steam is online and try again.")
    end
end

-- Asks the DLL to invite friends. Co-op then waits on its party screen, Versus on the busy popup
-- until its one friend's game has paired, then starts.
local function invite(tag)
    send("menu:invite:" .. tag)
    if tag ~= "vs_new" and tag ~= "vs_load" and state.party then
        state.party_tag = tag
        return push(state.party)
    end
    local popup = global("Popup_Busy")
    if not popup or kind(popup.fnActivate) ~= "function" or popup.title == nil then
        send("menu:no_popup")
        return start(tag)
    end
    -- 1 once the friend's game has paired, -1 when the DLL reports a failure.
    popup.fnCheckDone = function()
        if asked("paired") then return 1 end
        if asked("failed") then return -1 end
        return 0
    end
    popup.fnOnSuccess = function()
        popup:fnActivate(nil)
        send("menu:paired")
        start(tag)
    end
    popup.fnOnFail = function()
        popup:fnActivate(nil)
        send("menu:failed")
        explain()
    end
    -- Back has already closed the popup.
    popup.fnOnCancel = function() send("menu:cancel") end
    popup.bNoCancel = nil
    popup.fTimeout = nil
    popup.fMinTimeout = nil
    popup:fnActivate(1)
    if asked("no_overlay") then
        busy_text(popup, "The Steam overlay is off. Your friend can join from their Steam friends list.")
    else
        busy_text(popup, "Pick your friend in the Steam overlay. Waiting for their game to join...")
    end
end

-- A sub-screen of the Galactic Conquest tab, built like ifs_sp_gc_main, with the PC tabs when the
-- shell has them. choose gets the picked button's tag.
local function screen(name, title, buttons, labels, choose, extra)
    local layout = {xWidth = 400, width = 400, xSpacing = 10, ySpacing = 5, font = gMenuButtonFont,
        buttonlist = buttons, title = "ifs.meta.Configs.title"}
    local pc = gPlatformStr == "PC" and global("ifelem_tabmanager_Create") and global("ifelem_tabmanager_SetSelected")
        and global("ifelem_tabmanager_HandleInputAccept") and gPCMainTabsLayout and gPCSinglePlayerTabsLayout
    local this = NewIFShellScreen {
        movieIntro = nil,
        movieBackground = nil,
        music = "shell_soundtrack",
        bg_texture = "single_player_conquest",
        buttons = NewIFContainer {ScreenRelativeX = 0.5, ScreenRelativeY = 0.5},
        Enter = function(self, forward)
            if pc then
                if global("UpdatePCTitleText") then UpdatePCTitleText(self) end
                ifelem_tabmanager_SetSelected(self, gPCMainTabsLayout, "_tab_single")
                ifelem_tabmanager_SetSelected(self, gPCSinglePlayerTabsLayout, "_tab_gc", 1)
            end
            gIFShellScreenTemplate_fnEnter(self, forward)
            self.CurButton = ShowHideVerticalButtons(self.buttons, layout)
            SetCurButton(self.CurButton)
        end,
        Input_Accept = function(self)
            if pc then
                if ifelem_tabmanager_HandleInputAccept(self, gPCMainTabsLayout) then return end
                if ifelem_tabmanager_HandleInputAccept(self, gPCSinglePlayerTabsLayout, 1) then return end
            end
            if gShellScreen_fnDefaultInputAccept(self) then return end
            ifelm_shellscreen_fnPlaySound(self.acceptSound)
            choose(self.CurButton)
        end,
    }
    if pc then
        if global("AddPCTitleText") then AddPCTitleText(this) end
        ifelem_tabmanager_Create(this, gPCMainTabsLayout, gPCSinglePlayerTabsLayout)
    end
    this.CurButton = AddVerticalButtons(this.buttons, layout)
    -- These labels are not in the game's string tables: Unicode text set before registration takes
    -- precedence over the string keys.
    IFText_fnSetUString(this.buttons._titlebar_, text(title))
    for tag, label in pairs(labels) do RoundIFButtonLabel_fnSetUString(this.buttons[tag], text(label)) end
    if extra then extra(this) end
    state.add_original(this, name)
    rawset(_G, name, this)
    return this
end

-- The co-op party screen.
local party_rows, party_row, party_build
do
    -- The host's party screen (ifs_gct_party): this PC and a row per friend's slot. Start works once
    -- a friend is ready; Back leaves the co-op game, for every friend too.
    local party_states = {ready = "ready", joining = "joining...", build = "different version"}
    -- Takes the DLL's party rows (_GCT_party_rows): the lobby's member count, "<slot>:<state>:<name>" per
    -- friend, and "L<seq>:<slot>:<name>" or "J<seq>:<slot>:<name>" per friend who left or joined.
    function party_rows(data)
        local rows, notes = {}, {}
        local _, _, members = string.find(data or "", "^(%d+)\n")
        rows.members = tonumber(members) or 0
        state.party_names = state.party_names or {}
        for line in string.gfind(data or "", "[^\n]+") do
            local _, _, slot, what, name = string.find(line, "^(%d):(%a+):(.*)$")
            local _, _, note, seq, at, who = string.find(line, "^([LJ])(%d+):(%d):(.*)$")
            if slot then
                rows[tonumber(slot)] = {state = what, name = name}
                state.party_names[tonumber(slot)] = name
            elseif note then
                -- Each slot keeps its last name for later notes, unless a friend is in it now.
                if not rows[tonumber(at)] then state.party_names[tonumber(at)] = who end
                -- Only notes newer than the last one shown are kept.
                if tonumber(seq) > (state.note_shown or 0) then
                    table.insert(notes, {seq = tonumber(seq), slot = tonumber(at), name = who, joined = note == "J"})
                end
            end
        end
        state.party_rows, state.party_notes = rows, notes
        return true
    end
    -- A friend's row, or nil for an open slot.
    function party_row(slot)
        local rows = state.party_rows
        return kind(rows) == "table" and rows[slot] or nil
    end
    -- Relabels the friend rows, and Start while no friend is ready.
    local function party_refresh(this)
        if not this then return end
        for slot = 1, 3 do
            local row = party_row(slot)
            local label = "Open slot"
            if row then label = row.name .. ": " .. (party_states[row.state] or row.state) end
            RoundIFButtonLabel_fnSetUString(this.buttons["friend" .. slot], text(label))
        end
        RoundIFButtonLabel_fnSetUString(this.buttons.start, text(asked("paired") and "Start" or "Start (no friend ready yet)"))
    end
    -- The party screen's buttons. A friend's row explains its state when picked.
    local function party_choose(tag)
        local _, _, slot = string.find(tag, "^friend(%d)$")
        local row = slot and party_row(tonumber(slot))
        if tag == "invite" then
            if asked("no_overlay") then
                return message("The Steam overlay is off. Friends can join from their Steam friends list.")
            end
            if party_row(1) and party_row(2) and party_row(3) then return message("Your party is full: you and three friends.") end
            send("menu:invite_more")
        elseif tag == "start" then
            if not asked("paired") then return message("No friend is ready yet. Start once a friend's row says ready.") end
            send("menu:start")
            start(state.party_tag)
        elseif row and row.state == "build" then
            message(differs_text)
        elseif row and row.state == "joining" then
            message(row.name .. " is joining. Their row says ready once their game has paired with yours.")
        end
    end
    local party_frames = 0
    -- Every 10th frame: leaves the screen if the co-op game failed, else refreshes the rows.
    local function party_update(this)
        party_frames = party_frames + 1
        if math.mod(party_frames, 10) ~= 1 then return end
        -- A failure other than a build mismatch: Steam refused the lobby, or this PC is a guest.
        if asked("failed") and not asked("build") then
            send("menu:party_failed")
            ScriptCB_PopScreen()
            return explain()
        end
        party_refresh(this)
    end
    -- Builds the party screen. Its Update refreshes the rows, and Back leaves the co-op game.
    function party_build()
        return screen("ifs_gct_party", "Co-op: your party",
            {{tag = "you", string = "You (host)"}, {tag = "friend1", string = "Open slot"},
             {tag = "friend2", string = "Open slot"}, {tag = "friend3", string = "Open slot"},
             {tag = "invite", string = "Invite friends"}, {tag = "start", string = "Start"}},
            {you = "You (host)", friend1 = "Open slot", friend2 = "Open slot", friend3 = "Open slot",
             invite = "Invite friends", start = "Start"}, party_choose, function(this)
                local enter = this.Enter
                this.Enter = function(self, forward)
                    enter(self, forward)
                    send("menu:party")
                    party_refresh(self)
                end
                this.Update = function(self, dt)
                    if global("gIFShellScreenTemplate_fnUpdate") then gIFShellScreenTemplate_fnUpdate(self, dt) end
                    local ok, problem = protected(party_update, self)
                    if not ok then send("menu:party_error:" .. string.sub(tostring(problem), 1, 80)) end
                end
                this.Input_Back = function(self)
                    send("menu:leave")
                    ScriptCB_PopScreen()
                end
            end)
    end
end

local versus_build
-- Builds the Online screens: Co-op with its party screen, Versus with its side screen, then Online.
local function build()
    local buttons = {}
    for i = 1, table.getn(scenarios) do buttons[i] = {tag = scenarios[i].tag, string = scenarios[i].string} end
    state.coop = screen("ifs_gct_coop", "Co-op", buttons, {}, invite)
    -- If the party screen fails to build, Co-op falls back to the busy popup and one friend.
    local built, party = protected(party_build)
    if built then state.party = party else send("menu:party_build_failed:" .. string.sub(tostring(party), 1, 120)) end
    versus_build()
    state.online = screen("ifs_gct_online", "Online", {{tag = "coop", string = "Co-op"}, {tag = "versus", string = "Versus"}},
        {coop = "Co-op", versus = "Versus"}, function(tag)
            if tag == "coop" then push(state.coop)
            elseif tag == "versus" then push(state.versus) end
        end)
end

-- Following the host's galaxy from a friend's menus.
local watched, frames, showing, told, quiet, watch
do
    -- The friend's side, from the menus' Update: a busy popup while their game waits for the host,
    -- the Versus side screen when the host opens it, and a failed join explained once.
    watched = {ifs_main = true, ifs_sp_campaign = true, ifs_sp_gc_main = true}
    frames, showing, told, quiet = 0, false, nil, 0
    -- Every 15th frame while no popup is open. Back on the busy popup leaves the co-op game.
    local function guest_watch()
        frames = frames + 1
        if showing or frames < quiet or (frames ~= 1 and math.mod(frames, 15) ~= 0) then return end
        if global("ScriptCB_IsPopupOpen") and ScriptCB_IsPopupOpen() then return end
        -- A version mismatch or a failed join is explained once.
        local problem = (asked("guest_build") and "build") or (asked("join_failed") and "join") or nil
        if problem ~= told then
            told = problem
            if problem == "build" then
                return message("Your game does not match your friend's: the co-op DLL, or the Remaster (both of you need it, or neither). Both of you need the same dinput8.dll.")
            elseif problem == "join" then
                return message("Could not join your friend's co-op game. Ask them to invite you again.")
            end
        end
        -- The host is on the Versus side screen, so this PC goes there too, once: a screen transition
        -- can take longer than 15 frames.
        if not asked("versus") then state.sides_open = nil end
        local function sides_due()
            return not state.sides_open and asked("versus") and not asked("versus_host") and asked("versus_heard") and
                not asked("versus_started")
        end
        local function open_sides()
            state.sides_open = true
            send("menu:guest_sides")
            push(state.sides)
        end
        if sides_due() then return open_sides() end
        if not asked("guest_wait") then return end
        local popup = global("Popup_Busy")
        if not popup or kind(popup.fnActivate) ~= "function" or popup.title == nil then return end
        showing = true
        local versus_game = asked("versus")
        -- The co-op popup says how many players are in the party so far.
        local members, checks = nil, 0
        local function waiting_text()
            if versus_game then return "Joined your friend's Versus game. Waiting for them to set it up..." end
            local now = 0
            for n = 4, 2, -1 do
                if asked("lobby_members_" .. n) then now = n; break end
            end
            if now == members then return nil end
            members = now
            if now >= 3 then
                return "Joined your friend's co-op game: " .. now .. " players so far. Their galaxy loads when they start playing."
            end
            return "Joined your friend's co-op game. Their galaxy loads when they start playing."
        end
        -- Ends once the DLL stops the wait, or when the Versus side screen is due.
        popup.fnCheckDone = function()
            if versus_game and sides_due() then return 1 end
            if not asked("guest_wait") then return 1 end
            checks = checks + 1
            if not versus_game and math.mod(checks, 15) == 0 then
                local update = waiting_text()
                if update then busy_text(popup, update) end
            end
            return 0
        end
        popup.fnOnSuccess = function()
            showing = false
            popup:fnActivate(nil)
            if versus_game and sides_due() then return open_sides() end
            -- The host left before starting, or its game went away.
            if asked("guest_ended") then
                send("menu:guest_ended")
                return message(versus_game and "Your friend left the Versus game before it started." or
                    "Your friend left the co-op game before it started.")
            end
            send("menu:guest_loading")
        end
        popup.fnOnFail = function()
            showing = false
            popup:fnActivate(nil)
        end
        popup.fnOnCancel = function()
            showing = false
            quiet = frames + 120  -- gives the DLL time to leave the lobby
            send("menu:leave")
        end
        popup.bNoCancel = nil
        popup.fTimeout = nil
        popup.fMinTimeout = nil
        popup:fnActivate(1)
        busy_text(popup, waiting_text())
        send("menu:guest_waiting")
    end
    -- Runs guest_watch after a menu screen's Update, passing its results on.
    local function updated(...)
        protected(guest_watch)
        return unpack_args(arg)
    end
    -- Wraps a watched menu screen's Update.
    function watch(this)
        local update = this.Update
        if kind(update) ~= "function" then return end
        this.Update = function(self, ...) return updated(update(self, unpack_args(arg))) end
    end
end

-- Hosting a shared battle from the battle card.
local battle, unit_types, versus_card_wrap, wrap_card, enter_mission
do
    -- Shared battles run as the game's own LAN match, which the DLL carries over Steam. The stock
    -- host calls run one per frame on a busy popup; on failure the battle runs single-player.
    battle = {}
    -- No ScriptCB_StartLogin, as on the stock PC LAN menus: its sign-in handler crashes in LAN mode.
    local host_steps = {
        {"connect", function() ScriptCB_SetConnectType("lan") end},
        {"name", function() ScriptCB_SetNetLoginName(ScriptCB_GetCurrentProfileNetName()) end},
        {"netshell", function() ScriptCB_OpenNetShell(1) end},
        {"host", function() ScriptCB_SetAmHost(1) end},
        -- One mission, not a list: without a rotation the match ends in the shell.
        {"missions", function() ScriptCB_SetMissionNames(battle.mission, nil) end},
        {"options", function()
            local prefs = ScriptCB_GetNetGameDefaults()
            -- No auto-assignment: each human keeps the side the stock SaveMissionSetup gave them.
            prefs.bAutoAssignTeams = false
            prefs.bIsDedicated = false
            prefs.PasswordStr = ""
            -- No warm-up: the match starts as soon as the host's battle runs.
            prefs.iWarmUp = 0
            if battle.heroes then prefs.bHeroesEnabled = true end
            -- Network games read the bots' difficulty from iDifficulty, not from the profile.
            -- Copying the profile's keeps a shared battle at Galactic Conquest's difficulty.
            local difficulty = global("ScriptCB_GetDifficulty") and ScriptCB_GetDifficulty()
            if kind(difficulty) == "number" and difficulty >= 1 and difficulty <= 3 then
                prefs.iDifficulty = difficulty
                send("battle:difficulty:" .. difficulty)
            end
            -- Room for every player of the co-op game: the count only rises, to the party's size.
            local party = 0
            for n = 4, 2, -1 do
                if asked("battle_party_" .. n) then party = n; break end
            end
            local players = prefs.iNumPlayers
            if party > 0 and kind(players) == "number" and players < party then prefs.iNumPlayers = party end
            send("battle:players:" .. tostring(players) .. ":" .. tostring(prefs.iNumPlayers))
            ScriptCB_SetNetGameDefaults(prefs)
            -- The Leader card turns heroes on. Both games need the same hero rules from the start:
            -- unlocked by the card only, for the player with the most kills, back 120 s after it dies.
            if battle.heroes then
                ScriptCB_SetNetHeroDefaults({iHeroUnlock = 7, iHeroUnlockVal = 5, iHeroPlayer = 2, iHeroTeam = 3,
                    iHeroRespawn = 2, iHeroRespawnVal = 120})
                send("battle:heroes:leader")
            end
        end},
        {"gamename", function() ScriptCB_SetGameName(battle.token) end},
        -- ScriptCB_EnterMission's wrappers do their part now (the Remaster saves its settings with the
        -- galaxy); the game's own function does nothing when given an argument.
        {"wrappers", function() battle.wrapped = true; remaster_settings("battle"); battle.enter("gct") end},
        {"lobby", function() battle.lobby = true; ScriptCB_BeginLobby() end},
    }

    -- Undoes the steps, as the stock Single Player button does, and starts the single-player battle.
    local function single_player(why)
        if battle.lobby and global("ScriptCB_CancelLobby") then protected(ScriptCB_CancelLobby) end
        if global("ScriptCB_CancelLogin") then protected(ScriptCB_CancelLogin) end
        if global("ScriptCB_CloseNetShell") then protected(ScriptCB_CloseNetShell, 1) end
        if global("ScriptCB_SetInNetGame") then protected(ScriptCB_SetInNetGame, nil) end
        send("battle:fallback:" .. why)
        if not battle.wrapped then return battle.enter() end
        -- The wrappers have run, so only the game's own launch is left: its change to the mission state.
        if global("SetState") then SetState("mission") end
    end

    -- What the friend's game needs to join, as SaveMissionSetup stored it: the human's side, the
    -- world and each side's purchased unit types ("all=1111100,imp=1111111").
    unit_types = {"soldier", "pilot", "assault", "sniper", "engineer", "officer", "special"}
    local function setup_text(main)
        local side
        for _, team in pairs(main.controllerTeam or {}) do
            side = kind(main.teamCode) == "table" and main.teamCode[team] or nil
            if kind(side) == "string" then break end
        end
        local setup = global("ScriptCB_IsMissionSetupSaved") and global("ScriptCB_LoadMissionSetup") and
            ScriptCB_IsMissionSetupSaved() and ScriptCB_LoadMissionSetup()
        if kind(side) ~= "string" or kind(setup) ~= "table" or kind(setup.world) ~= "string" or kind(setup.units) ~= "table" then
            return nil
        end
        local parts = {}
        for code, owned in pairs(setup.units) do
            if kind(code) == "string" and kind(owned) == "table" then
                local bits = ""
                for i = 1, table.getn(unit_types) do bits = bits .. (owned[unit_types[i]] and "1" or "0") end
                table.insert(parts, code .. "=" .. bits)
            end
        end
        table.sort(parts)
        return ":" .. side .. ":" .. setup.world .. ":" .. table.concat(parts, ",")
    end
    -- The sides' bonus cards, "1=autoturrets,2=leader"; the friend's game activates them too.
    local function bonus_text(main)
        local parts = {}
        local active = kind(main.activeBonus) == "table" and main.activeBonus or {}
        for team = 1, 2 do
            local name = active[team]
            if kind(name) == "string" and string.find(name, "^[%w_]+$") and string.len(name) <= 31 then
                table.insert(parts, team .. "=" .. name)
            end
        end
        return table.concat(parts, ",")
    end

    -- Hosts the battle on the busy popup. Without a mission or the popup, the stock launch runs.
    local function host()
        local main = global("ifs_freeform_main")
        local popup = global("Popup_Busy")
        battle.mission = main and main.launchMission
        if kind(battle.mission) ~= "string" or not popup or kind(popup.fnActivate) ~= "function" or popup.title == nil then
            send("battle:skipped")
            return battle.enter()
        end
        battle.step, battle.wrapped, battle.lobby, battle.failed = 1, nil, nil, nil
        battle.token = string.format("GCT-%04x%04x", math.random(0, 65535), math.random(0, 65535))
        local ok, setup = protected(setup_text, main)
        setup = ok and setup or ""
        local read, bonus = protected(bonus_text, main)
        bonus = read and bonus or ""
        battle.heroes = string.find("," .. bonus .. ",", "=leader,", 1, true) ~= nil and
            global("ScriptCB_SetNetHeroDefaults") ~= nil
        if bonus ~= "" then
            if setup == "" then setup = ":::" end
            setup = setup .. ":" .. bonus
        end
        -- Sent before any network call, so the DLL can seal the game's LAN sockets first.
        send("battle:hosting:" .. battle.token .. ":" .. battle.mission .. setup)
        -- One host step per frame, then the lobby calls.
        popup.fnCheckDone = function()
            local step = host_steps[battle.step]
            if step then
                local ok, problem = protected(step[2])
                if not ok then
                    battle.failed = step[1] .. ":" .. string.sub(tostring(problem), 1, 80)
                    return -1
                end
                send("battle:step:" .. step[1])
                battle.step = battle.step + 1
                return 0
            end
            -- Then as the mission select's auto-launch Update does: every frame until the mission starts.
            local ok, problem = protected(ScriptCB_UpdateLobby, nil)
            if ok then ok, problem = protected(ScriptCB_LaunchLobby) end
            if not ok and not battle.failed then battle.failed = "launch:" .. string.sub(tostring(problem), 1, 80) end
            return 0
        end
        popup.fnOnSuccess = function() popup:fnActivate(nil) end
        popup.fnOnFail = function()
            popup:fnActivate(nil)
            single_player(battle.failed or "no launch within 20 s")
        end
        popup.fnOnCancel = function() end
        popup.bNoCancel = 1
        popup.fTimeout = 20  -- seconds to launch, else single-player
        popup.fMinTimeout = nil
        popup:fnActivate(1)
        busy_text(popup, "Starting the battle...")
    end

    -- The battle card's Next is the only Galactic Conquest launch, so hosting happens only inside it.
    -- In Versus the card is also wrapped for the defender's pick.
    function wrap_card(card)
        local next = card.Next
        if kind(next) ~= "function" then return send("battle:card_unknown") end
        card.Next = function(self, ...)
            state.launching = true
            next(self, unpack_args(arg))
            state.launching = nil
        end
        if versus_card_wrap then versus_card_wrap(card) end
    end
    -- Replaces ScriptCB_EnterMission: the battle card's launch is hosted when the DLL asks for it;
    -- any other call goes to the function it replaces.
    function enter_mission(...)
        remaster_settings("battle")
        if table.getn(arg) == 0 and state.launching and asked("battle_host") then
            state.launching = nil
            local ok, problem = protected(host)
            if ok then return end
            send("battle:fallback:setup:" .. string.sub(tostring(problem), 1, 80))
            return battle.enter()
        end
        return battle.enter(unpack_args(arg))
    end
end

-- Notes shown in the galaxy after a battle.
local notes, popup_open, show_note, versus_note, battle_note, card_pick, join_ready, close_note
do
    -- Notes after a battle the players did not fight together, shown from the fleet screen when the
    -- DLL says one is due (player_hold.lua). Each has a tag, a co-op text and a Versus text.
    notes = {
        {"join_failed", "Could not join your friend's battle. Their galaxy updates here when it is over.",
            "Could not join your opponent's battle. Its result shows here when their turn ends."},
        {"friend_failed", "Your friend's game could not join that battle, so you fought it without them.",
            "Your opponent's game could not join that battle, so you fought it against the AI."},
        {"friend_stayed", "Your friend stayed in the galaxy during that battle.",
            "Your opponent stayed in the galaxy during that battle, so you fought it against the AI."},
        {"friend_left", "Your friend left that battle before it ended.",
            "Your opponent's game left that battle before it ended, and the AI fought on for them."},
        {"friend_quit", "Your friend left that battle before it ended.",
            "Your opponent quit that battle, so it counts as their loss."},
    }
    function popup_open() return global("ScriptCB_IsPopupOpen") and ScriptCB_IsPopupOpen() end
    -- The game's OK popup, or nil when it cannot be used.
    local function ok_popup()
        local popup = global("Popup_Ok")
        if not popup or kind(popup.fnActivate) ~= "function" or popup.title == nil then return nil end
        return popup
    end
    -- Shows one of this script's notes on the OK popup; the friend's battle join may close it.
    function show_note(s)
        local popup = ok_popup()
        if not popup then return false end
        state.note = popup
        -- Accept closes the popup, then calls fnDone.
        popup.fnDone = function() state.note = nil end
        popup:fnActivate(1)
        popup_text(popup, s)
        return true
    end
    -- With several friends, one note names each who missed the battle, by the party rows' names
    -- ("Friend 2" to "Friend 4" without them).
    local friend_notes = {failed = "could not join that battle.", stayed = "stayed in the galaxy during that battle.",
        left = "left that battle before it ended."}
    local function friends_note()
        if not asked("battle_note_friends") then return false end
        local lines = {}
        for slot = 1, 3 do
            for _, what in ipairs({"failed", "stayed", "left"}) do
                if asked("battle_note_friend" .. slot .. "_" .. what) then
                    local row = party_row(slot)
                    local name = row and row.name or (state.party_names and state.party_names[slot]) or ("Friend " .. (slot + 1))
                    table.insert(lines, name .. " " .. friend_notes[what])
                end
            end
        end
        if table.getn(lines) == 0 then return false end
        show_note(table.concat(lines, " "))
        send("battle:noted:friends")
        return true
    end
    -- A friend who left the co-op game or joined it in progress: the host's galaxy says so once, by name,
    -- after any battle's note.
    local function party_note()
        local notes = state.party_notes
        if kind(notes) ~= "table" or not notes[1] then return false end
        local note = table.remove(notes, 1)
        if not show_note(note.name .. (note.joined and " joined the co-op game." or " left the co-op game.")) then
            return false
        end
        -- Shown once: rows the DLL hands over before it drops the note skip it.
        state.note_shown = note.seq
        send("menu:noted:" .. note.seq)
        return true
    end
    -- The fleet screen's notes, one at a time: Versus, then the friends', the battle's and the party's.
    function battle_note()
        if popup_open() then return end
        state.note = nil  -- no popup is open, so any note has closed
        if not ok_popup() then return end
        if versus_note() then return end
        if friends_note() then return end
        for i = 1, table.getn(notes) do
            local name = notes[i][1]
            if asked("battle_note_" .. name) then
                show_note(asked("versus") and notes[i][3] or notes[i][2])
                send("battle:noted:" .. name)
                return
            end
        end
        party_note()
    end
    -- The friend's battle join waits while a popup is open, except this script's note, which the join
    -- closes, and while this PC's own card pick is up.
    card_pick = nil
    function join_ready()
        if card_pick then return false end
        if not popup_open() then state.note = nil; return true end
        return state.note ~= nil
    end
    -- Closes this script's note if it is open.
    function close_note()
        local popup = state.note
        if not popup then return end
        state.note = nil
        popup.fnDone = nil
        protected(popup.fnActivate, popup, nil)
    end
end

-- What a friend's game may do in the host's galaxy.
local versus, lock_fleet, lock_menu, pause_invite
do
    -- The friend's lock: in the host's galaxy only the host moves fleets, buys, ends turns and loads.
    -- The first refusal in a galaxy says why (Load's every time); later ones play the error sound.
    local lock_notes = {
        follow = "In co-op your friend, the host, moves the fleets and buys. Your galaxy shows theirs.",
        load = "In co-op only your friend, the host, loads a game. To play a save of your own, quit this co-op game first.",
        stopped = "Your galaxy has stopped following your friend's. Quit to the menus to end this co-op game.",
    }
    -- In Versus the PC whose side is taking its turn acts, and refusals only play the error sound, unless
    -- the two galaxies no longer agree.
    local versus_notes = {
        stopped = "The two galaxies no longer agree. Quit to the menus to end this Versus game.",
    }
    function versus() return asked("versus") end
    -- True on this PC's turn: its one controller then plays the active team.
    local function my_turn()
        local main = global("ifs_freeform_main")
        return main ~= nil and main.joystick ~= nil
    end
    -- Whether this PC only watches: in the host's galaxy in co-op, on the other side's turn in Versus.
    local function follows()
        if versus() then return not my_turn() end
        return asked("galaxy_follow")
    end
    -- Refuses an input: the error sound, or a note saying why the first time (every time with always).
    local function refuse(screen, what, always)
        send("galaxy:refused:" .. what)
        local quiet = versus() and not asked("galaxy_stopped")
        if quiet or (state.lock_told and not always) or popup_open() then
            if screen.errorSound and global("ifelm_shellscreen_fnPlaySound") then
                protected(ifelm_shellscreen_fnPlaySound, screen.errorSound)
            end
            return
        end
        local notes = versus() and versus_notes or lock_notes
        local note = asked("galaxy_stopped") and notes.stopped or notes[what] or notes.follow
        if show_note(note) then state.lock_told = true; send("galaxy:told") end
    end
    -- Fleet screen inputs that leave the galaxy as it is: Help, Back, the Move tab, and on PC a click on
    -- nothing or, with no fleet selected, on another planet, which only moves the cursor.
    local function harmless(screen)
        local button = screen.CurButton
        if button == "_help" or button == "_back" or button == "_tab_fleet" then return true end
        if gPlatformStr ~= "PC" or button == "_accept" or button == "_next" then return false end
        if kind(button) == "string" and string.sub(button, 1, 5) == "_tab_" then return false end
        local planet = screen.planetRollover
        if planet == nil then return true end
        local main = global("ifs_freeform_main")
        return not screen.fleetSelected and main ~= nil and main.planetSelected ~= planet
    end
    -- Locks the fleet screen: Accept (moves, purchases, end of turn, tabs), Back (deselect, undo), Tab.
    function lock_fleet(screen)
        local accept, back, key = screen.Input_Accept, screen.Input_Back, screen.Input_KeyDown
        if kind(accept) == "function" then
            screen.Input_Accept = function(self, ...)
                if not harmless(self) and follows() then return refuse(self, "accept") end
                return accept(self, unpack_args(arg))
            end
        end
        if kind(back) == "function" then
            screen.Input_Back = function(self, ...)
                if (self.fleetSelected or self.displayTimer) and follows() then return refuse(self, "back") end
                return back(self, unpack_args(arg))
            end
        end
        if kind(key) == "function" then
            screen.Input_KeyDown = function(self, code, ...)
                -- Key 9 is Tab, which opens the Bonus screen directly.
                if code == 9 and follows() then return refuse(self, "tab") end
                return key(self, code, unpack_args(arg))
            end
        end
        send("galaxy:lock:fleet")
    end
    -- Locks the pause menu's Load, which would replace the host's galaxy. Save and Quit stay.
    function lock_menu(screen)
        local accept = screen.Input_Accept
        if kind(accept) ~= "function" then return end
        screen.Input_Accept = function(self, ...)
            if self.CurButton == "load" and (follows() or versus()) then return refuse(self, "load", true) end
            return accept(self, unpack_args(arg))
        end
        send("galaxy:lock:menu")
    end
    -- While the co-op host's lobby has room, its pause menu offers Invite friends: the stock friends
    -- button, relabelled after the stock Enter and hidden again when not wanted.
    function pause_invite(screen)
        local enter, accept, layout = screen.Enter, screen.Input_Accept, global("ifsfreeformmenu_vbutton_layout")
        if kind(enter) ~= "function" or kind(accept) ~= "function" or kind(screen.buttons) ~= "table" or
            kind(screen.buttons.friends) ~= "table" or not layout or not global("ShowHideVerticalButtons") or
            not global("SetCurButton") then
            return
        end
        screen.Enter = function(self, ...)
            enter(self, unpack_args(arg))
            local wanted = asked("party_invite")
            self.gct_invite = wanted
            if not wanted and not self.gct_relabelled then return end
            local friends = self.buttons.friends
            if wanted then
                RoundIFButtonLabel_fnSetUString(friends, text("Invite friends"))
                self.gct_relabelled = true
                send("galaxy:invite_shown")
            end
            friends.hidden = not wanted
            self.CurButton = ShowHideVerticalButtons(self.buttons, layout)
            SetCurButton(self.CurButton)
        end
        screen.Input_Accept = function(self, ...)
            if self.CurButton ~= "friends" or not self.gct_invite then return accept(self, unpack_args(arg)) end
            if gShellScreen_fnDefaultInputAccept(self) then return end
            ifelm_shellscreen_fnPlaySound(self.acceptSound)
            if asked("no_overlay") then
                return message("The Steam overlay is off. Friends can join from their Steam friends list.")
            end
            if not asked("party_invite") then return message("Your party is full: you and three friends.") end
            send("menu:invite_more")
        end
    end
end

-- Joining the host's battle.
local battle_join
do
    -- Seconds the friend's game waits for the battle's launch: the host's game takes it in only once its
    -- own map has loaded, which can take over a minute with the Remaster or a slow disk.
    local join_wait = 150
    local join = {}
    -- The stock LAN client calls (ifs_mp, ifs_mp_main, ifs_mp_autonet), made one per frame.
    local join_steps = {
        {"connect", function() ScriptCB_SetConnectType("lan") end},
        {"name", function() ScriptCB_SetNetLoginName(ScriptCB_GetCurrentProfileNetName()) end},
        {"netshell", function() ScriptCB_OpenNetShell(1) end},
        {"client", function() ScriptCB_SetAmHost(nil) end},
        {"list", function() join.listing = true; ScriptCB_BeginSessionList() end},
    }
    -- Closes the network game; the friend's game stays in the galaxy.
    local function close_join(why)
        if join.found and global("ScriptCB_CancelJoin") then protected(ScriptCB_CancelJoin) end
        if join.listing and global("ScriptCB_CancelSessionList") then protected(ScriptCB_CancelSessionList) end
        if global("ScriptCB_CancelLogin") then protected(ScriptCB_CancelLogin) end
        if global("ScriptCB_CloseNetShell") then protected(ScriptCB_CloseNetShell, 1) end
        if global("ScriptCB_SetInNetGame") then protected(ScriptCB_SetInNetGame, nil) end
        send("battle:joinfail:" .. why)
    end
    -- The friend's game joins the host's battle. The DLL calls this from the galaxy with the host's
    -- announcement: token, mission, side, world, units and bonus cards, one per line.
    function battle_join(data)
        if card_pick then return false end
        close_note()
        if popup_open() then return false end
        local main = global("ifs_freeform_main")
        local popup = global("Popup_Busy")
        if not main or kind(main.SaveState) ~= "function" or not popup or kind(popup.fnActivate) ~= "function" or
            popup.title == nil then return false end
        local _, _, token, mission, side, world, units, bonus =
            string.find(data, "^([^\n]*)\n([^\n]*)\n([^\n]*)\n([^\n]*)\n([^\n]*)\n([^\n]*)$")
        assert(token and token ~= "" and mission ~= "", "the battle announcement is incomplete")
        join.step, join.found, join.listing, join.failed = 1, nil, nil, nil
        -- Versus: a result this PC keeps lands at the cursor (planetNext), so the cursor moves to
        -- the battle's planet, the world the announcement names.
        local versus_game = asked("versus")
        if versus_game and world ~= "" and world ~= main.planetNext and kind(main.modelMatrix) == "table" and
            kind(main.modelMatrix[world]) == "table" then
            main.planetNext = world
            send("battle:planet_next:" .. world)
        end
        -- As the battle card does: save the galaxy, which the shell resumes after the battle, and let the
        -- EnterMission wrappers do their part.
        main:SaveState()
        remaster_settings("join")
        if battle.enter then battle.enter("gct") end
        -- The host's mission setup when announced, else this galaxy's own.
        if units ~= "" and world ~= "" then
            local setup = {units = {}, world = world}
            for code, bits in string.gfind(units, "(%a%a%a)=([01]+)") do
                local owned = {}
                for i = 1, table.getn(unit_types) do
                    if string.sub(bits, i, i) == "1" then owned[unit_types[i]] = true end
                end
                setup.units[code] = owned
            end
            ScriptCB_SaveMissionSetup(setup)
        elseif kind(main.SaveMissionSetup) == "function" then
            main:SaveMissionSetup()
        end
        -- Versus: this PC's player fights on their own side, the teamCode of this PC's controller's team.
        if versus_game then
            local team = kind(main.controllerTeam) == "table" and main.controllerTeam[0]
            local own = team and kind(main.teamCode) == "table" and main.teamCode[team]
            if kind(own) == "string" then side = own end
            send("battle:own_side:" .. tostring(own))
        end
        if side ~= "" then ScriptCB_MetagameSetSide(0, side) end
        -- The host's bonus cards, and none for a side without one: this game works out some of their
        -- effects itself, from its own copy.
        if bonus ~= "" and global("ActivateBonus") then
            local names = {}
            for team, name in string.gfind(bonus, "([12])=([%w_]+)") do names[tonumber(team)] = name end
            for team = 1, 2 do
                if names[team] then ActivateBonus(team, "team_bonus_" .. names[team])
                else ActivateBonus(team, "team_bonus_none", false) end
            end
            send("battle:bonus:" .. bonus)
        end
        send("battle:joining:" .. token)
        -- Each frame: the next client step, then ScriptCB_AutoNetJoin until it finds the host's session.
        popup.fnCheckDone = function()
            if asked("battle_join_off") then join.failed = "the host's battle is over"; return -1 end
            local step = join_steps[join.step]
            if step then
                local ok, problem = protected(step[2])
                if not ok then
                    join.failed = step[1] .. ":" .. string.sub(tostring(problem), 1, 80)
                    return -1
                end
                send("battle:step:" .. step[1])
                join.step = join.step + 1
                return 0
            end
            if not join.found then
                local ok, found = protected(ScriptCB_AutoNetJoin)
                if not ok then join.failed = "find:" .. string.sub(tostring(found), 1, 80); return -1 end
                if found then join.found = true; send("battle:found") end
                return 0
            end
            -- Then the popup only watches the join (-1: refused or lost). The DLL makes the lobby calls
            -- that launch the mission, which close this popup first.
            local ok, done = protected(ScriptCB_IsJoinDone)
            if not ok then join.failed = "watch:" .. string.sub(tostring(done), 1, 80); return -1 end
            if kind(done) == "number" and done < 0 then join.failed = "the host's game refused or lost the join"; return -1 end
            return 0
        end
        popup.fnOnSuccess = function() popup:fnActivate(nil) end
        popup.fnOnFail = function()
            popup:fnActivate(nil)
            close_join(join.failed or ("no launch within " .. join_wait .. " s"))
        end
        -- Back has already closed the popup.
        popup.fnOnCancel = function() close_join("the friend stayed in the galaxy") end
        popup.bNoCancel = nil
        popup.fTimeout = join_wait
        popup.fMinTimeout = nil
        popup:fnActivate(1)
        busy_text(popup, versus_game and "Joining your opponent's battle... Cancel stays in the galaxy, and the AI fights for you." or
            "Joining your friend's battle... Cancel stays in the galaxy.")
        return true
    end
end

-- The Versus card pick.
local versus_card
do
    -- Versus: the defender's bonus card is picked on the defender's PC. The attacker's card screen holds
    -- the AI's draw and waits for the answer; Esc, or card_wait seconds without one, accepts the draw.
    local card_wait = 60
    -- A team's three card slots, 0 for an empty one.
    local function team_slots(team)
        local all = global("ifs_purchase_tech_using")
        local using = all and all[team]
        if kind(using) ~= "table" then return nil end
        local slots = {}
        for i = 1, 3 do
            local v = using[i]
            slots[i] = (kind(v) == "number" and v > 0 and v < 65536) and math.floor(v) or 0
        end
        return slots
    end
    local function any_card(slots) return slots[1] > 0 or slots[2] > 0 or slots[3] > 0 end

    -- The card screen's title, or its stock one when s is nil.
    local function card_title(screen, s)
        local title = kind(screen.title) == "table" and screen.title.text
        if not title then return end
        if s then IFText_fnSetUString(title, text(s))
        elseif global("IFText_fnSetString") then IFText_fnSetString(title, "ifs.freeform.usecard") end
    end
    -- The card screen's info panel, with a caption and a text.
    local function card_info(screen, caption, body)
        local info = kind(screen.info) == "table" and screen.info
        local show = global("IFObj_fnSetVis")
        if not info or not show then return end
        show(info, true)
        if kind(info.caption) == "table" then show(info.caption, true); IFText_fnSetUString(info.caption, text(caption)) end
        if kind(info.text) == "table" then show(info.text, true); IFText_fnSetUString(info.text, text(body)) end
    end
    -- Selects a card (nil for none) and updates the info panel.
    local function select_card(screen, index)
        if kind(screen.SetSelected) == "function" then screen:SetSelected(index) else screen.selected = index end
        if kind(screen.UpdateAction) == "function" then screen:UpdateAction() end
    end

    -- The attacker's PC: the wait is over. slot is the answer (0 for none), or nil for the AI's draw; the
    -- stock AcceptBonus then runs.
    local function card_answered(screen, slot)
        local wait = screen.card_wait
        if not wait then return end
        screen.card_wait = nil
        card_title(screen, nil)
        local chosen = wait.drawn
        if slot then
            chosen = nil
            for i, item in ipairs(kind(screen.useActive) == "table" and screen.useActive or {}) do
                if kind(item) == "table" and item.slot == slot then chosen = i; break end
            end
        end
        select_card(screen, chosen)
        send("versus:card_done:" .. (slot or "ai"))
        if kind(wait.accept) == "function" then wait.accept(screen) end
    end

    -- The defender's PC: puts back what the pick changed, sends the answer if not sent yet and pops
    -- the screen. Tables a newer revision has replaced meanwhile are left alone.
    local function close_pick(answer)
        local pick = card_pick
        if not pick then return end
        card_pick = nil
        local main = global("ifs_freeform_main")
        local all = global("ifs_purchase_tech_using")
        if all and all == pick.all then all[pick.team] = pick.using end
        if main then
            main.launchMission = pick.mission
            if kind(main.activeBonus) == "table" and main.activeBonus == pick.bonuses then main.activeBonus[pick.team] = pick.bonus end
            if main.playerTeam ~= pick.active and kind(main.SetActiveTeam) == "function" then main:SetActiveTeam(pick.active) end
        end
        if answer and not pick.answered then send("versus:card:" .. pick.number .. ":" .. answer) end
        send("versus:card_closed:" .. (answer and "answered" or "unanswered"))
        if global("ScriptCB_PopScreen") then ScriptCB_PopScreen() end
    end
    -- The defender's PC: shows this side's card pick, called by the DLL with the request number, the
    -- mission and the slots. Without cards there is nothing to pick.
    function versus_card(data)
        if card_pick or popup_open() then return false end
        local _, _, number, mission, a, b, c = string.find(data or "", "^(%d+)\n([%w_]+)\n(%d+),(%d+),(%d+)$")
        assert(number, "the card request is incomplete")
        local main = global("ifs_freeform_main")
        local all = global("ifs_purchase_tech_using")
        assert(main and all and global("ifs_freeform_battle_card") and kind(main.SetActiveTeam) == "function",
            "the card screen is missing")
        local team = kind(main.controllerTeam) == "table" and main.controllerTeam[0]
        assert(team == 1 or team == 2, "this side's team is unknown")
        local slots = {tonumber(a), tonumber(b), tonumber(c)}
        if not any_card(slots) then send("versus:card:" .. number .. ":0"); return true end
        -- Borrows the slots, the mission and the active team for the pick; close_pick puts them back.
        card_pick = {number = number, team = team, active = main.playerTeam, all = all, using = all[team],
            mission = main.launchMission, bonuses = main.activeBonus,
            bonus = kind(main.activeBonus) == "table" and main.activeBonus[team] or nil, slot = 0, frames = 0}
        all[team] = slots
        main.launchMission = mission
        if main.playerTeam ~= team then main:SetActiveTeam(team) end
        send("versus:card_shown:" .. number)
        ScriptCB_PushScreen("ifs_freeform_battle_card")
        return true
    end

    -- Wraps the battle card screen for Versus: the attacker's wait for the defender's pick, and the
    -- defender's own pick.
    versus_card_wrap = function(card)
        local enter, accept_bonus, next, update = card.Enter, card.AcceptBonus, card.Next, card.Update
        if kind(enter) ~= "function" or kind(accept_bonus) ~= "function" or kind(next) ~= "function" then
            return send("versus:card_unknown")
        end
        card.Enter = function(self, ...)
            self.card_wait = nil
            local main = global("ifs_freeform_main")
            local slots = main and team_slots(main.playerTeam)
            local mission = main and main.launchMission
            -- Only the defender's pass on the attacker's PC waits, and only when the defender has a card.
            local wait = not card_pick and self.defending and main and main.joystick == nil and slots and any_card(slots) and
                kind(mission) == "string" and string.find(mission, "^[%w_]+$") and string.len(mission) <= 31 and
                asked("versus") and asked("versus_paired")
            if not wait then return enter(self, unpack_args(arg)) end
            -- The stock Enter draws the AI's card; it is held, not accepted, while the other PC picks.
            local accept = self.AcceptBonus
            self.AcceptBonus = function() end
            local results = {protected(enter, self, unpack_args(arg))}
            self.AcceptBonus = accept
            if not results[1] then error(results[2]) end
            self.card_wait = {drawn = self.selected, accept = accept, time = 0, frames = 0}
            select_card(self, nil)
            card_title(self, "Your opponent is picking a card...")
            card_info(self, "Your opponent is picking a card", "Their pick shows here. Esc lets the AI pick for them.")
            send("versus:card_ask:" .. mission .. ":" .. slots[1] .. "," .. slots[2] .. "," .. slots[3])
            table.remove(results, 1)
            return unpack_args(results)
        end
        -- The defender's PC answers as soon as its pick is accepted.
        card.AcceptBonus = function(self, ...)
            local pick = card_pick
            if pick and not pick.answered then
                local item = kind(self.useActive) == "table" and self.selected and self.useActive[self.selected]
                pick.slot = kind(item) == "table" and kind(item.slot) == "number" and item.slot or 0
                pick.answered = true
                send("versus:card:" .. pick.number .. ":" .. pick.slot)
            end
            return accept_bonus(self, unpack_args(arg))
        end
        -- On the defender's PC, Next closes the pick instead of launching.
        card.Next = function(self, ...)
            if card_pick then return close_pick(card_pick.slot) end
            return next(self, unpack_args(arg))
        end
        -- Esc arrives as Back or Start. On these passes it never opens the pause menu, whose Save would
        -- store the borrowed slots and active team.
        for _, name in ipairs({"Input_Back", "Input_Start"}) do
            local handler = card[name]
            if kind(handler) == "function" then
                card[name] = function(self, ...)
                    -- On the attacker's PC, Esc ends the wait with the AI's draw.
                    if self.card_wait then return card_answered(self, nil) end
                    -- On the defender's PC, Esc skips the pick: no card.
                    if card_pick then
                        if not self.displayTimer then close_pick(0) end
                        return
                    end
                    return handler(self, unpack_args(arg))
                end
            end
        end
        -- The attacker's player cannot pick for the defender while the screen waits.
        for _, name in ipairs({"Input_Accept", "Input_GeneralLeft", "Input_GeneralRight", "Input_KeyDown", "Input_Misc"}) do
            local handler = card[name]
            if kind(handler) == "function" then
                card[name] = function(self, ...)
                    if self.card_wait then return end
                    return handler(self, unpack_args(arg))
                end
            end
        end
        -- The attacker's PC checks for the answer every 5 frames; the defender's checks every 15 that the
        -- request is still open.
        if kind(update) == "function" then
            card.Update = function(self, ...)
                local wait = self.card_wait
                if wait then
                    wait.time = wait.time + (kind(arg[1]) == "number" and arg[1] or 0)
                    wait.frames = wait.frames + 1
                    if wait.frames >= 5 then
                        wait.frames = 0
                        if asked("versus_card_answered") then
                            local slot = 0
                            for k = 1, 3 do if asked("versus_card_slot_" .. k) then slot = k end end
                            card_answered(self, slot)
                        elseif wait.time > card_wait then
                            card_answered(self, nil)
                        end
                    end
                end
                local pick = card_pick
                if pick then
                    pick.frames = pick.frames + 1
                    if pick.frames >= 15 then
                        pick.frames = 0
                        if not pick.answered and not asked("versus_card_open") then return close_pick(nil) end
                    end
                end
                return update(self, unpack_args(arg))
            end
        end
        send("versus:card_wrapped")
    end
end

-- The friend's game back from a shared battle loads its own save from before it. Its own result
-- is not applied (the host's arrives as a revision), and the AI's turn shows the fleet screen.
local function returning_enter(this)
    local enter = this.Enter
    if kind(enter) ~= "function" then return end
    this.Enter = function(self, ...)
        if not state.returning then return enter(self, unpack_args(arg)) end
        state.returning = nil
        local keep = state.keep
        state.keep = nil
        local push, victory = ScriptCB_PushScreen, ScriptCB_GetLastBattleVictory
        local winner
        -- During Enter: the fleet screen, not the AI's, and no result screen for a kept result.
        ScriptCB_PushScreen = function(screen, ...)
            if screen == "ifs_freeform_ai" then screen = "ifs_freeform_fleet"; send("battle:ai_screen_skipped") end
            if winner and screen == "ifs_freeform_result" then return send("battle:result_screen_skipped") end
            return push(screen, unpack_args(arg))
        end
        -- Versus, a result kept after the battle's host left: Enter gets the winning galaxy side,
        -- this PC's side (mine) or the other (theirs), not the engine's battle team.
        if keep then
            ScriptCB_GetLastBattleVictory = function()
                local own = kind(self.controllerTeam) == "table" and self.controllerTeam[0]
                if own ~= 1 and own ~= 2 then return -1 end
                winner = keep == "mine" and own or 3 - own
                return winner
            end
        end
        local results = {protected(enter, self, unpack_args(arg))}
        ScriptCB_PushScreen, ScriptCB_GetLastBattleVictory = push, victory
        if not results[1] then error(results[2]) end
        -- A kept result skips its result screen and save prompt, then the attacker's turn ends as
        -- NextTurn ends it.
        if winner then
            self.requestSave = nil
            send("battle:kept:" .. winner .. ":" .. tostring(self.planetNext))
            if kind(self.NextTurn) == "function" then self:NextTurn() end
        elseif keep then
            send("battle:kept_none")
        end
        table.remove(results, 1)
        return unpack_args(results)
    end
end

-- The Versus galaxy.
local eras, versus_main, versus_ai
do
    -- Versus: each PC plays one side and the AI plays neither. The DLL sends a turn to the other PC only
    -- when it ends, so planning stays private.
    eras = {[1] = {"Republic", "CIS"}, [2] = {"Rebellion", "Empire"}}

    -- Runs a stock function with the AI screen's push showing the fleet screen instead, in Versus only.
    -- Enter and NextTurn are the only places that push the AI screen.
    local function without_ai(call, ...)
        if not versus() then return call(unpack_args(arg)) end
        local push = ScriptCB_PushScreen
        ScriptCB_PushScreen = function(screen, ...)
            if screen == "ifs_freeform_ai" then screen = "ifs_freeform_fleet"; send("versus:ai_screen_skipped") end
            return push(screen, unpack_args(arg))
        end
        local results = {protected(call, unpack_args(arg))}
        ScriptCB_PushScreen = push
        if not results[1] then error(results[2]) end
        table.remove(results, 1)
        return unpack_args(results)
    end
    -- A Versus galaxy loaded from a save: a save naming only the opponent's profile gives this player the
    -- other side; anything else keeps LoadState's mapping. Reported once per shell build.
    local function versus_loaded(main)
        local read, name, plain = global("ScriptCB_LoadMetagameState"), global("ScriptCB_GetProfileName"),
            global("ScriptCB_ununicode")
        if not read or not name or not plain then return send("versus:load_unread") end
        local _, _, saved = saved_values(read)
        local own = plain(name(1))
        local side = kind(main.controllerTeam) == "table" and main.controllerTeam[0] or nil
        local how = "start"
        if kind(saved) == "table" and own ~= nil and saved[own] ~= nil then
            how = "saved"
        elseif kind(saved) == "table" and global("ifs_freeform_controllers") then
            local count, other = 0, nil
            for _, team in pairs(saved) do count = count + 1; other = team end
            if count == 1 and (other == 1 or other == 2) then
                side, how = 3 - other, "other"
                ifs_freeform_controllers(main, {[0] = side})
            end
        end
        local told = tostring(side) .. ":" .. how
        if state.load_told == told then return end
        state.load_told = told
        send("versus:load:" .. told)
    end
    -- Wraps the galaxy's main screen. In Versus: Enter and NextTurn without the AI, no console side
    -- screen, and the end told to the other PC. In both modes LoadState and OneTimeInit are logged.
    function versus_main(this)
        local enter, once, next_turn, load = this.Enter, this.OneTimeInit, this.NextTurn, this.LoadState
        if kind(load) == "function" then
            this.LoadState = function(self, ...)
                -- Logs the save's shape, the Remaster's settings and any error, which is raised again.
                local read, shape = protected(save_shape)
                send("diag:galaxy_load:" .. (read and shape or "unread") .. ":" .. kind(rawget(_G, "rema_database")))
                local results = packed(protected(load, self, unpack_args(arg)))
                if not results[1] then failed("galaxy_load", results[2]); error(results[2], 0) end
                table.remove(results, 1)
                send("diag:galaxy_loaded")
                if versus() then
                    local ok, problem = protected(versus_loaded, self)
                    if not ok then send("versus:load_error:" .. string.sub(tostring(problem), 1, 80)) end
                end
                return unpack_args(results)
            end
        end
        if kind(enter) == "function" then
            this.Enter = function(self, ...) return without_ai(enter, self, unpack_args(arg)) end
        end
        if kind(once) == "function" then
            this.OneTimeInit = function(self, ...)
                send("diag:galaxy_init")
                local results = packed(protected(once, self, unpack_args(arg)))
                if not results[1] then failed("galaxy_init", results[2]); error(results[2], 0) end
                table.remove(results, 1)
                -- The Versus sides are already picked, so the console side screen never shows.
                if versus() and self.setupSides then self.setupSides = nil; send("versus:sides_skipped") end
                return unpack_args(results)
            end
        end
        if kind(next_turn) == "function" then
            this.NextTurn = function(self, ...)
                local results = {without_ai(next_turn, self, unpack_args(arg))}
                local winner, turn = self.teamVictory, self.turnNumber
                -- The end of a Versus game is told to the other PC.
                if (winner == 1 or winner == 2) and kind(turn) == "number" and versus() then
                    send("versus:over:" .. winner .. ":" .. turn)
                end
                return unpack_args(results)
            end
        end
        send("versus:main_wrapped")
    end
    -- Only Enter and NextTurn push the AI screen; one that shows in a Versus galaxy anyway is logged.
    function versus_ai(this)
        local enter = this.Enter
        if kind(enter) ~= "function" then return end
        this.Enter = function(self, ...)
            if versus() then send("versus:ai_entered") end
            return enter(self, unpack_args(arg))
        end
    end
end

-- The Versus side select.
local versus_setup, sides_refresh, sides_choose, sides_update
do
    -- Versus New: the stock custom setup's launch is kept instead of run. Its Prefs are copied and the
    -- friend is invited.
    function versus_setup(this)
        local accept = this.Input_Accept
        if kind(accept) ~= "function" then return send("versus:setup_unknown") end
        this.Input_Accept = function(self, ...)
            if not state.versus_setup then return accept(self, unpack_args(arg)) end
            local begin, push_screen = rawget(_G, "ifs_freeform_start_custom"), ScriptCB_PushScreen
            local prefs
            -- For this call the setup's start just keeps its Prefs, and the galaxy is not pushed.
            ifs_freeform_start_custom = function(main, chosen) prefs = chosen end
            ScriptCB_PushScreen = function(screen, ...)
                if prefs and screen == "ifs_freeform_main" then return end
                return push_screen(screen, unpack_args(arg))
            end
            local results = {protected(accept, self, unpack_args(arg))}
            ifs_freeform_start_custom, ScriptCB_PushScreen = begin, push_screen
            if not results[1] then error(results[2]) end
            if kind(prefs) == "table" then
                state.versus_setup = nil
                local copy = {}
                for k, v in pairs(prefs) do copy[k] = v end
                state.versus_prefs, state.versus_era = copy, copy.iEra
                send("versus:setup:" .. tostring(copy.iEra) .. ":" .. tostring(copy.iVictoryType))
                invite("vs_new")
            end
            table.remove(results, 1)
            return unpack_args(results)
        end
        send("versus:setup_wrapped")
    end

    -- The side screen (ifs_gct_sides), on both PCs: each picks a side and Ready. Once both are ready on
    -- different sides, the host starts the galaxy and the friend's game loads it.
    local function side_names()
        local era = state.versus_era or (asked("versus_era_1") and 1) or (asked("versus_era_2") and 2) or nil
        return eras[era] or {"Side 1", "Side 2"}
    end
    -- A side's label: who picked it, and who is ready.
    local function side_label(n, names)
        local mine, theirs = asked("versus_mine_" .. n), asked("versus_peer_" .. n)
        if mine and theirs then return names[n] .. " (you and your friend: one of you change)" end
        if mine then return names[n] .. (asked("versus_mine_ready") and " (you, ready)" or " (you)") end
        if theirs then return names[n] .. (asked("versus_peer_ready") and " (your friend, ready)" or " (your friend)") end
        return names[n]
    end
    -- Relabels the side buttons and Ready.
    function sides_refresh(this)
        if not this then return end
        local names = side_names()
        RoundIFButtonLabel_fnSetUString(this.buttons.side1, text(side_label(1, names)))
        RoundIFButtonLabel_fnSetUString(this.buttons.side2, text(side_label(2, names)))
        local ready = "Ready"
        if state.versus_starting or asked("versus_started") then ready = "Starting the galaxy..."
        elseif asked("versus_mine_ready") then ready = "Not ready" end
        RoundIFButtonLabel_fnSetUString(this.buttons.ready, text(ready))
    end
    local function my_side() return (asked("versus_mine_1") and 1) or (asked("versus_mine_2") and 2) or nil end
    -- The host: the custom setup's own start with the kept Prefs, then this PC's controller on its side.
    local function versus_start()
        local main, begin = global("ifs_freeform_main"), global("ifs_freeform_start_custom")
        local controllers, side = global("ifs_freeform_controllers"), my_side()
        if not main or not begin or not controllers or kind(state.versus_prefs) ~= "table" or not side then
            state.versus_failed = true  -- so the failure is explained once
            send("versus:start_failed")
            return message("The Versus game could not start. Go back and set it up again.")
        end
        state.versus_starting = true
        send("versus:started")
        if global("ScriptCB_SetQuitPlayer") then ScriptCB_SetQuitPlayer(1) end
        begin(main, state.versus_prefs)
        controllers(main, {[0] = side})
        send("versus:host_side:" .. side)
        ScriptCB_PopScreen()
        ScriptCB_PushScreen("ifs_freeform_main")
    end
    -- The side and Ready buttons. Ready needs a side the friend is not ready on.
    function sides_choose(tag)
        if state.versus_starting or asked("versus_started") then return end
        if tag == "side1" or tag == "side2" then
            if asked("versus_mine_ready") then message("Press Not ready to change sides.")
            else send("versus:pick:" .. (tag == "side1" and 1 or 2)) end
        elseif tag == "ready" then
            local side = my_side()
            if asked("versus_mine_ready") then send("versus:ready:0")
            elseif not side then message("Pick a side first.")
            elseif asked("versus_peer_" .. side) and asked("versus_peer_ready") then
                message("Your friend is ready on that side. Pick the other one.")
            else send("versus:ready:1") end
        end
        sides_refresh(state.sides)
    end
    local sides_frames = 0
    -- Every 10th frame: closes the screen if the other PC left; the host starts the galaxy when told to.
    function sides_update(this)
        sides_frames = sides_frames + 1
        if math.mod(sides_frames, 10) ~= 1 then return end
        if not versus() then
            -- The other PC left, and the DLL has already left the lobby.
            if not state.versus_starting then
                state.sides_open = nil
                send("versus:sides_closed")
                ScriptCB_PopScreen()
                message("Your friend has left the Versus game.")
            end
            return
        end
        if asked("versus_host") and not state.versus_starting and not state.versus_failed and asked("versus_go") then
            return versus_start()
        end
        sides_refresh(this)
    end
end
-- The Versus screen (New game, Load) and the side screen.
function versus_build()
    state.versus = screen("ifs_gct_versus", "Versus", {{tag = "vs_new", string = "New game"}, {tag = "vs_load", string = "Load"}},
        {vs_new = "New game", vs_load = "Load"}, function(tag)
            if tag == "vs_new" then
                local setup = global("ifs_freeform_customsetup")
                if not setup then return message("The custom Galactic Conquest setup is not available.") end
                -- Registered before this script runs, so its launch is wrapped here, once.
                if not state.setup_wrapped then state.setup_wrapped = true; versus_setup(setup) end
                state.versus_setup = true
                push(setup)
            elseif tag == "vs_load" then invite("vs_load") end
        end, function(this)
            local enter = this.Enter
            -- Returning here from the custom setup stops keeping its launch.
            this.Enter = function(self, forward) state.versus_setup = nil; return enter(self, forward) end
        end)
    state.sides = screen("ifs_gct_sides", "Versus: pick your side",
        {{tag = "side1", string = "Side 1"}, {tag = "side2", string = "Side 2"}, {tag = "ready", string = "Ready"}},
        {side1 = "Side 1", side2 = "Side 2", ready = "Ready"}, sides_choose, function(this)
            local enter = this.Enter
            this.Enter = function(self, forward)
                enter(self, forward)
                state.versus_starting, state.versus_failed = nil, nil
                send("versus:sides")
                if state.versus_era == 1 or state.versus_era == 2 then send("versus:era:" .. state.versus_era) end
                sides_refresh(self)
            end
            this.Update = function(self, dt)
                if global("gIFShellScreenTemplate_fnUpdate") then gIFShellScreenTemplate_fnUpdate(self, dt) end
                local ok, problem = protected(sides_update, self)
                if not ok then send("versus:sides_error:" .. string.sub(tostring(problem), 1, 80)) end
            end
            -- Back ends the Versus game for both PCs.
            this.Input_Back = function(self)
                state.sides_open = nil
                send("menu:leave")
                ScriptCB_PopScreen()
            end
        end)
end

-- Versus notes from the fleet screen: the other PC's game is over, so this one shows the end too, or
-- this PC's turn has begun. No popup marks a hand-off; the other side's turn shows on the map.
function versus_note()
    if not versus() then return false end
    local main = global("ifs_freeform_main")
    if not main then return false end
    local winner = (asked("versus_over_1") and 1) or (asked("versus_over_2") and 2) or nil
    if winner and not state.over_shown then
        state.over_shown = true
        main.teamVictory = winner
        send("versus:end_shown:" .. winner)
        ScriptCB_PopScreen("ifs_freeform_main")
        ScriptCB_PushScreen("ifs_freeform_end")
        return true
    end
    -- The hand-off's patch gives this PC the turn (player_patch_body.lua). It starts as
    -- NextTurn starts one: the galaxy's main screen, then the fleet screen for the new side.
    local started = rawget(_G, "_GCT_versus_turn_start")
    if started then
        rawset(_G, "_GCT_versus_turn_start", nil)
        send("versus:turn_start:" .. tostring(started))
        ScriptCB_PopScreen("ifs_freeform_main")
        ScriptCB_PushScreen("ifs_freeform_fleet")
        return true
    end
    return false
end

-- Online goes after the stock buttons. The Remaster's list box shows an entry's string as its
-- text when it has no translation, so the string is the text.
local previous_list = custom_GetGCButtonList
local previous_pressed = custom_PressedGCButton
state.list = function(...)
    local list = previous_list(unpack_args(arg))
    if kind(list) == "table" then table.insert(list, {tag = "gct_online", string = "Online"}) end
    return list
end
-- The tab calls this for buttons it does not know. A true result would push ifs_freeform_main; Online
-- pushes its own screen, so it returns nothing.
state.pressed = function(tag, ...)
    if tag ~= "gct_online" then return previous_pressed(tag, unpack_args(arg)) end
    if state.online then
        ifelm_shellscreen_fnPlaySound("shell_menu_accept")
        push(state.online)
    else
        send("menu:unavailable")
    end
    return nil
end
-- AddIFScreen is wrapped until every screen this script changes has been registered.
state.add_original = AddIFScreen
-- After registration: builds the Online screens with the tab (the PC tab layouts exist by then), and
-- wraps the custom setup, the fleet screen and the pause menu as registration leaves them.
local function added(this, name, ...)
    if name == "ifs_sp_gc_main" and not state.tab then
        state.tab = true
        local ok, problem = protected(build)
        if ok then send("menu:screens") else send("menu:build_failed:" .. string.sub(tostring(problem), 1, 120)) end
    end
    -- Versus: the custom setup's launch, as registration and the Remaster leave it.
    if name == "ifs_freeform_customsetup" and kind(this) == "table" and not state.setup_wrapped then
        state.setup_wrapped = true
        versus_setup(this)
    end
    -- The friend's lock wraps what registration leaves, so its checks run first.
    if name == "ifs_freeform_fleet" and kind(this) == "table" and not state.fleet then
        state.fleet = true
        lock_fleet(this)
    end
    if name == "ifs_freeform_menu" and kind(this) == "table" and not state.pause then
        state.pause = true
        lock_menu(this)
        pause_invite(this)
    end
    -- The stock function is put back once every screen has been seen.
    if state.tab and state.card and state.fleet and state.pause and state.main and state.setup_wrapped and
        AddIFScreen == state.add then
        AddIFScreen = state.add_original
    end
    return unpack_args(arg)
end
-- Before registration: names the Online button, wraps the battle card, the galaxy and the AI
-- screen, and watches the menus for the friend's side.
state.add = function(this, name, ...)
    if name == "ifs_sp_gc_main" and kind(this) == "table" and kind(this.buttons) == "table"
        and kind(this.buttons.gct_online) == "table" then
        RoundIFButtonLabel_fnSetUString(this.buttons.gct_online, text("Online"))
    end
    if name == "ifs_freeform_battle_card" and kind(this) == "table" and not state.card then
        state.card = true
        wrap_card(this)
    end
    if name == "ifs_freeform_main" and state.returning and kind(this) == "table" then returning_enter(this) end
    if name == "ifs_freeform_main" and kind(this) == "table" and not state.main then
        state.main = true
        versus_main(this)
    end
    if name == "ifs_freeform_ai" and kind(this) == "table" and not state.ai then
        state.ai = true
        versus_ai(this)
    end

    if watched[name] and kind(this) == "table" then watch(this) end
    return added(this, name, state.add_original(this, name, unpack_args(arg)))
end
custom_GetGCButtonList = state.list
custom_PressedGCButton = state.pressed
AddIFScreen = state.add
-- Battles launch through enter_mission; battle.enter keeps the function it replaces.
if kind(ScriptCB_EnterMission) == "function" then
    battle.enter = ScriptCB_EnterMission
    ScriptCB_EnterMission = enter_mission
end
-- Exit to Windows from the menus and the galaxy: the DLL tells the other PC first, so its game leaves
-- at once (the DLL waits up to 0.5 s for the message to go).
if kind(ScriptCB_QuitToWindows) == "function" then
    local quit = ScriptCB_QuitToWindows
    ScriptCB_QuitToWindows = function(...)
        send("menu:closing")
        return quit(unpack_args(arg))
    end
end
-- The galaxy menu's Quit, its save prompt and the DLL's leaving all end in SetState("shell"), which
-- the Remaster wraps and which needs its settings.
if kind(SetState) == "function" then
    local set_state = SetState
    SetState = function(...)
        remaster_settings("state")
        return set_state(unpack_args(arg))
    end
end
-- The functions the DLL calls.
rawset(_G, "_GCT_battle_join", battle_join)
rawset(_G, "_GCT_battle_join_ready", join_ready)
rawset(_G, "_GCT_battle_note", battle_note)
rawset(_G, "_GCT_versus_card", versus_card)
rawset(_G, "_GCT_party_rows", party_rows)
-- Back from a shared battle: this game's own result is dropped, and returning_enter handles the
-- galaxy's Enter that the shell is about to run.
if asked("battle_return") then
    if global("ScriptCB_SetLastBattleVictoryValid") then protected(ScriptCB_SetLastBattleVictoryValid, false) end
    state.returning = true
    send("battle:return")
    -- Versus: the result this galaxy keeps, if any (returning_enter).
    state.keep = (asked("battle_keep_mine") and "mine") or (asked("battle_keep_theirs") and "theirs") or nil
    if state.keep then send("battle:keep:" .. state.keep) end
end
send("menu:installed")
return 6217431
