-- Tests src/lua/campaign_capture.lua: the galaxy state records it sends, their triggers and bounds.
local f=assert(io.open("src/lua/campaign_capture.lua","r"))
local source=f:read("*a") f:close()
-- Loads the capture script, as producer 6 or the given capture_version, and returns capture_state.
local function make(version)
    local prefix = version and ("local capture_version = " .. version .. "\n") or ""
    return assert(loadstring(prefix .. source .. "\nreturn capture_state"))()
end
-- A galaxy with every kind of saved value, including values that cannot be represented.
local function setup()
    ifs_freeform_main={scenario="test:with\nquotes'",turnNumber=3,playerTeam=1,planetNext="cor",
        planetTeam={cor=1,geo=2},planetFleet={cor=1,geo=0},planetBase={cor=1},
        teamResources={[1]=123,[2]=456},lastSelected={[1]="cor"},lastFleet={[1]="cor"},
        -- Side and slot candidates: a scalar, maps, a nested table, a function and an absent field.
        playerSide=1,otherSide=2,controllerTeam={[1]=1,[2]=0},
        controllerPlayer={[1]="p1"},teamCode={[1]="all",[2]="imp"},
        teamName=print,fleetPtr={[1]={a=1,b=2},[2]=print},
        -- The other saved values: booleans, a string, an absent field, filled maps and an empty one.
        custom=false,launchMission="cor1g_con",activeBonus=nil,
        fleetBattle={planet="cor"},recentPlanets={"cor","geo"},
        planetResources={cor=5,geo=7},battleResources={},soakMode=false}
    ifs_freeform_fleet={turnNumber=3,planetStart="cor",planetNext="geo"}
    ifs_purchase_unit_owned={1}
    ifs_purchase_tech_cards={}
    ifs_purchase_tech_using=nil
end
-- Collects the records sent; each must fit in 200 characters.
local messages={}
local function send(s)
    assert(string.len(s)<=200)
    table.insert(messages,s)
end
local function find(text)
    for i=1,table.getn(messages) do
        if string.find(messages[i],text,1,true) then return messages[i] end
    end
    return nil
end
-- Counts the snapshots begun so far.
local function begins()
    local n=0
    for i=1,table.getn(messages) do
        if string.find(messages[i],"^state:begin:") then n=n+1 end
    end
    return n
end
-- Runs capture tickets first to last.
local function run(capture,first,last)
    for t=first,last do capture(2,t,send) end
end
-- The record count announced by the first producer 6 begin record.
local function record_count()
    local line=find("state:begin:6:")
    local _,_,count=string.find(line,"^state:begin:6:%d+:(%d+):")
    return tonumber(count)
end

-- One snapshot at the first sampled ticket, then nothing while the galaxy is unchanged.
setup()
local capture=make()
run(capture,1,29)
assert(begins()==1,"expected one baseline snapshot")
-- 6 scalars, 6 map markers, 9 map entries, 27 records for the 15 probed fields, 3 for the fleet
-- screen and 4 for the globals.
assert(record_count()==55,"unexpected record count: " .. record_count())
assert(find("state:begin:6:1:55:2:1:0"),"expected a version 6 envelope")
assert(messages[table.getn(messages)]=="state:end:1:55","expected a complete first transaction")
-- The side and slot fields keep their shapes and kinds.
assert(find("playerSide:z:-:i:1"),"expected a scalar side value")
assert(find("teamCode:i:1:s:616C6C"),"expected a typed team map")
assert(find("teamName:z:-:u:66756E6374696F6E"),"expected an unsupported kind report")
assert(find("fleetPtr:i:1:c:"),"expected a nested composition summary")
-- The other saved values: booleans, an absent field, and an empty map that sends only its marker.
assert(find("custom:z:-:b:0"),"expected a Boolean false")
assert(find("soakMode:z:-:b:0"),"expected a Boolean false")
assert(find("launchMission:z:-:s:636F7231675F636F6E"),"expected a mission string")
assert(find("activeBonus:z:-:z:-"),"expected an absent field as nil")
assert(find("fleetBattle:s:706C616E6574:s:636F72"),"expected a string-keyed battle entry")
assert(find("recentPlanets:i:1:s:636F72"),"expected an integer-keyed recent planet")
assert(find("planetResources:s:636F72:i:5"),"expected a per-planet resource")
assert(find("battleResources:z:-:t:-"),"expected an empty map marker")
assert(not find("battleResources:i:"),"an empty map has no entries")
-- The fleet screen and the globals, under prefixed names.
assert(find("fleet.turnNumber:z:-:i:3"),"expected the fleet screen turn")
assert(find("fleet.planetStart:z:-:s:636F72"),"expected the staged move origin")
assert(find("fleet.planetNext:z:-:s:67656F"),"expected the staged move target")
assert(find("global.ifs_purchase_unit_owned:i:1:i:1"),"expected a purchase entry")
assert(find("global.ifs_purchase_tech_cards:z:-:t:-"),"expected an empty purchase map")
assert(find("global.ifs_purchase_tech_using:z:-:z:-"),"expected an absent global as nil")
run(capture,30,60)
assert(begins()==1,"unchanged samples must not emit a snapshot")

