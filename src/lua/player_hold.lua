-- Shows the game's busy popup while the host waits for friends to load the galaxy.
-- Back cancels the wait.
local hold_state
do
    local ask = ScriptCB_IsFileExist
    local kind = type
    local mod = math.mod
    local showing = false
    -- The DLL answers 1 while it holds the galaxy for a joining friend.
    local function held()
        return ask("GCOBS:hold:ask") == 1
    end
    -- Called from the fleet screen's Update; acts every 15th frame while no popup is open. When the
    -- galaxy is not held, the same frames run the menu script's battle notes.
    hold_state = function(ticket, send)
        if showing or (ticket ~= 1 and mod(ticket, 15) ~= 0) then return end
        local popup = rawget(_G, "Popup_Busy")
        if kind(popup) ~= "table" or kind(popup.fnActivate) ~= "function" or popup.title == nil then return end
        if kind(IFText_fnSetUString) ~= "function" or kind(ScriptCB_tounicode) ~= "function" then return end
        if kind(ScriptCB_IsPopupOpen) == "function" and ScriptCB_IsPopupOpen() then return end
        if not held() then
            local note = rawget(_G, "_GCT_battle_note")
            if kind(note) == "function" then note() end
            return
        end
        showing = true
        -- Closes the popup and tells the DLL how the wait ended.
        local function close(how)
            showing = false
            popup:fnActivate(nil)
            send("hold:" .. how)
        end
        -- The popup text for how many friends' games are loading; nil when the count is unchanged.
        local loading, checks = nil, 0
        local function waiting_text()
            local now = 1
            for n = 3, 2, -1 do
                if ask("GCOBS:ask:hold_loading_" .. n) == 1 then now = n; break end
            end
            if now == loading then return nil end
            loading = now
            if now > 1 then return "Waiting for " .. now .. " friends' games to load the galaxy..." end
            return "Waiting for your friend's game to load the galaxy..."
        end
        -- Called every frame: 1 ends the wait, 0 keeps it; the text updates every 15th call.
        popup.fnCheckDone = function()
            if not held() then return 1 end
            checks = checks + 1
            if mod(checks, 15) == 0 then
                local update = waiting_text()
                if update then IFText_fnSetUString(popup.title, ScriptCB_tounicode(update)) end
            end
            return 0
        end
        popup.fnOnSuccess = function() close("released") end
        popup.fnOnFail = function() close("timeout") end
        -- Back has already closed the popup.
        popup.fnOnCancel = function()
            showing = false
            send("hold:cancel")
        end
        popup.bNoCancel = nil
        popup.fTimeout = 90  -- seconds; running out counts as a failure
        popup.fMinTimeout = nil
        popup:fnActivate(1)
        -- No elapsed-time line: the hold has its own timeout, and the line would run into the text.
        popup.bCallerSetsString = 1
        if kind(popup.BusyTimeStr) == "table" then IFText_fnSetUString(popup.BusyTimeStr, ScriptCB_tounicode("")) end
        IFText_fnSetUString(popup.title, ScriptCB_tounicode(waiting_text()))
        send("hold:shown")
    end
end
