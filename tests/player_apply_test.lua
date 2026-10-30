-- Tests the generated Player apply scripts against galaxies rebuilt from recorded sessions. Arguments:
-- the scripts' folder, then the first, second, host, ai-start, ai-moved and host-again .bin files.
local output,first_path,second_path=arg[1],arg[2],arg[3]
local function read(path) local f=assert(io.open(path,'rb')); local s=f:read('*a'); f:close(); return s end
local first,second=read(first_path),read(second_path)
local host_turn,ai_start,ai_moved,host_again=read(arg[4]),read(arg[5]),read(arg[6]),read(arg[7])

-- Stand-ins for the game: one local profile, no battle result pending.
setmetatable(_G,{__index=function(t,k)
    if string.sub(k,1,19)=='ifs_freeform_start_' then return function() end end
end})
local profile='LocalPlayer'
ScriptCB_GetNumCameras=function() return 1 end
ScriptCB_GetLastBattleVictory=function() return -1 end
ScriptCB_GetProfileName=function(i) assert(i==1); return profile end
ScriptCB_ununicode=function(v) return v end
ifs_freeform_main={}
ifs_freeform_load={}
ifs_freeform_load_LoadMetagameSuccess=function() error('unexpected dispatch') end
local stored,saves=nil,0
ScriptCB_SaveMetagameState=function(...) stored=arg; saves=saves+1 end
ScriptCB_LoadMetagameState=function() error('preparation must not load') end

-- First update, from the main menu.
local prepare=dofile(output..'/player_prepare.lua')
assert(prepare(first)==6217431 and saves==1 and stored.n==26)
-- The save's third value maps this PC's profile to the host's human team.
local count=0
for name,team in pairs(stored[3]) do count=count+1; assert(name==profile and team==stored[13]) end
assert(count==1)
for _,i in ipairs({15,19,21,22}) do assert(stored[i]==nil) end

-- What the game assigns from the save while loading it.
local fields={[2]='scenario',[6]='planetTeam',[7]='planetFleet',[9]='teamResources',
    [13]='playerTeam',[14]='turnNumber',[21]='winnerTeam'}
for index,field in pairs(fields) do ifs_freeform_main[field]=stored[index] end
local purchases={[10]='ifs_purchase_unit_owned',[11]='ifs_purchase_tech_cards',[12]='ifs_purchase_tech_using'}
for index,field in pairs(purchases) do _G[field]=stored[index] end
ScriptCB_LoadMetagameState=function() return unpack(stored) end

-- Readback and live-model checks pass for the applied state only.
local verify=dofile(output..'/player_verify.lua')
assert(verify(first)==6217431 and saves==1)
local live=dofile(output..'/player_live.lua')
assert(live(first)==6217431)
assert(not pcall(verify,second))
local credits=ifs_freeform_main.teamResources
ifs_freeform_main.teamResources={[1]=1}
assert(not pcall(live,first))       -- the guest changed a campaign field
ifs_freeform_main.teamResources=credits
ifs_freeform_main.lastSelected='navigation only'
assert(live(first)==6217431)

-- Seamless update: the patch script brings the galaxy to the next revision in place. Stand-ins
-- track every fleet model the engine would hold.
local alive,models=0,{}
CreateEntity=function(class,matrix) local e={class=class,matrix=matrix}; models[e]=true; alive=alive+1; return e end
DeleteEntity=function(e) assert(models[e]); models[e]=nil; alive=alive-1 end
local main=ifs_freeform_main
main.fleetClass={[1]='class1',[2]='class2'}
main.modelMatrix=setmetatable({},{__index=function(t,planet) return {[1]='m1:'..planet,[2]='m2:'..planet} end})
-- Fleet models a galaxy shows: two on a planet whose planetFleet is 0 (both sides), else one.
local function pairs_of(fleets)
    local n=0
    for planet,team in pairs(fleets) do n=n+(team==0 and 2 or 1) end
    return n