-- The main table's planetNext is only the selection cursor, so a change sends nothing.
ifs_freeform_main.planetNext="geo"
run(capture,61,90)
assert(begins()==1,"a planetNext change alone must not emit a snapshot")

-- The fleet screen's planetNext is the staged move target, so a change sends a snapshot.
ifs_freeform_fleet.planetNext="tat"
run(capture,91,129)
assert(begins()==2,"a staged move target change must emit a snapshot")
assert(find("state:begin:6:2:55:2:120:3"),"expected three skipped samples reported")
assert(find("fleet.planetNext:z:-:s:746174"),"expected the changed staged target")

-- A global purchase table is campaign state, so a change sends a snapshot.
ifs_purchase_tech_using=1
run(capture,130,169)
assert(begins()==3,"a global change must emit a snapshot")
assert(find("global.ifs_purchase_tech_using:z:-:i:1"),"expected the changed global")
assert(messages[table.getn(messages)]=="state:end:3:55","expected a complete third transaction")
-- Keeps the records in artifacts/ as a channel log.
local output=assert(io.open("artifacts/campaign-capture-test-v6.log","w"))
for i=1,table.getn(messages) do output:write("TEST CHANNEL GCOBS:" .. messages[i] .. "\n") end
output:close()

-- Values are copied at the snapshot: a change made after its first records are sent never shows.
setup()
capture=make() messages={}
run(capture,1,1)
ifs_freeform_main.planetResources.cor=99
run(capture,2,9)
assert(find("planetResources:s:636F72:i:5"),"expected the copied resource value")
assert(not find("planetResources:s:636F72:i:99"),"post-capture mutation must not be emitted")

-- A missing fleet screen is reported per field, not as a failure of the whole capture.
setup()
ifs_freeform_fleet=nil
capture=make() messages={}
run(capture,1,9)
assert(find("fleet.turnNumber:z:-:e:534F55524345"),"expected a per-field source error")
assert(find("fleet.planetNext:z:-:e:534F55524345"),"expected a per-field source error")
assert(find("playerSide:z:-:i:1"),"other fields must still be captured")
assert(record_count()==55,"three scalars become three error records")

-- A probed field too large to send becomes one field error, and the snapshot still completes.
setup()
ifs_freeform_main.teamCode={}
for i=1,65 do ifs_freeform_main.teamCode[i]=1 end
capture=make() messages={}
run(capture,1,9)
assert(find("teamCode:z:-:e:4C494D49545F7465616D436F6465"),"expected a bounded field error")
assert(record_count()==53,"a marker and two entries become one error record")
assert(begins()==1 and messages[table.getn(messages)]=="state:end:1:53")

-- Producer 6 sends at most eight snapshots, however often the campaign changes.
setup()
capture=make() messages={}
for t=1,400 do
    if math.mod(t,30)==0 then ifs_freeform_main.turnNumber=ifs_freeform_main.turnNumber+1 end
    capture(2,t,send)
end
assert(begins()==8,"expected exactly eight snapshots")
assert(not find("state:begin:6:9:"),"ninth snapshot must be refused")

-- A bad value in a required map, or no map at all, is rejected before any begin record, within the budget.
setup()
ifs_freeform_main.planetTeam.bad={nested=true}
capture=make() messages={}
capture(2,1,send)
assert(table.getn(messages)==1 and messages[1]=="state:rejected:1:VALUE")
run(capture,2,400)
assert(table.getn(messages)==8,"rejections consume the same bounded budget")
setup()
ifs_freeform_main.planetTeam="not a table"
capture=make() messages={}
capture(2,1,send)
assert(table.getn(messages)==1 and messages[1]=="state:rejected:1:MAP_planetTeam")
print("PASS: saved-value coverage, prefixed sources, per-field errors, cursor exclusion, copied data, bounded budget and rejection")

