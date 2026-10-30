-- Tests the co-op ready check (src/lua/player_hold.lua) with the screen observer, as the Player build
-- embeds them, against a stand-in for the game's busy popup.
local function read(path) local f=assert(io.open(path,'rb')); local s=f:read('*a'); f:close(); return s end
local source="local capture_state = nil\n"..read('src/lua/player_hold.lua').."\n"..read('src/lua/screen_observer.lua')

-- The DLL's side: whether the galaxy is held, how many friends' games are loading, and what is sent.
local held,asks,sent,loading=false,0,{},0
ScriptCB_IsFileExist=function(text)
    if text=='GCOBS:hold:ask' then asks=asks+1; if held then return 1 end return 0 end
    -- How many friends' games the hold still waits for.
    local _,_,count=string.find(text,'^GCOBS:ask:hold_loading_(%d)$')
    if count then if tonumber(count)==loading then return 1 end return 0 end
    table.insert(sent,text)
    return 0
end
local function said(text)
    for _,s in ipairs(sent) do if s==text then return true end end
    return false
end
-- Empty galaxy screens for the observer to wrap.
local function nothing() end
AddIFScreen=function() end
ifs_freeform_main={Enter=nothing,Exit=nothing,Update=nothing,SelectPlanet=nothing}
ifs_freeform_fleet={Enter=nothing,Exit=nothing,Update=nothing,Input_Accept=nothing,Input_Back=nothing}

-- The busy popup: activating needs all four callbacks, and closing clears bCallerSetsString, without
-- which Update writes the elapsed time in BusyTimeStr.
Popup_Busy={title={},BusyTimeStr={text='0:42'}}
function Popup_Busy:fnActivate(on)
    if on then
        assert(self.fnCheckDone and self.fnOnSuccess and self.fnOnFail and self.fnOnCancel)
        self.active=true; self.fBusyTime=0
    else self.active=false; self.bCallerSetsString=nil end
end
-- One popup frame: polls fnCheckDone and counts fTimeout as a failure.
local function popup_update(dt)
    local p=Popup_Busy
    local status=p.fnCheckDone()
    assert(type(status)=='number')
    p.fBusyTime=p.fBusyTime+dt
    if p.fTimeout and p.fBusyTime>p.fTimeout then status=-1; p.fTimeout=nil end
    if status>0.5 then p.fnOnSuccess() elseif status< -0.5 then p.fnOnFail() end
end
-- Back closes the popup and cancels unless bNoCancel is set.
local function popup_back()
    if not Popup_Busy.bNoCancel then Popup_Busy:fnActivate(nil); Popup_Busy.fnOnCancel() end
end
local other_popup=false
ScriptCB_IsPopupOpen=function() return Popup_Busy.active or other_popup end
IFText_fnSetUString=function(object,text) object.text=text end
ScriptCB_tounicode=function(s) return s end

assert(assert(loadstring(source))()==6217431)
-- Runs n frames of the fleet screen.
local ticks=0
local function frames(n) for _=1,n do ticks=ticks+1; ifs_freeform_fleet.Update(ifs_freeform_fleet,0.016) end end

-- No friend joining: the fleet screen asks on its first frame and every 15th.
frames(60)
assert(asks==5 and not Popup_Busy.active and not said('GCOBS:hold:shown'))
-- A friend's game is loading: the popup covers the galaxy within 15 frames.
held=true
frames(15)
assert(Popup_Busy.active and said('GCOBS:hold:shown'))
assert(string.find(Popup_Busy.title.text,'friend',1,true) and Popup_Busy.bNoCancel==nil and Popup_Busy.fTimeout==90)
-- No elapsed-time line, which would run into the text and the Cancel button.
assert(Popup_Busy.bCallerSetsString==1 and Popup_Busy.BusyTimeStr.text=='')
local before=asks
frames(60)                                   -- shown once; the popup asks from now on
assert(asks==before)
popup_update(0.016)
assert(Popup_Busy.active and asks==before+1)
-- The friend's game shows the galaxy: the popup closes on its next frame.
held=false
popup_update(0.016)
assert(not Popup_Busy.active and said('GCOBS:hold:released'))
-- Back cancels the wait, and the DLL stops holding.
sent={}
held=true
frames(15)
assert(Popup_Busy.active)
popup_back()
assert(not Popup_Busy.active and said('GCOBS:hold:cancel'))
held=false
frames(30)
assert(not Popup_Busy.active)
-- The popup gives up after fTimeout.
sent={}
held=true
frames(15)
for _=1,91 do popup_update(1) end
assert(not Popup_Busy.active and said('GCOBS:hold:timeout'))
-- Another popup is never covered; the hold waits for it to close.
held=true
other_popup=true
sent={}
frames(45)
assert(not Popup_Busy.active and not said('GCOBS:hold:shown'))
other_popup=false
frames(15)
assert(Popup_Busy.active)
held=false; popup_update(0.016)
-- Without the busy popup nothing happens and nothing fails.
local busy=Popup_Busy
Popup_Busy=nil
held=true; sent={}
frames(30)
assert(not said('GCOBS:hold:shown'))
Popup_Busy=busy; held=false
-- With several friends the popup says how many games are still loading, checked every 15th frame.
loading=3; held=true
frames(15)
assert(Popup_Busy.active and Popup_Busy.title.text=="Waiting for 3 friends' games to load the galaxy...")
loading=2
for _=1,15 do popup_update(0.016) end
assert(Popup_Busy.title.text=="Waiting for 2 friends' games to load the galaxy...")
loading=1                                    -- the DLL answers only 2 and 3
for _=1,15 do popup_update(0.016) end
assert(Popup_Busy.title.text=="Waiting for your friend's game to load the galaxy...")
held=false; popup_update(0.016)
assert(not Popup_Busy.active)
loading=0
-- When the galaxy is not held, the same frames give the battle note (_GCT_battle_note) its turn.
local notes=0
_GCT_battle_note=function() notes=notes+1 end
frames(30)
assert(notes==2)
-- Not while another popup is open, nor while the galaxy is held.
other_popup=true
frames(30)
assert(notes==2)
other_popup=false
held=true
frames(15)
assert(Popup_Busy.active and notes==2)
held=false; popup_update(0.016)
frames(15)
assert(notes==3)
-- A failing note never breaks the fleet screen; without one nothing fails.
_GCT_battle_note=function() error('broken note') end
frames(15)
_GCT_battle_note=nil
frames(15)
print('player_hold_test PASS: ready check popup, loading count and battle notes')