end
main.fleetPtr={[1]={},[2]={}}  -- as Enter() builds it after the first load
for planet,team in pairs(main.planetFleet) do
    for _,t in ipairs(team==0 and {1,2} or {team}) do
        main.fleetPtr[t][planet]=CreateEntity(main.fleetClass[t],main.modelMatrix[planet][t])
    end
end
local refreshed,selected=nil,nil
ifs_freeform_fleet={player='hud',info='panel',zoomLevel=1}
main.UpdatePlayerText=function(self,info) assert(self==main); refreshed=info end
-- Like the game's: records the selection and moves the cursor and the camera.
main.SelectPlanet=function(self,info,planet)
    assert(self==main); selected={info,planet}; self.planetSelected=planet; self.planetNext=planet
end
-- What SetActiveTeam reads, and stand-ins that record the calls.
main.teamCode={[1]='all',[2]='imp'}
main.teamController={[1]=0}
ScriptCB_SetHotController=function() end
ifs_freeform_purchase_unit={SetActiveSide=function() end}
local activations,zooms={},{}
main.SetActiveTeam=function(self,team) assert(self==main); table.insert(activations,team); self.playerTeam=team end
main.SetZoom=function(self,level) assert(self==main); table.insert(zooms,level) end
-- The host's values for the second revision, for the checks below.
local host
do
    local save=ScriptCB_SaveMetagameState
    ScriptCB_SaveMetagameState=function(...) host=arg end
    assert(prepare(second)==6217431)
    ScriptCB_SaveMetagameState=save
end
local patch=dofile(output..'/player_patch.lua')
-- A refusal changes nothing: here a planet has no model position.
local positions=main.modelMatrix
main.modelMatrix={}
assert(not pcall(patch,second))
assert(alive==pairs_of(main.planetFleet) and refreshed==nil and live(first)==6217431)
main.modelMatrix=positions
-- The update applies in place, saving and loading nothing, and the fleet models match the new galaxy.
assert(patch(second)==6217431 and saves==1)
assert(live(second)==6217431 and not pcall(live,first))
assert(alive==pairs_of(main.planetFleet) and refreshed=='hud')
for planet,team in pairs(main.planetFleet) do
    for _,t in ipairs(team==0 and {1,2} or {team}) do
        local e=main.fleetPtr[t][planet]
        assert(models[e] and e.class=='class'..t and e.matrix=='m'..t..':'..planet)
    end
end
-- The camera follows the host's selection, with the fleet screen's info panel.
assert(type(host[8])=='string' and selected[1]=='panel' and selected[2]==host[8])
-- An update where no fleet moved leaves every model in place.
local kept=main.fleetPtr
selected=nil
assert(patch(second)==6217431 and main.fleetPtr==kept and selected[2]==host[8])
-- Back to the first revision for the reload checks below.
assert(patch(first)==6217431 and live(first)==6217431)

-- Second update, from the galaxy, after the host's battle.
local repeat_prepare=dofile(output..'/player_repeat_prepare.lua')
assert(repeat_prepare(second)==6217431 and saves==2)
for _,i in ipairs({15,19,21,22}) do assert(stored[i]==nil) end
profile='Friend'                    -- another PC fills in its own profile
assert(repeat_prepare(second)==6217431 and saves==3)
for name in pairs(stored[3]) do assert(name=='Friend') end
ifs_freeform_main.scenario='other'  -- a different campaign is never replaced
assert(not pcall(repeat_prepare,second) and saves==3)
ScriptCB_GetNumCameras=function() return 2 end
ifs_freeform_main.scenario=stored[2]
assert(not pcall(repeat_prepare,second) and saves==3) -- split screen
ScriptCB_GetNumCameras=function() return 1 end
print('player_apply_test PASS: producer 6 load, patch, readback and refusals')

-- Producer 7: the host's turn, loaded as the first update would.
profile='LocalPlayer'
assert(prepare(host_turn)==6217431)
for index,field in pairs(fields) do main[field]=stored[index] end
for index,field in pairs(purchases) do _G[field]=stored[index] end
main.fleetPtr={[1]={},[2]={}}
for planet,team in pairs(main.planetFleet) do
    for _,t in ipairs(team==0 and {1,2} or {team}) do
        main.fleetPtr[t][planet]=CreateEntity(main.fleetClass[t],main.modelMatrix[planet][t])
    end