-- Nested purchase tables are copied when sampled; a flag change, or a Boolean turned number, sends
-- a new snapshot.
setup()
ifs_purchase_tech_cards={[1]={[1]=false,["1"]=true,["quoted:\n"]="value:/"},[2]={}}
ifs_purchase_unit_owned={[1]={[1]=true}}
ifs_purchase_tech_using={[1]={[1]=false}}
capture=make() messages={}
run(capture,1,1)
ifs_purchase_tech_cards[1][1]=true
run(capture,2,29)
assert(begins()==1)
assert(find("global.ifs_purchase_tech_cards:p:i/1/i/1:b:0"),"first copy must retain false")
assert(find("global.ifs_purchase_tech_cards:i:2:t:-"),"empty child map must have a marker")
run(capture,30,59)
assert(begins()==2,"same-shape purchase flag change must trigger")
assert(find("global.ifs_purchase_tech_cards:p:i/1/i/1:b:1"))
ifs_purchase_tech_cards[1][1]=1
run(capture,60,89)
assert(begins()==3,"Boolean and integer must be distinct")
-- Keeps these records in artifacts/ as a channel log too.
local output6=assert(io.open("artifacts/campaign-purchases-v6-test.log","w"))
for i=1,table.getn(messages) do output6:write("TEST CHANNEL GCOBS:" .. messages[i] .. "\n") end
output6:close()
-- A purchase table too deep, cyclic, too wide or too large becomes one field error; others still send.
for _,bad in ipairs({"depth","cycle","width","total"}) do
    setup()
    ifs_purchase_tech_cards={[1]={}}
    if bad=="depth" then ifs_purchase_tech_cards[1][1]={}
    elseif bad=="cycle" then ifs_purchase_tech_cards[1][1]=ifs_purchase_tech_cards
    elseif bad=="width" then
        for i=1,65 do ifs_purchase_tech_cards[1][i]=false end
    else
        for team=1,3 do
            ifs_purchase_tech_cards[team]={}
            for i=1,64 do ifs_purchase_tech_cards[team][i]=false end
        end
    end
    capture=make() messages={}
    run(capture,1,29)
    assert(begins()==1,"bad purchase field must not discard other fields")
    assert(find("global.ifs_purchase_tech_cards:z:-:e:"),"expected field error")
    assert(not find("global.ifs_purchase_tech_cards:p:"),"partial field must be discarded")
end
print("PASS: nested purchase copies, same-shape flag changes, typed values, empty maps, depth/cycle/width/total bounds")

-- Producer 7 (live co-op) sends every change: 32 records a tick and no snapshot budget.
setup()
capture=make(7) messages={}
capture(2,1,send)
assert(table.getn(messages)==32,"producer 7 sends 32 records a tick")
assert(find("state:begin:7:1:57:2:1:0"),"expected a version 7 envelope with the two camera fields")
run(capture,2,4)
assert(messages[table.getn(messages)]=="state:end:1:57")
setup()
capture=make(7) messages={}
for t=1,400 do
    if math.mod(t,5)==0 then ifs_freeform_main.turnNumber=ifs_freeform_main.turnNumber+1 end
    capture(2,t,send)
end
assert(begins()==81,"no emission budget: every sampled change is sent, got " .. begins())
assert(find("state:begin:7:81:"),"sequence numbers continue past eight")
-- The selection cursor triggers, so the friend's camera can follow.
setup()
capture=make(7) messages={}
run(capture,1,9)
assert(begins()==1)
ifs_freeform_main.planetNext="geo"
run(capture,10,14)
assert(begins()==2,"producer 7 sends cursor moves")
-- What the host's camera shows, the selected planet and the fleet screen's zoom, each triggers.
-- Producer 6 reads neither.
assert(find("planetSelected:z:-:z:-") and find("fleet.zoomLevel:z:-:z:-"))
ifs_freeform_main.planetSelected="cor"
run(capture,15,19)
assert(begins()==3 and find("planetSelected:z:-:s:636F72"),"producer 7 sends camera moves")
ifs_freeform_fleet.zoomLevel=0
run(capture,20,24)
assert(begins()==4 and find("fleet.zoomLevel:z:-:i:0"),"producer 7 sends zoom changes")
setup()
capture=make() messages={}
run(capture,1,29)
assert(begins()==1 and not find("planetSelected") and not find("zoomLevel"))
-- Rejections stay bounded.
setup()
ifs_freeform_main.planetTeam.bad={nested=true}
capture=make(7) messages={}
run(capture,1,400)
assert(table.getn(messages)==8,"producer 7 still bounds rejections")
print("PASS: producer 7 sampling cadence, 32 records a tick, no emission budget, cursor and camera triggers, bounded rejections")
