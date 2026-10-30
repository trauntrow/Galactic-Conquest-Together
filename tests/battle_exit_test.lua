-- Tests src/lua/battle_exit.lua, which wraps a shared battle's ways out so the DLL hears of them first.
local function read(path) local f=assert(io.open(path,'rb')); local s=f:read('*a'); f:close(); return s end
local source=read(arg and arg[1] or 'src/lua/battle_exit.lua')

-- A fresh battle state whose stand-ins record each call in events.
local events
local function battle_state()
    events={}
    _G._GCT_exit_wrapped=nil
    _G.rema_database=nil
    _G.__thisIsGC__=nil
    ScriptCB_IsFileExist=function(text) table.insert(events,'ask '..text); return 0 end
    ScriptCB_CloseNetShell=function() table.insert(events,'close net shell') end
    ScriptCB_QuitToWindows=function(...) table.insert(events,'quit n='..arg.n..' '..tostring(arg[1])) end
    ScriptCB_QuitToShell=function(...) table.insert(events,'to shell n='..arg.n..' '..tostring(arg[1])) end
    ScriptCB_QuitFromStats=function(...) table.insert(events,'from stats n='..arg.n) end
    ScriptCB_IsMetagameStateSaved=function() return true end
end
-- What the pause menu's Exit does after Yes; both functions are looked up as globals at each call.
local function exit_popup_done()
    ScriptCB_CloseNetShell()
    ScriptCB_QuitToWindows()
end
local function said() return table.concat(events,',') end

-- Exit to Windows: the DLL is told first, then the stock call runs. The battle's starting state is
-- reported once, at install.
battle_state()
rema_database={data={}}
__thisIsGC__=true
local stock=ScriptCB_QuitToWindows
assert(assert(loadstring(source))()==6217431 and ScriptCB_QuitToWindows~=stock and _GCT_exit_wrapped)
assert(said()=='ask GCOBS:diag:battle_remaster:table:saved:boolean',said())
events={}
exit_popup_done()
assert(said()=='close net shell,ask GCOBS:menu:closing,quit n=0 nil',said())
-- The stock call gets the caller's arguments, nils included.
events={}
ScriptCB_QuitToWindows(nil,'x')
assert(said()=='ask GCOBS:menu:closing,quit n=2 nil',said())
-- Quit and the scoreboard's Quit report the Remaster settings and the galaxy save, then run the
-- stock call. Quit first tells the DLL that this player quit.
events={}
ScriptCB_QuitToShell()
assert(said()=='ask GCOBS:battle:quit_own,ask GCOBS:diag:battle_quit:shell:table:saved,to shell n=0 nil',said())
events={}
rema_database=nil
ScriptCB_IsMetagameStateSaved=function() return false end
ScriptCB_QuitFromStats(1)
assert(said()=='ask GCOBS:diag:battle_quit:stats:nil:none,from stats n=1',said())
-- Running the script again in the same Lua state wraps nothing twice.
local wrapped,shell_quit=ScriptCB_QuitToWindows,ScriptCB_QuitToShell
assert(assert(loadstring(source))()==6217431 and ScriptCB_QuitToWindows==wrapped and ScriptCB_QuitToShell==shell_quit)
events={}
ScriptCB_QuitToWindows()
assert(said()=='ask GCOBS:menu:closing,quit n=0 nil')
-- A missing or failing DLL channel never stops a quit.
battle_state()
assert(assert(loadstring(source))()==6217431)
ScriptCB_IsFileExist=nil  -- the wrapper keeps its own reference
events={}
ScriptCB_QuitToWindows()
assert(said()=='ask GCOBS:menu:closing,quit n=0 nil')
battle_state()
local failing=function() error('no channel') end
ScriptCB_IsFileExist=failing
assert(assert(loadstring(source))()==6217431)
events={}
ScriptCB_QuitToWindows()
ScriptCB_QuitToShell('x')
assert(said()=='quit n=0 nil,to shell n=1 x',said())
-- Missing pieces are reported as nil, and a failing save check as unknown.
battle_state()
ScriptCB_QuitFromStats=nil
ScriptCB_IsMetagameStateSaved=function() error('no state') end
assert(assert(loadstring(source))()==6217431 and ScriptCB_QuitFromStats==nil)
assert(said()=='ask GCOBS:diag:battle_remaster:nil:unknown:nil',said())
-- Without the functions it needs, the script wraps nothing and returns 0.
for _,missing in ipairs({'ScriptCB_QuitToWindows','ScriptCB_IsFileExist','unpack','pcall'}) do
    battle_state()
    local kept=_G[missing]
    _G[missing]=nil
    local before,shell_before=ScriptCB_QuitToWindows,ScriptCB_QuitToShell
    assert(assert(loadstring(source))()==0 and ScriptCB_QuitToWindows==before and ScriptCB_QuitToShell==shell_before and
        not _GCT_exit_wrapped,missing)
    assert(table.getn(events)==0)
    _G[missing]=kept