end
assert(live(host_turn)==6217431 and main.playerTeam==1)
-- The AI's turn: a load refuses it, since Enter would start the AI on this PC, and nothing is saved.
local before=saves
assert(not pcall(prepare,ai_start) and not pcall(repeat_prepare,ai_start) and saves==before)
-- The patch mirrors it: the AI is the active team, the camera shows the AI fleet's planet, and the
-- cursor marks its destination.
selected=nil
assert(patch(ai_start)==6217431 and live(ai_start)==6217431)
assert(activations[1]==2 and table.getn(activations)==1 and main.playerTeam==2)
assert(selected[1]=='panel' and selected[2]=='cor' and main.planetSelected=='cor' and main.planetNext=='star17')
assert(table.getn(zooms)==0)                -- level 1 already, as ifs_freeform_ai Enter sets it
-- The AI fleet moves: the models are rebuilt and the camera follows it.
local moved=main.fleetPtr
assert(patch(ai_moved)==6217431 and live(ai_moved)==6217431)
assert(main.fleetPtr~=moved and main.fleetPtr[2]['star17'] and not main.fleetPtr[2]['cor'])
assert(selected[2]=='star17' and table.getn(activations)==1)
-- Back to the host's turn: its team and its planet again.
assert(patch(host_again)==6217431 and live(host_again)==6217431)
assert(activations[2]==1 and main.playerTeam==1 and selected[2]=='pol' and main.planetNext=='pol')
assert(repeat_prepare(host_again)==6217431)
for name,team in pairs(stored[3]) do assert(name==profile and team==1) end
-- The friend zooms out on the host's turn; the patch brings the host's zoom level back.
ifs_freeform_fleet.zoomLevel=0
assert(patch(host_again)==6217431 and zooms[1]==1 and ifs_freeform_fleet.zoomLevel==1)
-- After a battle the host's rebuilt fleet screen has no zoom level, so no hint [3] arrives; the AI's
-- turn still shows level 1.
local level='\3\0\0\64\64\3\0\0\128\63'     -- [3] = 1, as float32 numbers
local at=string.find(ai_start,level,1,true)
assert(at and not string.find(ai_start,level,at+1,true))
local no_zoom=string.sub(ai_start,1,at-1)..string.sub(ai_start,at+string.len(level))
ifs_freeform_fleet.zoomLevel=0                -- the friend zoomed out on the host's turn
assert(patch(no_zoom)==6217431 and main.playerTeam==2)
assert(zooms[2]==1 and table.getn(zooms)==2 and ifs_freeform_fleet.zoomLevel==0)
-- A galaxy the friend changed anyway (a fleet moved, credits spent, a unit bought) is out of step,
-- and the patch still brings it to the host's revision.
assert(patch(host_again)==6217431 and live(host_again)==6217431)
local spent={}
for t,credits in pairs(main.teamResources) do spent[t]=credits-10 end
main.teamResources=spent
ifs_purchase_unit_owned={}
local from,team,to
for planet,t in pairs(main.planetFleet) do if t~=0 then from,team=planet,t end end
for planet in pairs(main.planetTeam) do if main.planetFleet[planet]==nil then to=planet end end
assert(from and to)
main.planetFleet[to],main.planetFleet[from]=team,nil
main.fleetPtr[team][to],main.fleetPtr[team][from]=main.fleetPtr[team][from],nil
assert(not pcall(live,host_again))
local saved=saves
assert(patch(host_again)==6217431 and live(host_again)==6217431 and saves==saved)
local shown=0
for _,team_models in pairs(main.fleetPtr) do
    for _,e in pairs(team_models) do assert(models[e]); shown=shown+1 end
end
assert(shown==pairs_of(main.planetFleet) and main.fleetPtr[team][from] and not main.fleetPtr[team][to])
print('player_apply_test PASS: producer 7 AI turn, camera, zoom and catch-up')