end
-- The Remaster's wrappers put its settings in front of the galaxy save each time they run. However
-- often the battle is quit, one copy stays in front of the save's 26 values.
battle_state()
local db={isRemaDatabase=true}
rema_database=db
local store={n=26}
for i=1,26 do store[i]='g'..i end
ScriptCB_LoadMetagameState=function() return unpack(store) end
ScriptCB_SaveMetagameState=function(...) store=arg end
local function first26(t)
    return t[1],t[2],t[3],t[4],t[5],t[6],t[7],t[8],t[9],t[10],t[11],t[12],t[13],t[14],t[15],t[16],t[17],t[18],
        t[19],t[20],t[21],t[22],t[23],t[24],t[25],t[26]
end
-- Stand-ins for the Remaster's wrappers around Quit and the scoreboard's Quit.
for _,name in ipairs({'ScriptCB_QuitToShell','ScriptCB_QuitFromStats'}) do
    local inner=_G[name]
    _G[name]=function(...)
        if ScriptCB_IsMetagameStateSaved() then
            ScriptCB_SaveMetagameState(rema_database,first26({ScriptCB_LoadMetagameState()}))
        elseif rema_database then
            ScriptCB_SaveMetagameState(rema_database)
        end
        return inner(unpack(arg))
    end
end
assert(assert(loadstring(source))()==6217431)
local function one_copy_kept()
    assert(store.n==27 and store[1]==db,'n='..tostring(store.n))
    for i=1,26 do assert(store[i+1]=='g'..i,i) end
end
events={}
ScriptCB_QuitToShell()
one_copy_kept()
assert(said()=='ask GCOBS:battle:quit_own,ask GCOBS:diag:battle_quit:shell:table:saved,to shell n=0 nil',said())
events={}
ScriptCB_QuitToShell()
one_copy_kept()
assert(said()=='ask GCOBS:battle:quit_own,ask GCOBS:diag:battle_quit:shell:table:saved:again,to shell n=0 nil',said())
events={}
ScriptCB_QuitFromStats()
one_copy_kept()
assert(said()=='ask GCOBS:diag:battle_quit:stats:table:saved:again,from stats n=0',said())
-- A save that cannot be read is left alone, and the Quit goes on.
battle_state()
ScriptCB_LoadMetagameState=function() error('no save') end
ScriptCB_SaveMetagameState=function() table.insert(events,'saved') end
assert(assert(loadstring(source))()==6217431)
events={}
ScriptCB_QuitToShell()
assert(said()=='ask GCOBS:battle:quit_own,ask GCOBS:diag:battle_quit:shell:nil:saved:unchecked,to shell n=0 nil',said())
ScriptCB_LoadMetagameState,ScriptCB_SaveMetagameState=nil,nil
print('battle_exit_test PASS: Exit, Quit and scoreboard wrappers')