-- Versus: this PC plays the side the sender does not, a load may land on the other side's turn, and
-- a hand-off starts this PC's turn the way NextTurn does.
local versus_on=true
ScriptCB_IsFileExist=function(text) if text=='GCOBS:ask:versus' and versus_on then return 1 end return 0 end
local screens={}
ScriptCB_PopScreen=function(name) table.insert(screens,'pop '..name) end
ScriptCB_PushScreen=function(name) table.insert(screens,'push '..name) end
_GCT_versus_turn_start=nil
-- The first load arrives on the other side's turn and is prepared with this PC's profile on side 2.
saved=saves
assert(prepare(host_turn)==6217431 and saves==saved+1 and stored[13]==1)
for name,t in pairs(stored[3]) do assert(name==profile and t==2) end
assert(repeat_prepare(host_turn)==6217431)
for name,t in pairs(stored[3]) do assert(name==profile and t==2) end
-- The galaxy as that load leaves it.
for index,field in pairs(fields) do main[field]=stored[index] end
for index,field in pairs(purchases) do _G[field]=stored[index] end
assert(patch(host_turn)==6217431 and main.playerTeam==1 and table.getn(screens)==0)
-- The host hands over: side 2's turn is this PC's. The patch marks it for the fleet screen's notes
-- to start, with no screen change inside the patch.
assert(patch(ai_start)==6217431 and live(ai_start)==6217431 and main.playerTeam==2)
assert(table.getn(screens)==0 and _GCT_versus_turn_start==main.turnNumber)
_GCT_versus_turn_start=nil
-- More of this PC's own turn as it plays: no new turn start.
assert(patch(ai_moved)==6217431 and _GCT_versus_turn_start==nil)
-- This PC hands back: the other side's turn shows, and nothing is started.
assert(patch(host_again)==6217431 and main.playerTeam==1 and _GCT_versus_turn_start==nil)
-- A battle leaves result screen fields behind, which the live check refuses; the next hand-off
-- clears them as a load does, so it applies in place.
main.curScreen,main.winnerTeam,main.fleetBattle='battle_card_1',1,false
assert(not pcall(live,host_again))
assert(patch(ai_start)==6217431 and live(ai_start)==6217431 and main.playerTeam==2)
assert(main.curScreen==nil and main.winnerTeam==nil and main.fleetBattle==nil)
assert(_GCT_versus_turn_start==main.turnNumber)
_GCT_versus_turn_start=nil
assert(patch(host_again)==6217431 and main.playerTeam==1)
-- The other side's turn, watched as it is played: each revision patches in place with no turn start,
-- and the camera and zoom follow the other player's.
ifs_freeform_fleet.zoomLevel=0                 -- this player zoomed out while watching
local zoom_count=table.getn(zooms)
selected=nil
assert(patch(host_again)==6217431 and main.playerTeam==1 and _GCT_versus_turn_start==nil)
assert(selected[2]=='pol' and zooms[zoom_count+1]==1 and ifs_freeform_fleet.zoomLevel==1)
local zero='\3\0\0\64\64\3\0\0\0\0'          -- [3] = 0
local at3=string.find(host_again,level,1,true)
assert(at3 and not string.find(host_again,level,at3+1,true))
local watched_out=string.sub(host_again,1,at3-1)..zero..string.sub(host_again,at3+string.len(level))
assert(patch(watched_out)==6217431 and zooms[zoom_count+2]==0 and ifs_freeform_fleet.zoomLevel==0)
assert(main.playerTeam==1 and _GCT_versus_turn_start==nil)
-- Co-op is unchanged: the friend's profile on the host's side, the AI's turn refused for a load, and
-- no turn start.
versus_on=false
assert(prepare(host_turn)==6217431)
for name,t in pairs(stored[3]) do assert(name==profile and t==1) end
saved=saves
assert(not pcall(prepare,ai_start) and saves==saved)
assert(patch(ai_start)==6217431 and table.getn(screens)==0 and _GCT_versus_turn_start==nil)
ScriptCB_IsFileExist,ScriptCB_PopScreen,ScriptCB_PushScreen=nil,nil,nil
print('player_apply_test PASS: Versus sides, hand-offs and watched turns')
