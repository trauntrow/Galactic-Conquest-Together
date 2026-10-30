-- Tests src/lua/player_menu.lua, the Online menu and its co-op and Versus flows, on a stand-in shell.
local function read(path) local f=assert(io.open(path,'rb')); local s=f:read('*a'); f:close(); return s end
local source=read(arg and arg[1] or 'src/lua/player_menu.lua')

-- sent: what the script tells the DLL. answers: the DLL questions (GCOBS:ask:) that answer yes.
local sent,answers={},{}
local function said(text)
    for _,s in ipairs(sent) do if s==text then return true end end
    return false
end
local function said_prefix(text)
    for _,s in ipairs(sent) do if string.sub(s,1,string.len(text))==text then return true end end
    return false
end
-- Stand-in calls, recorded by name.
local calls={}
local function record(name) return function(...) table.insert(calls,{name=name,arg=arg}) end end
local function called(name)
    for _,c in ipairs(calls) do if c.name==name then return c end end
end

local asks={}
local function asked_dll(question)
    for _,q in ipairs(asks) do if q==question then return true end end
    return false
end
-- A fresh stock shell: the DLL channel, button and screen helpers, galaxy screens and the popups.
local function shell()
    sent,calls,asks={},{},{}
    _G._GCT_menu_v1=nil
    ScriptCB_IsFileExist=function(text)
        if string.sub(text,1,10)=='GCOBS:ask:' then
            table.insert(asks,string.sub(text,11))
            if answers[string.sub(text,11)] then return 1 end
            return 0
        end
        table.insert(sent,text)
        return 0
    end
    ScriptCB_tounicode=function(s) return 'U:'..s end
    gPlatformStr='PC'
    gMenuButtonFont='gamefont_large'
    -- ifs_era_handler's stock Galactic Conquest button list and press handler.
    custom_GetGCButtonList=function()
        return {{tag='1',string='ifs.meta.Configs.1'},{tag='2',string='ifs.meta.Configs.2'},
            {tag='3',string='ifs.meta.Configs.3'},{tag='4',string='ifs.meta.Configs.4'},
            {tag='custom',string='ifs.meta.Configs.custom'},{tag='load',string='ifs.meta.load.btnload'}}
    end
    custom_PressedGCButton=function(tag) table.insert(calls,{name='stock_pressed',arg={tag}}); return false end
    added={}
    AddIFScreen=function(this,name)
        assert(type(this)=='table' and type(name)=='string')
        table.insert(added,name); this.created=true
        -- Creation shows a Unicode label in place of the string key.
        for _,b in pairs(this.buttons or {}) do
            if type(b)=='table' and b.label then b.label.shown=b.label.ustring or b.label.string end
        end
        if this.buttons and this.buttons._titlebar_ then
            this.buttons._titlebar_.shown=this.buttons._titlebar_.ustring or this.buttons._titlebar_.string
        end
    end
    NewIFContainer=function(t) return t end
    NewIFShellScreen=function(t) return t end
    AddVerticalButtons=function(dest,layout)
        if layout.title then dest._titlebar_={string=layout.title} end
        for i,entry in ipairs(layout.buttonlist) do
            dest[entry.tag]={tag=entry.tag,label={string=entry.string}}
        end
        return layout.buttonlist[1].tag
    end
    ShowHideVerticalButtons=function(dest,layout) return layout.buttonlist[1].tag end
    SetCurButton=record('SetCurButton')
    IFText_fnSetUString=function(t,u) assert(type(t)=='table'); t.ustring=u end
    RoundIFButtonLabel_fnSetUString=function(b,u) assert(type(b)=='table'); IFText_fnSetUString(b.label,u) end
    gIFShellScreenTemplate_fnEnter=record('template_enter')
    gShellScreen_fnDefaultInputAccept=function() return nil end
    ifelm_shellscreen_fnPlaySound=record('sound')
    gPCMainTabsLayout,gPCSinglePlayerTabsLayout={'main'},{'sp'}
    AddPCTitleText=record('AddPCTitleText')
    UpdatePCTitleText=record('UpdatePCTitleText')
    ifelem_tabmanager_Create=record('tabs_create')
    ifelem_tabmanager_SetSelected=record('tabs_select')
    ifelem_tabmanager_HandleInputAccept=function() return nil end
    pushed={}
    ifs_movietrans_PushScreen=function(screen) assert(type(screen)=='table'); table.insert(pushed,screen) end
    ScriptCB_SetQuitPlayer=record('SetQuitPlayer')
    ifs_freeform_main={name='main'}
    ifs_freeform_load={name='load'}
    for _,side in ipairs({'all','cis','rep','imp'}) do
        local name='start_'..side  -- Lua 5.0 closures share the loop variable
        _G['ifs_freeform_'..name]=function(main) table.insert(calls,{name=name,arg={main}}) end
    end
    -- The stock busy popup: activating needs all four callbacks; closing clears bCallerSetsString.
    Popup_Busy={title={}}
    function Popup_Busy:fnActivate(on)
        if on then
            assert(self.fnCheckDone and self.fnOnSuccess and self.fnOnFail and self.fnOnCancel)
            self.active=true; self.fBusyTime=0
        else self.active=false; self.bCallerSetsString=nil end
    end
    Popup_Ok={title={}}
    function Popup_Ok:fnActivate(on) self.active=on and true or false end
end
-- One busy-popup frame: polls fnCheckDone, counts fTimeout as a failure, then calls back.
local function popup_update(dt)
    local p=Popup_Busy
    local status=p.fnCheckDone()
    assert(type(status)=='number')
    p.fBusyTime=p.fBusyTime+dt
    if p.fTimeout and p.fBusyTime>p.fTimeout then status=-1; p.fTimeout=nil end
    if status>0.5 then p.fnOnSuccess() elseif status< -0.5 then p.fnOnFail() end
end
-- Back on the busy popup: closes it and cancels unless bNoCancel is set.
local function popup_back()
    if not Popup_Busy.bNoCancel then Popup_Busy:fnActivate(nil); Popup_Busy.fnOnCancel() end
end
-- ifs_sp_campaign's part: builds the Galactic Conquest tab from custom_GetGCButtonList.
local function build_gc_tab()
    ifs_sp_gc_main={buttons={}}
    local layout={buttonlist=custom_GetGCButtonList()}
    ifs_sp_gc_main.CurButton=AddVerticalButtons(ifs_sp_gc_main.buttons,layout)
    AddIFScreen(ifs_sp_gc_main,'ifs_sp_gc_main')
    return layout
end
local function press(screen,tag)
    screen.CurButton=tag
    screen.Input_Accept(screen)
end

-- Installs once per shell build and adds Online to the Galactic Conquest tab.
shell()
-- Installing the Online menu.
local stock_add, layout
do
    stock_add=AddIFScreen
    assert(assert(loadstring(source))()==6217431)
    assert(said('GCOBS:menu:installed'))
    assert(assert(loadstring(source))()==6217431)  -- same build: nothing is wrapped twice
    layout=build_gc_tab()
    local tags={}
    for i,b in ipairs(layout.buttonlist) do tags[i]=b.tag end
    assert(table.concat(tags,',')=='1,2,3,4,custom,load,gct_online')
    assert(ifs_sp_gc_main.buttons.gct_online.label.shown=='U:Online')
    -- The Remaster draws the tab as a list box: a row shows its key's translation, else the key itself,
    -- and a row without a string stays "XXXX".
    local translations={['ifs.meta.Configs.1']='Clone Wars: Republic',['ifs.meta.load.btnload']='Load'}
    local function remaster_row(entry)
        local row='XXXX'
        if entry.string then row=translations[entry.string] or ('U:'..entry.string) end
        return row
    end
    local rows={}
    for i,entry in ipairs(custom_GetGCButtonList()) do rows[entry.tag]=remaster_row(entry) end
    assert(rows.gct_online=='U:Online' and rows['1']=='Clone Wars: Republic' and rows.load=='Load')
    assert(ifs_sp_gc_main.buttons['1'].label.shown=='ifs.meta.Configs.1')
    assert(table.concat(added,',')=='ifs_sp_gc_main,ifs_gct_coop,ifs_gct_party,ifs_gct_versus,ifs_gct_sides,ifs_gct_online')
    -- The AddIFScreen wrapper also wraps the galaxy's pause menu, fleet screen, main screen and custom
    -- setup, and restores the stock function at the battle card.
    assert(said('GCOBS:menu:screens') and AddIFScreen~=stock_add)
    AddIFScreen({Input_Accept=function() end},'ifs_freeform_menu')
    AddIFScreen({Input_Accept=function() end,Input_Back=function() end,Input_KeyDown=function() end},'ifs_freeform_fleet')
    assert(AddIFScreen~=stock_add and said('GCOBS:galaxy:lock:menu') and said('GCOBS:galaxy:lock:fleet'))
    AddIFScreen({Enter=function() end,OneTimeInit=function() end,NextTurn=function() end},'ifs_freeform_main')
    AddIFScreen({Input_Accept=function() end},'ifs_freeform_customsetup')
    assert(AddIFScreen~=stock_add and said('GCOBS:versus:main_wrapped') and said('GCOBS:versus:setup_wrapped'))
    local first_card_next=function() end
    local first_card={Next=first_card_next}
    AddIFScreen(first_card,'ifs_freeform_battle_card')
    assert(AddIFScreen==stock_add and first_card.Next~=first_card_next and first_card.created)
    -- The Online and Co-op screens exist with Unicode labels; Co-op has no Custom button.
    assert(ifs_gct_online.created and ifs_gct_coop.created)
    assert(ifs_gct_online.buttons.coop.label.shown=='U:Co-op' and ifs_gct_online.buttons.versus.label.shown=='U:Versus')
    assert(ifs_gct_online.buttons._titlebar_.shown=='U:Online' and ifs_gct_coop.buttons._titlebar_.shown=='U:Co-op')
    assert(ifs_gct_coop.buttons['3'].label.shown=='ifs.meta.Configs.3' and ifs_gct_coop.buttons.load)
    assert(not ifs_gct_coop.buttons.custom)
    assert(called('AddPCTitleText') and called('tabs_create'))

    -- Pressing Online pushes its screen and returns nil; other buttons still reach the stock handler.
    calls={}
    assert(custom_PressedGCButton('gct_online')==nil and pushed[1]==ifs_gct_online)
    assert(not called('stock_pressed'))
    assert(custom_PressedGCButton('mod_button')==false and called('stock_pressed').arg[1]=='mod_button')

    -- On the Online screen, Co-op opens the scenario list and Versus its New/Load screen.
    pushed={}
    ifs_gct_online.Enter(ifs_gct_online,true)
    assert(ifs_gct_online.CurButton=='coop' and called('tabs_select') and called('template_enter'))
    press(ifs_gct_online,'coop')
    assert(pushed[1]==ifs_gct_coop)
    press(ifs_gct_online,'versus')
    assert(pushed[2]==ifs_gct_versus and not Popup_Ok.active)
    assert(ifs_gct_versus.buttons.vs_new.label.shown=='U:New game' and ifs_gct_versus.buttons.vs_load.label.shown=='U:Load')
    assert(ifs_gct_sides.buttons.ready.label.shown=='U:Ready')
end

-- The co-op party screen.
local label
do
    -- Co-op on scenario 3: the invite goes out and the party screen opens with this PC's row and three
    -- open slots. Nothing starts yet.
    local popped=0
    ScriptCB_PopScreen=function() popped=popped+1 end
    local function party_frames(n) for _=1,n do ifs_gct_party.Update(ifs_gct_party,0.016) end end
    function label(tag) return ifs_gct_party.buttons[tag].label.ustring end
    assert(ifs_gct_party.created and ifs_gct_party.buttons._titlebar_.shown=='U:Co-op: your party')
    pushed,calls,sent={},{},{}
    answers={}
    press(ifs_gct_coop,'3')
    assert(said('GCOBS:menu:invite:3') and pushed[1]==ifs_gct_party and not Popup_Busy.active)
    ifs_gct_party.Enter(ifs_gct_party,true)
    assert(said('GCOBS:menu:party') and label('you')=='U:You (host)')
    assert(label('friend1')=='U:Open slot' and label('friend2')=='U:Open slot' and label('friend3')=='U:Open slot')
    assert(label('invite')=='U:Invite friends' and label('start')=='U:Start (no friend ready yet)')
    -- Start is refused until a friend is ready.
    press(ifs_gct_party,'start')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'No friend is ready',1,true) and not said('GCOBS:menu:start'))
    Popup_Ok.active=false
    -- Rows from the DLL: a friend joining, then ready, and one with a different version.
    assert(_GCT_party_rows('2\n1:joining:Ben\n')==true)
    party_frames(10)
    assert(label('friend1')=='U:Ben: joining...' and label('friend2')=='U:Open slot')
    press(ifs_gct_party,'friend1')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'Ben is joining',1,true))
    Popup_Ok.active=false
    assert(_GCT_party_rows('3\n1:ready:Ben\n2:build:Dr "Q": two\n'))
    answers={paired=true}
    party_frames(10)
    assert(label('friend1')=='U:Ben: ready' and label('friend2')=='U:Dr "Q": two: different version')
    assert(label('friend3')=='U:Open slot' and label('start')=='U:Start')
    press(ifs_gct_party,'friend2')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'differs from yours',1,true))
    Popup_Ok.active=false
    press(ifs_gct_party,'friend1')
    press(ifs_gct_party,'you')
    assert(not Popup_Ok.active)                                        -- ready rows need no explanation
    -- Invite friends reopens the Steam overlay; with the overlay off or the party full, a popup says so.
    sent={}
    press(ifs_gct_party,'invite')
    assert(said('GCOBS:menu:invite_more') and not said('GCOBS:menu:invite:3') and not Popup_Ok.active)
    answers={paired=true,no_overlay=true}
    sent={}
    press(ifs_gct_party,'invite')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'friends list',1,true) and not said('GCOBS:menu:invite_more'))
    Popup_Ok.active=false
    answers={paired=true}
    assert(_GCT_party_rows('4\n1:ready:Ben\n2:ready:Cai\n3:joining:Friend 4\n'))
    party_frames(10)
    assert(label('friend3')=='U:Friend 4: joining...')
    press(ifs_gct_party,'invite')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'party is full',1,true) and not said('GCOBS:menu:invite_more'))
    Popup_Ok.active=false
    -- Start: the DLL holds the galaxy for the ready friends, then the stock start runs.
    pushed,calls,sent={},{},{}
    press(ifs_gct_party,'start')
    assert(said('GCOBS:menu:start') and called('SetQuitPlayer') and called('start_rep').arg[1]==ifs_freeform_main)
    assert(pushed[1]==ifs_freeform_main and not called('start_all') and not called('start_cis') and not called('start_imp'))
    -- Load: the party screen's Start opens the stock load screen.
    pushed,calls,sent={},{},{}
    press(ifs_gct_coop,'load')
    assert(said('GCOBS:menu:invite:load') and pushed[1]==ifs_gct_party)
    press(ifs_gct_party,'start')
    assert(said('GCOBS:menu:start') and ifs_freeform_load.Mode=='Load' and pushed[2]==ifs_freeform_load)
    -- Back leaves the co-op game, for every friend too.
    sent,popped={},0
    ifs_gct_party:Input_Back()
    assert(said('GCOBS:menu:leave') and popped==1 and not said('GCOBS:menu:start'))
    -- Failures close the party screen with a reason; a friend's version mismatch shows on its row instead.
    for _,case in ipairs({{'guest','already in'},{'lobby','Steam could not'}}) do
        answers={failed=true,[case[1]]=true}
        sent,popped={},0
        Popup_Ok.active=false
        party_frames(10)
        assert(said('GCOBS:menu:party_failed') and popped==1)
        assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,case[2],1,true))
    end
    answers={failed=true,build=true}
    sent,popped={},0
    Popup_Ok.active=false
    party_frames(10)
    assert(popped==0 and not Popup_Ok.active and not said('GCOBS:menu:party_failed'))
    answers={}
    ScriptCB_PopScreen=nil

    -- Versus Load invites its one friend on the busy popup and opens the stock load screen once paired.
    answers={}
    pushed,calls,sent={},{},{}
    press(ifs_gct_versus,'vs_load')
    assert(said('GCOBS:menu:invite:vs_load') and Popup_Busy.active and not pushed[1])
    assert(string.find(Popup_Busy.title.ustring,'Steam overlay',1,true) and Popup_Busy.fTimeout==nil and Popup_Busy.bNoCancel==nil)
    for _=1,100 do popup_update(0.016) end
    assert(Popup_Busy.active and not pushed[1])
    answers.paired=true
    popup_update(0.016)
    assert(not Popup_Busy.active and said('GCOBS:menu:paired'))
    assert(ifs_freeform_load.Mode=='Load' and pushed[1]==ifs_freeform_load)

    -- With the Steam overlay off, the popup says how the friend can still join.
    answers={no_overlay=true}
    press(ifs_gct_versus,'vs_load')
    assert(Popup_Busy.active and string.find(Popup_Busy.title.ustring,'friends list',1,true))
    -- Back cancels the invite and starts nothing.
    pushed,calls,sent={},{},{}
    popup_back()
    assert(not Popup_Busy.active and said('GCOBS:menu:cancel') and not pushed[1])

    -- Failures close the popup and say why.
    for _,case in ipairs({{'build','the Remaster (both of you need it, or neither)'},{'guest','already in'},{'lobby','Steam could not'}}) do
        answers={failed=true,[case[1]]=true}
        pushed,sent={},{}
        Popup_Ok.active=false
        press(ifs_gct_versus,'vs_load')
        popup_update(0.016)
        assert(not Popup_Busy.active and said('GCOBS:menu:failed') and not pushed[1])
        assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,case[2],1,true))
    end
    answers={}
    Popup_Ok.active=false

    -- Without the busy popup the invite still goes out and the game loads.
    local busy=Popup_Busy
    Popup_Busy=nil
    pushed,calls,sent={},{},{}
    press(ifs_gct_versus,'vs_load')
    assert(said('GCOBS:menu:invite:vs_load') and said('GCOBS:menu:no_popup') and pushed[1]==ifs_freeform_load)
    Popup_Busy=busy

    -- The friend's PC: the Single Player tab registers through the wrapper, so its Update is watched;
    -- other screens are left alone.
    shell()
    assert(assert(loadstring(source))()==6217431)
end
-- A friend's game waiting at the menus.
do
    local updates=0
    ifs_sp_campaign={buttons={},Update=function(self,dt) updates=updates+1; return 'kept',dt end}
    AddIFScreen(ifs_sp_campaign,'ifs_sp_campaign')
    local unwatched={Update=function() end}
    local unwatched_update=unwatched.Update
    AddIFScreen(unwatched,'ifs_instant_top')
    assert(unwatched.Update==unwatched_update)
    build_gc_tab()
    local open_popup=false
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or Popup_Ok.active or open_popup end
    local function menu_frames(n)
        for _=1,n do
            local a,b=ifs_sp_campaign.Update(ifs_sp_campaign,0.016)
            assert(a=='kept' and b==0.016)  -- the stock Update's results pass through
            if Popup_Busy.active then popup_update(0.016) end
        end
    end
    answers={}
    menu_frames(30)
    assert(updates==30 and not Popup_Busy.active and not Popup_Ok.active)
    -- Paired with nothing from the host yet: the waiting popup shows within 15 frames.
    answers={guest_wait=true}
    menu_frames(15)
    assert(Popup_Busy.active and said('GCOBS:menu:guest_waiting'))
    assert(string.find(Popup_Busy.title.ustring,'Joined your friend',1,true) and Popup_Busy.fTimeout==nil)
    assert(not string.find(Popup_Busy.title.ustring,'so far',1,true))
    -- The popup shows the party size, checked every 15th popup frame. Back leaves the game.
    answers={guest_wait=true,lobby_members_3=true}
    menu_frames(15)
    assert(string.find(Popup_Busy.title.ustring,"Joined your friend's co-op game: 3 players so far.",1,true))
    answers={guest_wait=true}
    popup_back()
    assert(said('GCOBS:menu:leave'))
    menu_frames(100)  -- still paired for a moment: no new popup yet
    assert(not Popup_Busy.active)
    answers={}
    menu_frames(30)
    assert(not Popup_Busy.active)
    -- The host's first revision arrives: the popup closes on its next frame.
    answers={guest_wait=true}
    menu_frames(15)
    assert(Popup_Busy.active)
    answers={}
    menu_frames(1)
    assert(not Popup_Busy.active and said('GCOBS:menu:guest_loading'))
    -- The host leaves before starting: the popup closes and says so.
    answers={guest_wait=true}
    menu_frames(15)
    assert(Popup_Busy.active)
    answers={guest_ended=true}; sent={}
    menu_frames(1)
    assert(not Popup_Busy.active and said('GCOBS:menu:guest_ended') and not said('GCOBS:menu:guest_loading'))
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'left the co-op game before it started',1,true))
    Popup_Ok.active=false
    answers={}
    -- Another popup is never covered.
    open_popup=true; answers={guest_wait=true}; sent={}
    menu_frames(30)
    assert(not Popup_Busy.active and not said('GCOBS:menu:guest_waiting'))
    open_popup=false; answers={}
    -- A version mismatch is explained once, then a failed join once.
    answers={guest_build=true}
    menu_frames(15)
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'does not match',1,true))
    Popup_Ok.active=false
    menu_frames(45)
    assert(not Popup_Ok.active)
    answers={join_failed=true}
    menu_frames(15)
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'Could not join',1,true))
    Popup_Ok.active=false
    -- Once the co-op game has ended (the DLL left the lobby), nothing is shown.
    answers={}; sent={}
    menu_frames(60)
    assert(not Popup_Ok.active and not Popup_Busy.active)
    ScriptCB_IsPopupOpen=nil

    -- A shell rebuild brings a fresh custom_GetGCButtonList, so the script installs again.
    local old_online=ifs_gct_online
    shell()
    assert(assert(loadstring(source))()==6217431 and said('GCOBS:menu:installed'))
    build_gc_tab()
    assert(ifs_gct_online~=old_online and ifs_gct_online.created)

    -- A failing screen build never breaks the stock tab: Online reports it is unavailable and
    -- AddIFScreen is restored.
    shell()
    stock_add=AddIFScreen
    assert(assert(loadstring(source))()==6217431)
    NewIFShellScreen=function() error('no shell template') end
    ifs_gct_online,ifs_gct_coop=nil,nil
    build_gc_tab()
    AddIFScreen({},'ifs_freeform_menu')
    AddIFScreen({},'ifs_freeform_fleet')
    AddIFScreen({},'ifs_freeform_main')
    AddIFScreen({},'ifs_freeform_customsetup')
    AddIFScreen({Next=function() end},'ifs_freeform_battle_card')
    assert(table.concat(added,',')=='ifs_sp_gc_main,ifs_freeform_menu,ifs_freeform_fleet,ifs_freeform_main,ifs_freeform_customsetup,ifs_freeform_battle_card')
    assert(ifs_sp_gc_main.created and AddIFScreen==stock_add)
    assert(said_prefix('GCOBS:menu:build_failed:'))
    pushed={}
    assert(custom_PressedGCButton('gct_online')==nil and not pushed[1] and said('GCOBS:menu:unavailable'))
end

-- Hosting and joining a shared battle.
local entered, net, remaster_saves, net_called, battle_shell, hosting_frames, prefs, saves, setups, sides,
    cards, join_shell, announced
do
    -- Battles: the battle card's Next saves, then calls ScriptCB_EnterMission(). The game's function
    -- launches the single-player battle only when called with no argument.
    -- Network stand-ins record each call in net.
    local function net_call(name) return function(...) table.insert(net,{name=name,arg=arg}) end end
    function net_called(name)
        for _,c in ipairs(net) do if c.name==name then return c end end
    end
    local function net_names()
        local names={}
        for i,c in ipairs(net) do names[i]=c.name end
        return table.concat(names,',')
    end
    -- A shell at the battle card with network calls recorded. remaster: true adds the Remaster's
    -- wrapper; a table also sets its settings and defaults.
    function battle_shell(remaster)
        shell()
        entered,net,remaster_saves={},{},0
        ScriptCB_EnterMission=function(...) table.insert(entered,arg.n) end
        SetState=net_call('SetState')
        rema_database,swbf2Remaster_getDefaultSettings=nil,nil
        if remaster then
            -- The Remaster's wrapper saves its settings into the galaxy save, then calls the stock function.
            local wrapped=ScriptCB_EnterMission
            ScriptCB_EnterMission=function(...) remaster_saves=remaster_saves+1; return wrapped(unpack(arg)) end
        end
        if type(remaster)=='table' then
            -- Its EnterMission and SetState read rema_database.data first.
            rema_database,swbf2Remaster_getDefaultSettings=remaster.settings,remaster.defaults
            local enter,set_state=ScriptCB_EnterMission,SetState
            ScriptCB_EnterMission=function(...) local _=rema_database.data; return enter(unpack(arg)) end
            SetState=function(...) local _=rema_database.data; return set_state(unpack(arg)) end
        end
        for _,name in ipairs({'SetConnectType','StartLogin','SetNetLoginName','OpenNetShell','SetAmHost','SetMissionNames',
            'SetNetGameDefaults','SetNetHeroDefaults','SetGameName','BeginLobby','UpdateLobby','LaunchLobby','CancelLobby',
            'CancelLogin','CloseNetShell','SetInNetGame'}) do _G['ScriptCB_'..name]=net_call(name) end
        -- Multiplayer difficulty is Easy (1); the Galactic Conquest profile's is Hard (3).
        ScriptCB_GetNetGameDefaults=function() return {bAutoAssignTeams=true,PasswordStr='old',bIsDedicated=true,iCONNumBots=12,iWarmUp=30,iDifficulty=1,iNumPlayers=64} end
        ScriptCB_GetDifficulty=function() return 3 end
        ScriptCB_GetCurrentProfileNetName=function() return 'U:Rebel' end
        assert(assert(loadstring(source))()==6217431)
        build_gc_tab()
        ifs_freeform_main.launchMission='pol1g_con'
        ifs_freeform_battle_card={Next=function(this)
            table.insert(calls,{name='SaveState'}); table.insert(calls,{name='SaveMissionSetup'})
            ScriptCB_EnterMission()
        end}
        AddIFScreen(ifs_freeform_battle_card,'ifs_freeform_battle_card')
        Popup_Busy.active=false
    end
    -- Runs n frames of the busy popup while it is open.
    function hosting_frames(n,dt) for _=1,n do if Popup_Busy.active then popup_update(dt or 0.016) end end end

    -- Not hosted: the stock single-player launch and no network calls.
    answers={}
    battle_shell()
    ifs_freeform_battle_card:Next()
    assert(asked_dll('battle_host') and table.getn(entered)==1 and entered[1]==0)
    assert(not Popup_Busy.active and table.getn(net)==0 and not said_prefix('GCOBS:battle:'))

    -- Hosted: a LAN match replaces the single-player launch, one stock call per frame on a busy popup
    -- that Back cannot close.
    answers={battle_host=true}
    battle_shell(true)
    ifs_freeform_battle_card:Next()
    assert(table.getn(entered)==0 and Popup_Busy.active and Popup_Busy.bNoCancel and Popup_Busy.fTimeout==20)
    assert(string.find(Popup_Busy.title.ustring,'Starting the battle',1,true))
    local token
    for _,s in ipairs(sent) do
        local _,_,t=string.find(s,'^GCOBS:battle:hosting:(GCT%-%x%x%x%x%x%x%x%x):pol1g_con$')
        if t then token=t end
    end
    assert(token and table.getn(net)==0)  -- reported before any network call
    hosting_frames(9)
    assert(net_names()=='SetConnectType,SetNetLoginName,OpenNetShell,SetAmHost,SetMissionNames,SetNetGameDefaults,SetGameName,BeginLobby')
    assert(net_called('SetConnectType').arg[1]=='lan' and net_called('OpenNetShell').arg[1]==1 and net_called('SetAmHost').arg[1]==1)
    assert(net_called('SetNetLoginName').arg[1]=='U:Rebel')
    local mission_names=net_called('SetMissionNames').arg
    assert(mission_names.n==2 and mission_names[1]=='pol1g_con' and mission_names[2]==nil)  -- one mission, then the shell
    prefs=net_called('SetNetGameDefaults').arg[1]
    assert(prefs.bAutoAssignTeams==false and prefs.PasswordStr=='' and prefs.bIsDedicated==false and prefs.iCONNumBots==12)
    assert(prefs.iWarmUp==0)  -- no warm-up: the match starts at once
    -- The bots play at Galactic Conquest's difficulty, not the Multiplayer one.
    assert(prefs.iDifficulty==3 and said('GCOBS:battle:difficulty:3'))
    -- The player count stays the profile's when it has room for the party.
    assert(prefs.iNumPlayers==64 and said('GCOBS:battle:players:64:64'))
    assert(net_called('SetGameName').arg[1]==token)
    -- The Remaster's wrapper ran, and the game's function got an argument, so it launched nothing.
    assert(remaster_saves==1 and table.getn(entered)==1 and entered[1]==1)
    for _,step in ipairs({'connect','name','netshell','host','missions','options','gamename','wrappers','lobby'}) do
        assert(said('GCOBS:battle:step:'..step),step)
    end
    -- No GOG Galaxy sign-in: one finishing in LAN mode crashes the game, and PC LAN menus never make one.
    assert(not net_called('StartLogin') and not said('GCOBS:battle:step:login'))
    -- Then UpdateLobby and LaunchLobby every frame, as the stock autolaunch does.
    net={}
    hosting_frames(3)
    assert(net_names()=='UpdateLobby,LaunchLobby,UpdateLobby,LaunchLobby,UpdateLobby,LaunchLobby')
    assert(net[1].arg.n==1 and net[1].arg[1]==nil)
    assert(Popup_Busy.active and not said_prefix('GCOBS:battle:fallback'))

    -- No launch within 20 s: the network game closes, and SetState alone starts the single-player
    -- battle, since the wrappers already ran.
    net={}
    hosting_frames(1,21)
    assert(not Popup_Busy.active and said('GCOBS:battle:fallback:no launch within 20 s'))
    assert(net_called('CancelLobby') and net_called('CancelLogin') and net_called('CloseNetShell').arg[1]==1)
    assert(net_called('SetInNetGame').arg.n==1 and net_called('SetInNetGame').arg[1]==nil)
    assert(net_called('SetState').arg[1]=='mission' and remaster_saves==1 and table.getn(entered)==1)

    -- A failing network call: the network game closes and the battle launches single-player.
    battle_shell(true)
    ScriptCB_OpenNetShell=function() error('no net shell') end
    ifs_freeform_battle_card:Next()
    hosting_frames(10)
    assert(not Popup_Busy.active and said_prefix('GCOBS:battle:fallback:netshell:'))
    assert(not net_called('CancelLobby') and net_called('CloseNetShell') and not net_called('SetState'))
    assert(remaster_saves==1 and table.getn(entered)==1 and entered[1]==0)

    -- No mission: the stock launch.
    battle_shell()
    ifs_freeform_main.launchMission=nil
    ifs_freeform_battle_card:Next()
    assert(said('GCOBS:battle:skipped') and entered[1]==0 and table.getn(net)==0)

    -- Other launches (instant action, the story campaign) are never hosted, and the DLL is not asked.
    answers={battle_host=true}
    battle_shell()
    ScriptCB_EnterMission()
    assert(entered[1]==0 and not asked_dll('battle_host') and not Popup_Busy.active)
    ScriptCB_EnterMission('x')
    assert(entered[2]==1)

    -- The hosting report also carries the human's side and the saved mission setup for the friend's game.
    battle_shell()
    ifs_freeform_main.controllerTeam={[1]=2}
    ifs_freeform_main.teamCode={'all','imp'}
    ScriptCB_IsMissionSetupSaved=function() return 1 end
    ScriptCB_LoadMissionSetup=function()
        return {world='pol', units={all={soldier=true,pilot=true,assault=true,sniper=true,engineer=true},
            imp={soldier=true,pilot=true,assault=true,sniper=true,engineer=true,officer=true,special=true}}}
    end
    ifs_freeform_battle_card:Next()
    local setup_report
    for _,s in ipairs(sent) do
        local _,_,r=string.find(s,'^GCOBS:battle:hosting:GCT%-%x+:pol1g_con:(.*)$')
        if r then setup_report=r end
    end
    assert(setup_report=='imp:pol:all=1111100,imp=1111111',tostring(setup_report))

    -- The hosting report ends with the sides' bonus cards from activeBonus[team], so the friend's game
    -- can activate them too.
    local function hosting_report()
        local report
        for _,s in ipairs(sent) do
            local _,_,r=string.find(s,'^GCOBS:battle:hosting:GCT%-%x+:pol1g_con(.*)$')
            if r then report=r end
        end
        return report
    end
    battle_shell()
    ifs_freeform_main.controllerTeam={[1]=2}
    ifs_freeform_main.teamCode={'all','imp'}
    ifs_freeform_main.activeBonus={[1]='autoturrets',[2]='sensor_array'}
    ifs_freeform_battle_card:Next()
    assert(hosting_report()==':imp:pol:all=1111100,imp=1111111:1=autoturrets,2=sensor_array',tostring(hosting_report()))
    -- Without the Leader card the heroes stay as the galaxy's rules have them.
    hosting_frames(9)
    assert(said('GCOBS:battle:step:options') and net_called('SetNetGameDefaults').arg[1].bHeroesEnabled==nil)
    assert(not net_called('SetNetHeroDefaults') and not said_prefix('GCOBS:battle:heroes'))
    -- One side's card and no mission setup: empty setup fields first. Only teams 1 and 2 with plain names count.
    ScriptCB_IsMissionSetupSaved,ScriptCB_LoadMissionSetup=nil,nil
    battle_shell()
    ifs_freeform_main.activeBonus={[2]='leader',[3]='supplies',[1]='a:b'}
    ifs_freeform_battle_card:Next()
    assert(hosting_report()=='::::2=leader',tostring(hosting_report()))
    -- The Leader card turns heroes on with the card's rules at the options step, before the friend's
    -- game can join and copy them.
    hosting_frames(9)
    assert(net_names()=='SetConnectType,SetNetLoginName,OpenNetShell,SetAmHost,SetMissionNames,SetNetGameDefaults,SetNetHeroDefaults,SetGameName,BeginLobby',net_names())
    prefs=net_called('SetNetGameDefaults').arg[1]
    assert(prefs.bHeroesEnabled==true and prefs.iWarmUp==0 and prefs.bAutoAssignTeams==false and prefs.iDifficulty==3)
    local heroes=net_called('SetNetHeroDefaults').arg[1]
    assert(heroes.iHeroUnlock==7 and heroes.iHeroUnlockVal==5 and heroes.iHeroPlayer==2 and heroes.iHeroTeam==3)
    assert(heroes.iHeroRespawn==2 and heroes.iHeroRespawnVal==120)
    assert(said('GCOBS:battle:heroes:leader') and said('GCOBS:battle:step:options'))
    -- Without the profile's difficulty, or with one out of range, the Multiplayer difficulty stays.
    for _,get in ipairs({false,function() return 7 end}) do
        battle_shell()
        ScriptCB_GetDifficulty=get or nil
        ifs_freeform_battle_card:Next()
        hosting_frames(9)
        assert(net_called('SetNetGameDefaults').arg[1].iDifficulty==1 and not said_prefix('GCOBS:battle:difficulty'))
    end
    -- The player count rises to the co-op party's size and never falls: {profile, party, expected}.
    for _,case in ipairs({{64,3,64},{2,3,3},{2,4,4},{3,2,3}}) do
        battle_shell()
        ScriptCB_GetNetGameDefaults=function() return {iNumPlayers=case[1]} end
        answers={battle_host=true,['battle_party_'..case[2]]=true}
        ifs_freeform_battle_card:Next()
        hosting_frames(9)
        assert(net_called('SetNetGameDefaults').arg[1].iNumPlayers==case[3])
        assert(said('GCOBS:battle:players:'..case[1]..':'..case[3]))
    end
    answers={battle_host=true}
    -- Without the stock hero defaults call, heroes are not turned on early.
    battle_shell()
    ScriptCB_SetNetHeroDefaults=nil
    ifs_freeform_main.activeBonus={[1]='leader'}
    ifs_freeform_battle_card:Next()
    assert(hosting_report()=='::::1=leader')
    hosting_frames(9)
    assert(said('GCOBS:battle:step:lobby') and net_called('SetNetGameDefaults').arg[1].bHeroesEnabled==nil)
    assert(not said_prefix('GCOBS:battle:heroes'))
    -- A card list that cannot be read never stops the battle: no cards.
    battle_shell()
    ifs_freeform_main.activeBonus=setmetatable({},{__index=function() error('unreadable') end})
    ifs_freeform_battle_card:Next()
    assert(hosting_report()=='' and Popup_Busy.active and not said_prefix('GCOBS:battle:fallback'))
    battle_shell()
    ifs_freeform_main.activeBonus=7
    ifs_freeform_battle_card:Next()
    assert(hosting_report()=='' and Popup_Busy.active)
    answers={}

    -- The friend's game joins the host's battle: the DLL calls _GCT_battle_join with the announcement.
    -- ScriptCB_IsJoinDone's answer: -1 failed or no join, 1 taken in, 0 pending.
    local join_done=0
    -- A battle shell with the join's stand-ins; the third AutoNetJoin finds the session.
    function join_shell(remaster)
        battle_shell(remaster or true)
        saves,setups,sides,cards,join_done={},{},{},{},0
        ActivateBonus=function(...) table.insert(cards,arg) end
        for _,name in ipairs({'BeginSessionList','CancelJoin','CancelSessionList'}) do _G['ScriptCB_'..name]=net_call(name) end
        local finds=0
        ScriptCB_AutoNetJoin=function() finds=finds+1; table.insert(net,{name='AutoNetJoin',arg={}}); return finds>=3 end
        ScriptCB_IsJoinDone=function() table.insert(net,{name='IsJoinDone',arg={}}); return join_done end
        ScriptCB_SaveMissionSetup=function(t) table.insert(setups,t) end
        ScriptCB_MetagameSetSide=function(player,side) table.insert(sides,{player,side}) end
        ScriptCB_IsPopupOpen=function() return Popup_Busy.active end
        ifs_freeform_main.SaveState=function(self) table.insert(saves,'SaveState') end
        ifs_freeform_main.SaveMissionSetup=function(self) table.insert(saves,'SaveMissionSetup') end
        net={}
    end
    announced='GCT-0a1b2c3d\npol1g_con\nimp\npol\nall=1111100,imp=1111111\n'
    join_shell()
    assert(type(_GCT_battle_join)=='function')
    assert(_GCT_battle_join(announced)==true)
    -- Before any network call the galaxy is saved, the wrappers run without a launch, and the host's
    -- setup and this player's side are set.
    assert(table.getn(saves)==1 and saves[1]=='SaveState' and remaster_saves==1 and entered[1]==1)
    local stored=setups[1]
    assert(stored.world=='pol' and stored.units.all.soldier and stored.units.all.engineer and not stored.units.all.officer)
    assert(not stored.units.all.special and stored.units.imp.special and stored.units.imp.officer)
    assert(sides[1][1]==0 and sides[1][2]=='imp' and table.getn(net)==0)
    assert(said('GCOBS:battle:joining:GCT-0a1b2c3d') and Popup_Busy.active and Popup_Busy.fTimeout==150 and not Popup_Busy.bNoCancel)
    assert(string.find(Popup_Busy.title.ustring,'Joining your friend',1,true))
    hosting_frames(5)
    assert(net_names()=='SetConnectType,SetNetLoginName,OpenNetShell,SetAmHost,BeginSessionList')
    assert(net_called('SetConnectType').arg[1]=='lan' and net_called('OpenNetShell').arg[1]==1)
    assert(net_called('SetAmHost').arg.n==1 and net_called('SetAmHost').arg[1]==nil)
    for _,step in ipairs({'connect','name','netshell','client','list'}) do assert(said('GCOBS:battle:step:'..step),step) end
    assert(not net_called('StartLogin') and not said('GCOBS:battle:step:login'))
    -- AutoNetJoin each frame until it finds the session, then only IsJoinDone. The DLL makes the lobby
    -- calls, since the engine closes this popup once the join is done.
    net={}
    hosting_frames(5)
    assert(net_names()=='AutoNetJoin,AutoNetJoin,AutoNetJoin,IsJoinDone,IsJoinDone',net_names())
    assert(said('GCOBS:battle:found') and Popup_Busy.active)
    assert(not net_called('UpdateLobby') and not net_called('LaunchLobby'))
    -- Taken in by the host: the popup stays until the engine closes it without a callback, so nothing
    -- is reported.
    join_done=1
    local reports=table.getn(sent)
    hosting_frames(3)
    assert(Popup_Busy.active and table.getn(sent)==reports)
    Popup_Busy:fnActivate(nil)
    hosting_frames(3)
    assert(table.getn(sent)==reports and not net_called('CancelJoin'))
    -- The host refuses or loses the join: the network game closes and the friend stays in the galaxy.
    join_shell()
    assert(_GCT_battle_join(announced))
    hosting_frames(8)
    assert(said('GCOBS:battle:found') and Popup_Busy.active)
    join_done=-1
    net={}
    hosting_frames(1)
    assert(not Popup_Busy.active and said("GCOBS:battle:joinfail:the host's game refused or lost the join"))
    assert(net_called('CancelJoin') and net_called('CloseNetShell').arg[1]==1 and net_called('SetInNetGame').arg.n==1)
    -- The host's battle ends before this one launches: the network game closes and the friend stays.
    join_shell()
    assert(_GCT_battle_join(announced))
    hosting_frames(8)
    assert(said('GCOBS:battle:found') and Popup_Busy.active)
    answers={battle_join_off=true}
    net={}
    hosting_frames(1)
    answers={}
    assert(not Popup_Busy.active and said("GCOBS:battle:joinfail:the host's battle is over"))
    assert(net_called('CancelJoin') and net_called('CancelSessionList') and net_called('CancelLogin'))
    assert(net_called('CloseNetShell').arg[1]==1 and net_called('SetInNetGame').arg.n==1)

    -- Back on the joining popup: stay in the galaxy.
    join_shell()
    assert(_GCT_battle_join(announced))
    hosting_frames(2)
    popup_back()
    assert(said('GCOBS:battle:joinfail:the friend stayed in the galaxy') and not net_called('CancelJoin'))
    assert(net_called('CloseNetShell'))

    -- No launch within 150 s ends the join; a slow host load can take over a minute.
    join_shell()
    ScriptCB_AutoNetJoin=function() return false end
    assert(_GCT_battle_join(announced))
    hosting_frames(10)
    hosting_frames(1,61)
    assert(Popup_Busy.active and not said_prefix('GCOBS:battle:joinfail'))
    hosting_frames(1,90)
    assert(not Popup_Busy.active and said('GCOBS:battle:joinfail:no launch within 150 s'))

    -- Without the host's setup the friend's game sets up its own.
    join_shell()
    assert(_GCT_battle_join('GCT-1\nfel1g_con\n\n\n\n'))
    assert(saves[1]=='SaveState' and saves[2]=='SaveMissionSetup' and table.getn(setups)==0 and table.getn(sides)==0)
    -- Another popup open: nothing happens; a garbled announcement is an error.
    join_shell()
    Popup_Busy.active=true
    assert(_GCT_battle_join(announced)==false and table.getn(saves)==0)
    assert(not _GCT_battle_join_ready())  -- the DLL's readiness check waits too
    Popup_Busy.active=false
    assert(_GCT_battle_join_ready())
    assert(not pcall(_GCT_battle_join,'GCT-1\npol1g_con') and table.getn(saves)==0)
    -- An announcement without the bonus card line is refused.
    assert(not pcall(_GCT_battle_join,'GCT-0a1b2c3d\npol1g_con\nimp\npol\nall=1111100,imp=1111111') and table.getn(saves)==0)
    ScriptCB_IsPopupOpen=nil

    -- The host's bonus cards are activated on the friend's PC before any network call, and a side
    -- without one is cleared. No cards announced: none touched.
    join_shell()
    assert(_GCT_battle_join(announced) and table.getn(cards)==0 and not said_prefix('GCOBS:battle:bonus'))
    join_shell()
    assert(_GCT_battle_join('GCT-0a1b2c3d\nnab2g_con\nall\nnab\nall=1100000,imp=1100000\n1=autoturrets'))
    assert(table.getn(cards)==2 and table.getn(net)==0)
    assert(cards[1].n==2 and cards[1][1]==1 and cards[1][2]=='team_bonus_autoturrets')
    assert(cards[2].n==3 and cards[2][1]==2 and cards[2][2]=='team_bonus_none' and cards[2][3]==false)
    assert(said('GCOBS:battle:bonus:1=autoturrets') and said('GCOBS:battle:joining:GCT-0a1b2c3d'))
    assert(sides[1][2]=='all' and saves[1]=='SaveState')
    join_shell()
    assert(_GCT_battle_join('GCT-0a1b2c3d\nnab2g_con\nall\nnab\nall=1100000,imp=1100000\n1=sensor_array,2=leader'))
    assert(table.getn(cards)==2 and cards[1][2]=='team_bonus_sensor_array' and cards[1].n==2)
    assert(cards[2][1]==2 and cards[2][2]=='team_bonus_leader' and cards[2].n==2)
    -- Without ActivateBonus the join still goes ahead; the DLL puts the cards in place in the battle.
    join_shell()
    ActivateBonus=nil
    assert(_GCT_battle_join('GCT-1\nfel1g_con\n\n\n\n2=garrison') and Popup_Busy.active)
    assert(not said_prefix('GCOBS:battle:bonus') and said('GCOBS:battle:joining:GCT-1'))
    ScriptCB_IsPopupOpen=nil

    -- Battle notes: the fleet screen calls _GCT_battle_note while no popup is open, the DLL says which
    -- note is due, and the script reports it once shown.
    shell()
    assert(assert(loadstring(source))()==6217431)
    build_gc_tab()
    assert(type(_GCT_battle_note)=='function' and type(_GCT_battle_join_ready)=='function')
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or (Popup_Ok and Popup_Ok.active) end
end
-- Battle notes and the return to the galaxy.
local accept_ok, valid_calls, pushes
do
    -- The OK popup's accept: closes it, then calls fnDone.
    function accept_ok() Popup_Ok:fnActivate(nil); if Popup_Ok.fnDone then Popup_Ok.fnDone() end end
    answers={}
    _GCT_battle_note()
    assert(not Popup_Ok.active and not said_prefix('GCOBS:battle:noted'))
    for _,case in ipairs({{'join_failed','Could not join your friend'},{'friend_failed','could not join that battle'},
        {'friend_stayed','stayed in the galaxy'},{'friend_left','left that battle'}}) do
        answers={['battle_note_'..case[1]]=true}; sent={}
        _GCT_battle_note()
        assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,case[2],1,true),case[1])
        assert(said('GCOBS:battle:noted:'..case[1]) and string.len(Popup_Ok.title.ustring)<100)
        sent={}
        _GCT_battle_note()  -- once: its own popup is open now
        assert(not said_prefix('GCOBS:battle:noted'))
        accept_ok()
        assert(not Popup_Ok.active)
    end
    -- With several friends, one note names each who missed the battle by party row name, or
    -- "Friend 2" to "Friend 4" without one.
    answers={battle_note_friends=true,battle_note_friend1_stayed=true}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Friend 2 stayed in the galaxy during that battle.')
    accept_ok()
    assert(_GCT_party_rows('4\n1:ready:Ben\n2:ready:Cai\n3:ready:Dee\n'))
    answers={battle_note_friends=true,battle_note_friend2_failed=true,battle_note_friend3_left=true}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Cai could not join that battle. Dee left that battle before it ended.')
    assert(said('GCOBS:battle:noted:friends') and not said_prefix('GCOBS:battle:noted:friend_'))
    accept_ok()
    -- A name is remembered after its row goes (a friend who left).
    assert(_GCT_party_rows('2\n1:ready:Ben\n'))
    answers={battle_note_friends=true,battle_note_friend3_left=true}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Dee left that battle before it ended.')
    accept_ok()
    answers={battle_note_friends=true}; sent={}  -- nobody named: nothing shows
    _GCT_battle_note()
    assert(not Popup_Ok.active and not said_prefix('GCOBS:battle:noted'))
    -- A friend who left while the others play on is named once, and the DLL is told which note showed.
    assert(_GCT_party_rows('3\n1:ready:Ben\n2:ready:Cai\nL1:3:Dee\n'))
    answers={}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Dee left the co-op game.' and said('GCOBS:menu:noted:1'))
    accept_ok()
    -- Rows that still carry it (the DLL has not dropped it yet) do not show it again.
    assert(_GCT_party_rows('2\n1:ready:Ben\nL1:3:Dee\nL2:2:Cai\n'))
    sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Cai left the co-op game.' and said('GCOBS:menu:noted:2'))
    accept_ok()
    sent={}
    _GCT_battle_note()
    assert(not Popup_Ok.active and not said_prefix('GCOBS:menu:noted'))
    -- A battle's note shows first, naming a friend from its left note; the left note shows next.
    assert(_GCT_party_rows('2\n1:ready:Ben\nL3:3:Dee\n'))
    answers={battle_note_friends=true,battle_note_friend3_left=true}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Dee left that battle before it ended.')
    assert(not said_prefix('GCOBS:menu:noted'))
    accept_ok()
    answers={}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Dee left the co-op game.' and said('GCOBS:menu:noted:3'))
    accept_ok()
    -- A friend who joined mid-game, then one who left, in the notes' order; the joiner's name also
    -- serves the battle notes.
    assert(_GCT_party_rows('3\n1:ready:Ben\n3:ready:Eve\nJ4:3:Eve\nL5:2:Cai\n'))
    sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Eve joined the co-op game.' and said('GCOBS:menu:noted:4'))
    accept_ok()
    sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Cai left the co-op game.' and said('GCOBS:menu:noted:5'))
    accept_ok()
    assert(_GCT_party_rows('2\n1:ready:Ben\nJ4:3:Eve\n'))  -- Eve has left since: no note shows twice
    sent={}
    _GCT_battle_note()
    assert(not Popup_Ok.active and not said_prefix('GCOBS:menu:noted'))
    answers={battle_note_friends=true,battle_note_friend3_stayed=true}; sent={}
    _GCT_battle_note()
    assert(Popup_Ok.active and Popup_Ok.title.ustring=='U:Eve stayed in the galaxy during that battle.')
    accept_ok()
    answers={}
    -- Another popup open: the note waits for it.
    answers={battle_note_friend_left=true}; sent={}
    Popup_Busy.active=true
    _GCT_battle_note()
    assert(not Popup_Ok.active and not said_prefix('GCOBS:battle:noted'))
    Popup_Busy.active=false
    -- Without Popup_Ok nothing is shown and nothing fails.
    local ok_popup=Popup_Ok
    Popup_Ok=nil
    _GCT_battle_note()
    Popup_Ok=ok_popup
    assert(not said_prefix('GCOBS:battle:noted'))
    answers={}
    -- The friend's next join closes an open note instead of waiting for it.
    join_shell()
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or (Popup_Ok and Popup_Ok.active) end
    answers={battle_note_join_failed=true}
    _GCT_battle_note()
    assert(Popup_Ok.active)
    answers={}
    assert(_GCT_battle_join_ready())
    assert(_GCT_battle_join(announced)==true and not Popup_Ok.active and Popup_Busy.active)
    assert(saves[1]=='SaveState' and said('GCOBS:battle:joining:GCT-0a1b2c3d'))
    -- A note closed by the player no longer counts as this script's own popup.
    join_shell()
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or (Popup_Ok and Popup_Ok.active) end
    answers={battle_note_join_failed=true}
    _GCT_battle_note()
    answers={}
    accept_ok()
    Popup_Busy.active=true  -- some other popup now
    assert(not _GCT_battle_join_ready() and _GCT_battle_join(announced)==false)
    Popup_Busy.active=false
    ScriptCB_IsPopupOpen=nil

    -- Back from a battle, the friend drops its own result, and an AI-turn save shows the fleet screen
    -- instead of starting the AI here, once.
    answers={battle_return=true}
    shell()
    valid_calls,pushes={},{}
    ScriptCB_SetLastBattleVictoryValid=function(v) table.insert(valid_calls,v) end
    ScriptCB_PushScreen=function(screen) table.insert(pushes,screen) end
    local stock_push=ScriptCB_PushScreen
    assert(assert(loadstring(source))()==6217431)
    assert(table.getn(valid_calls)==1 and valid_calls[1]==false and said('GCOBS:battle:return'))
    answers={}
    build_gc_tab()
    local entered_galaxy=0
    local galaxy={Enter=function(self,forward) entered_galaxy=entered_galaxy+1; ScriptCB_PushScreen('ifs_freeform_ai'); return 'entered',forward end}
    AddIFScreen(galaxy,'ifs_freeform_main')
    local r1,r2=galaxy.Enter(galaxy,true)
    assert(r1=='entered' and r2==true and pushes[1]=='ifs_freeform_fleet' and said('GCOBS:battle:ai_screen_skipped'))
    assert(ScriptCB_PushScreen==stock_push)
    galaxy.Enter(galaxy,true)
    assert(pushes[2]=='ifs_freeform_ai' and entered_galaxy==2)  -- only the first Enter
    -- On a normal shell build nothing changes: outside Versus the wrapped Enter acts as the stock one.
    shell()
    valid_calls,pushes={},{}
    assert(assert(loadstring(source))()==6217431)
    build_gc_tab()
    local plain={Enter=function(self,forward) ScriptCB_PushScreen('ifs_freeform_ai'); return 'plain',forward end}
    AddIFScreen(plain,'ifs_freeform_main')
    local p1,p2=plain.Enter(plain,true)
    assert(p1=='plain' and p2==true and pushes[1]=='ifs_freeform_ai' and ScriptCB_PushScreen==stock_push)
    assert(table.getn(valid_calls)==0 and not said('GCOBS:battle:ai_screen_skipped') and not said('GCOBS:versus:ai_screen_skipped'))
    ScriptCB_SetLastBattleVictoryValid,ScriptCB_PushScreen=nil,nil

    -- Back from a Versus battle whose host left before its end, the DLL says whose win this galaxy keeps.
    local kept_main,stock_victory
    -- A Versus shell whose galaxy screen does what the stock Enter and NextTurn do here.
    local function kept_shell(versus_answers)
        answers=versus_answers
        shell()
        valid_calls,pushes={},{}
        ScriptCB_SetLastBattleVictoryValid=function(v) table.insert(valid_calls,v) end
        ScriptCB_PushScreen=function(screen) table.insert(pushes,screen) end
        ScriptCB_GetLastBattleVictory=function() return -1 end  -- what SetLastBattleVictoryValid(false) leaves
        stock_push,stock_victory=ScriptCB_PushScreen,ScriptCB_GetLastBattleVictory
        assert(assert(loadstring(source))()==6217431)
        answers={versus=true}
        build_gc_tab()
        kept_main={controllerTeam={[0]=2},playerTeam=1,planetNext='kas',applied={},turns=0,asked={}}
        -- The stock Enter: a win applies at planetNext with the result screen, else the AI's screen shows.
        kept_main.Enter=function(self,forward)
            table.insert(self.asked,ScriptCB_GetLastBattleVictory())  -- LoadState asks first
            local victory=ScriptCB_GetLastBattleVictory()
            if victory>0 then
                table.insert(self.applied,self.planetNext..'='..victory)
                ScriptCB_SetLastBattleVictoryValid(false)
                ScriptCB_PushScreen('ifs_freeform_result')
                self.requestSave=true
            else
                ScriptCB_PushScreen('ifs_freeform_ai')
            end
            return 'entered',forward
        end
        -- NextTurn hands the turn to the other side, this PC's.
        kept_main.NextTurn=function(self)
            self.turns=self.turns+1; self.playerTeam=3-self.playerTeam
            ScriptCB_PushScreen('ifs_freeform_fleet')
        end
        AddIFScreen(kept_main,'ifs_freeform_main')
    end
    -- This side's win: applied for this PC's side without the result screen or the save prompt, then
    -- the attacker's turn ends.
    kept_shell({battle_return=true,versus=true,battle_keep_mine=true})
    assert(said('GCOBS:battle:return') and said('GCOBS:battle:keep:mine') and valid_calls[1]==false)
    local k1,k2=kept_main.Enter(kept_main,true)
    assert(k1=='entered' and k2==true and kept_main.asked[1]==2)
    assert(table.concat(kept_main.applied,',')=='kas=2' and table.getn(valid_calls)==2)
    assert(table.concat(pushes,',')=='ifs_freeform_fleet' and said('GCOBS:battle:result_screen_skipped'),table.concat(pushes,','))
    assert(kept_main.requestSave==nil and kept_main.turns==1 and kept_main.playerTeam==2 and said('GCOBS:battle:kept:2:kas'))
    assert(ScriptCB_PushScreen==stock_push and ScriptCB_GetLastBattleVictory==stock_victory)
    kept_main.Enter(kept_main,true)  -- once: a later Enter is the stock one
    assert(kept_main.turns==1 and table.getn(kept_main.applied)==1 and pushes[2]=='ifs_freeform_fleet')
    -- The other side's win.
    kept_shell({battle_return=true,versus=true,battle_keep_theirs=true})
    assert(said('GCOBS:battle:keep:theirs'))
    kept_main.Enter(kept_main,true)
    assert(table.concat(kept_main.applied,',')=='kas=1' and kept_main.turns==1 and said('GCOBS:battle:kept:1:kas'))
    -- This PC's side unknown: nothing kept, the plain return.
    kept_shell({battle_return=true,versus=true,battle_keep_mine=true})
    kept_main.controllerTeam={}
    kept_main.Enter(kept_main,true)
    assert(table.getn(kept_main.applied)==0 and kept_main.turns==0 and said('GCOBS:battle:kept_none'))
    assert(table.concat(pushes,',')=='ifs_freeform_fleet' and not said('GCOBS:battle:result_screen_skipped'))
    -- Nothing to keep: the plain return, with its own result dropped.
    kept_shell({battle_return=true,versus=true})
    assert(not said_prefix('GCOBS:battle:keep'))
    kept_main.Enter(kept_main,true)
    assert(table.getn(kept_main.applied)==0 and kept_main.turns==0 and kept_main.asked[1]==-1 and not said_prefix('GCOBS:battle:kept'))
    ScriptCB_SetLastBattleVictoryValid,ScriptCB_PushScreen,ScriptCB_GetLastBattleVictory=nil,nil,nil
    answers={}
end

-- The friend's locked fleet screen.
local did, done, galaxy_shell
do
    -- The friend's lock: in the host's galaxy only the host acts.
    local function act(name) return function() table.insert(did,name); return true end end
    function done(name)
        for _,d in ipairs(did) do if d==name then return true end end
        return false
    end
    -- The stock fleet screen's PC input handlers, with what they do recorded in did.
    local function stock_fleet()
        local fleet={errorSound='error'}
        fleet.SelectFleet=function(self) table.insert(did,'SelectFleet'); self.fleetSelected=true; return true end
        fleet.IsValidBuild=function() return true end
        fleet.BuildFleet=act('BuildFleet')
        fleet.IsValidMove=function() return true end
        fleet.AttemptMove=act('AttemptMove')
        fleet.AttemptUndo=act('AttemptUndo')
        fleet.UpdateAction=function() end
        fleet.Input_Misc2=act('tutorial')
        fleet.Input_LTrigger2=act('zoom_out')
        fleet.Input_RTrigger2=act('zoom_in')
        fleet.Input_KeyDown=function(self,key)
            if key==8 then self.CurButton='_back'; self:Input_Accept(-1)
            elseif key==10 or key==13 then self.CurButton='_accept'; self:Input_Accept(-1)
            elseif key==32 then self.CurButton='_next'; self:Input_Accept(-1)
            elseif key==-59 then self.CurButton='_help'; self:Input_Accept(-1)
            elseif key==9 then ScriptCB_SetIFScreen('ifs_freeform_purchase_tech')
            elseif key==45 or key==95 then self:Input_LTrigger2()
            elseif key==43 or key==61 then self:Input_RTrigger2() end
        end
        fleet.Input_Accept=function(self,joystick)
            local main=ifs_freeform_main
            if gPlatformStr=='PC' then
                if ifelem_tabmanager_HandleInputAccept(self,ifs_freeform_tab_layout) then return end
                if self.CurButton=='_accept' then
                elseif self.CurButton=='_back' then self:Input_Back(joystick); return
                elseif self.CurButton=='_help' then self:Input_Misc2(joystick); return
                elseif self.CurButton=='_next' then
                    if not self.displayTimer then ScriptCB_PushScreen('ifs_freeform_summary') end
                    return
                else
                    if not self.planetRollover then return end
                    if self.fleetSelected and not self:IsValidMove(main.playerTeam,main.planetSelected,self.planetRollover) then return end
                    if main.planetNext~=self.planetRollover then main.planetNext=self.planetRollover end
                    if not self.fleetSelected and main.planetSelected~=main.planetNext then self:UpdateAction(); return end
                end
            end
            if gShellScreen_fnDefaultInputAccept(self) then return end
            if self.displayTimer then self.displayTimer=0; return end
            local team,planet=main.playerTeam,main.planetSelected
            if self.fleetSelected then
                if gPlatformStr~='PC' or self.CurButton=='_accept' then self:AttemptMove(team,planet,main.planetNext) end
            elseif not self:SelectFleet(team,planet) and self:IsValidBuild(team,planet) then
                if gPlatformStr~='PC' or self.CurButton=='_accept' then self:BuildFleet(team,planet) end
            end
            self:UpdateAction()
        end
        fleet.Input_Back=function(self)
            if self.fleetSelected then self.fleetSelected=nil; table.insert(did,'deselect')
            elseif self.displayTimer then self.displayTimer=nil; self:AttemptUndo(ifs_freeform_main.playerTeam) end
            self:UpdateAction()
        end
        fleet.Input_Misc=function(self,joystick)
            if gPlatformStr~='PC' then ScriptCB_PushScreen('ifs_freeform_summary') else self:Input_Back(joystick) end
        end
        return fleet
    end
    -- A galaxy shell with the pause menu and the stock fleet screen registered through the wrapper.
    function galaxy_shell()
        shell()
        did={}
        ScriptCB_PushScreen=function(screen) table.insert(did,'push '..screen) end
        ScriptCB_SetIFScreen=function(screen) table.insert(did,'set '..screen) end
        ScriptCB_IsPopupOpen=function() return Popup_Ok.active end
        -- The tab manager switches to a tab's screen; the selected tab returns its screen without switching.
        ifs_freeform_tab_layout={{tag='_tab_fleet',screen='ifs_freeform_fleet'},
            {tag='_tab_bonus',screen='ifs_freeform_purchase_tech'},{tag='_tab_units',screen='ifs_freeform_purchase_unit'}}
        ifelem_tabmanager_HandleInputAccept=function(screen,layout)
            for _,tab in ipairs(layout) do
                if tab.tag==screen.CurButton then
                    if tab.tag~='_tab_fleet' then ScriptCB_SetIFScreen(tab.screen) end
                    return tab.screen
                end
            end
        end
        assert(assert(loadstring(source))()==6217431)
        build_gc_tab()
        ifs_freeform_main={playerTeam=1,planetSelected='cor',planetNext='cor'}
        ifs_freeform_menu={Input_Accept=function(self) table.insert(did,'menu '..self.CurButton) end}
        AddIFScreen(ifs_freeform_menu,'ifs_freeform_menu',1)
        ifs_freeform_fleet=stock_fleet()
        AddIFScreen(ifs_freeform_fleet,'ifs_freeform_fleet')
        Popup_Ok.active=false
    end
end
-- Refused input and the pause menu.
local click, key, error_sounds, stack, versus_shell, on_stack
do
    -- Input helpers: a click on a planet, a key press, and the error sounds played so far.
    function click(planet) ifs_freeform_fleet.CurButton=nil; ifs_freeform_fleet.planetRollover=planet; ifs_freeform_fleet:Input_Accept() end
    function key(code) ifs_freeform_fleet:Input_KeyDown(code) end
    function error_sounds()
        local n=0
        for _,c in ipairs(calls) do if c.name=='sound' and c.arg[1]=='error' then n=n+1 end end
        return n
    end
    answers={galaxy_follow=true}
    galaxy_shell()
    -- Looking around stays: a click on another planet moves the cursor, a click on nothing does
    -- nothing, and neither asks the DLL.
    click('pol')
    assert(ifs_freeform_main.planetNext=='pol' and table.getn(did)==0 and not asked_dll('galaxy_follow'))
    click(nil)
    assert(table.getn(did)==0 and not said_prefix('GCOBS:galaxy:refused'))
    -- A click on the selected planet would select its fleet: refused, and the first refusal says why.
    ifs_freeform_main.planetNext='cor'
    click('cor')
    assert(not done('SelectFleet') and ifs_freeform_fleet.fleetSelected==nil and said('GCOBS:galaxy:refused:accept'))
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'the host, moves the fleets',1,true) and said('GCOBS:galaxy:told'))
    assert(string.len(Popup_Ok.title.ustring)<100)
    -- The friend's join closes it like the battle notes.
    assert(_GCT_battle_join_ready())
    accept_ok()
    -- Later refusals only play the error sound: Accept, Next, Tab and the Bonus and Units tabs.
    key(13); key(10); key(32); key(9)
    ifs_freeform_fleet.CurButton='_tab_units'; ifs_freeform_fleet:Input_Accept()
    ifs_freeform_fleet.CurButton='_tab_bonus'; ifs_freeform_fleet:Input_Accept()
    assert(table.getn(did)==0 and not Popup_Ok.active and error_sounds()==6)
    assert(said('GCOBS:galaxy:refused:tab'))
    -- Help, zoom, the Move tab and Back with nothing selected go through.
    key(-59); key(45); key(61)
    ifs_freeform_fleet.CurButton='_tab_fleet'; ifs_freeform_fleet:Input_Accept()
    key(8); ifs_freeform_fleet:Input_Misc()
    assert(table.concat(did,',')=='tutorial,zoom_out,zoom_in' and error_sounds()==6)
    -- Deselecting and undoing are refused, from Back, Backspace and Misc alike.
    ifs_freeform_fleet.fleetSelected=true
    ifs_freeform_fleet:Input_Back(); key(8); ifs_freeform_fleet:Input_Misc()
    assert(ifs_freeform_fleet.fleetSelected and not done('deselect') and error_sounds()==9)
    ifs_freeform_fleet.fleetSelected=nil; ifs_freeform_fleet.displayTimer=1
    ifs_freeform_fleet:Input_Back()
    assert(not done('AttemptUndo') and error_sounds()==10)
    ifs_freeform_fleet.displayTimer=nil
    -- In the pause menu, Load is refused with a note every time; Save, Resume and Quit work.
    for _=1,2 do
        ifs_freeform_menu.CurButton='load'; ifs_freeform_menu:Input_Accept()
        assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'loads a game',1,true))
        accept_ok()
    end
    for _,tag in ipairs({'save','resume','quit'}) do ifs_freeform_menu.CurButton=tag; ifs_freeform_menu:Input_Accept() end
    assert(table.concat(did,',')=='tutorial,zoom_out,zoom_in,menu save,menu resume,menu quit' and said('GCOBS:galaxy:refused:load'))
    -- Outside the host's galaxy (the host's PC, or the friend's own galaxy) every input acts as stock.
    answers={}
    did={}
    click('cor')
    assert(done('SelectFleet') and ifs_freeform_fleet.fleetSelected)
    ifs_freeform_fleet:Input_Back()
    assert(done('deselect'))
    key(9); key(32)
    ifs_freeform_menu.CurButton='load'; ifs_freeform_menu:Input_Accept()
    assert(done('set ifs_freeform_purchase_tech') and done('push ifs_freeform_summary') and done('menu load'))
    -- Once applying has stopped in this co-op game, the refusal note says so instead.
    answers={galaxy_follow=true,galaxy_stopped=true}
    galaxy_shell()
    click('cor')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'stopped following',1,true) and not done('SelectFleet'))
    accept_ok()
    ScriptCB_IsPopupOpen,ScriptCB_PushScreen,ScriptCB_SetIFScreen=nil,nil,nil
    answers={}

    -- While the co-op host's lobby has room, the pause menu's hidden friends button becomes Invite
    -- friends, laid out again after the stock Enter.
    local function pause_shell()
        shell()
        did={}
        ifsfreeformmenu_vbutton_layout={buttonlist={{tag='resume'},{tag='friends'},{tag='save'}}}
        ShowHideVerticalButtons=function(dest,layout)
            assert(layout==ifsfreeformmenu_vbutton_layout)
            table.insert(did,'layout friends='..tostring(not dest.friends.hidden))
            return 'resume'
        end
        assert(assert(loadstring(source))()==6217431)
        build_gc_tab()
        ifs_freeform_menu={buttons={resume={label={}},friends={label={}},save={label={}}},
            Enter=function(self) self.buttons.friends.hidden=true; table.insert(did,'enter') end,
            Input_Accept=function(self) table.insert(did,'menu '..self.CurButton) end}
        AddIFScreen(ifs_freeform_menu,'ifs_freeform_menu',1)
        Popup_Ok.active=false
    end
    local pause
    answers={party_invite=true}
    pause_shell()
    pause=ifs_freeform_menu
    calls={}; sent={}
    pause:Enter(1)
    assert(table.concat(did,',')=='enter,layout friends=true' and pause.CurButton=='resume' and called('SetCurButton'))
    assert(pause.buttons.friends.label.ustring=='U:Invite friends' and said('GCOBS:galaxy:invite_shown'))
    pause.CurButton='friends'; sent={}
    pause:Input_Accept()
    assert(said('GCOBS:menu:invite_more') and not done('menu friends') and called('sound') and not Popup_Ok.active)
    -- The overlay off, or the party full since the menu opened: it says so.
    answers={party_invite=true,no_overlay=true}; sent={}
    pause:Input_Accept()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'overlay is off',1,true) and not said('GCOBS:menu:invite_more'))
    accept_ok()
    answers={}
    pause:Input_Accept()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'party is full',1,true) and not said('GCOBS:menu:invite_more'))
    accept_ok()
    -- The other buttons are the stock ones.
    pause.CurButton='save'; pause:Input_Accept()
    assert(done('menu save'))
    -- Full when the menu opens: hidden again, and the button is the stock one.
    did={}; sent={}
    pause:Enter(1)
    assert(table.concat(did,',')=='enter,layout friends=false' and not said('GCOBS:galaxy:invite_shown'))
    pause.CurButton='friends'; pause:Input_Accept()
    assert(done('menu friends') and not said('GCOBS:menu:invite_more'))
    -- Not a co-op host (a friend's PC, Versus, single player): the stock menu.
    pause_shell()
    did={}; sent={}
    ifs_freeform_menu:Enter(1)
    assert(table.concat(did,',')=='enter' and ifs_freeform_menu.buttons.friends.label.ustring==nil)
    ifs_freeform_menu.CurButton='friends'; ifs_freeform_menu:Input_Accept()
    assert(done('menu friends') and not said('GCOBS:menu:invite_more'))
    -- Without the stock layout the menu is left as it is.
    shell()
    assert(assert(loadstring(source))()==6217431)
    ifsfreeformmenu_vbutton_layout=nil
    did={}
    local bare={buttons={friends={label={}}},Enter=function() table.insert(did,'enter') end,
        Input_Accept=function(self) table.insert(did,'menu '..self.CurButton) end}
    AddIFScreen(bare,'ifs_freeform_menu',1)
    answers={party_invite=true}
    bare:Enter(1); bare.CurButton='friends'; bare:Input_Accept()
    assert(table.concat(did,',')=='enter,menu friends')
    ShowHideVerticalButtons=function(dest,layout) return layout.buttonlist[1].tag end
    answers={}

    -- Versus: a stand-in for the stock custom setup's launch, with the screen stack recorded.
    function versus_shell(versus_answers)
        shell()
        stack={}
        answers=versus_answers or {}
        ScriptCB_PopScreen=function(name) table.insert(stack,'pop '..(name or '')) end
        ScriptCB_PushScreen=function(name) table.insert(stack,'push '..name) end
        ScriptCB_SndPlaySound=record('SndPlaySound')
        ifs_freeform_start_custom=function(main,prefs) table.insert(calls,{name='start_custom',arg={main,prefs}}) end
        ifs_freeform_controllers=function(main,map) table.insert(calls,{name='controllers',arg={main,map}}) end
        ifs_freeform_customsetup={Prefs={iEra=1,iVictoryType=3,iVictoryTurn=10}}
        ifs_freeform_customsetup.Input_Accept=function(self)
            ScriptCB_SndPlaySound('shell_menu_enter')
            ScriptCB_PopScreen()
            ifs_freeform_start_custom(ifs_freeform_main,self.Prefs)
            ScriptCB_PushScreen('ifs_freeform_main')
        end
        assert(assert(loadstring(source))()==6217431)
        build_gc_tab()
        AddIFScreen(ifs_freeform_customsetup,'ifs_freeform_customsetup')
    end
    function on_stack(entry)
        for _,e in ipairs(stack) do if e==entry then return true end end
        return false
    end
    versus_shell()
    -- Outside Versus the custom setup starts its single-player game.
    ifs_freeform_customsetup:Input_Accept()
    assert(called('start_custom') and on_stack('push ifs_freeform_main') and not said_prefix('GCOBS:menu:invite'))
    -- New opens the custom setup; its launch is held back, then the friend is invited.
    calls,stack,pushed={},{},{}
    press(ifs_gct_versus,'vs_new')
    assert(pushed[1]==ifs_freeform_customsetup)
    ifs_freeform_customsetup:Input_Accept()
    assert(not called('start_custom') and not on_stack('push ifs_freeform_main') and on_stack('pop '))
    assert(said('GCOBS:versus:setup:1:3') and said('GCOBS:menu:invite:vs_new') and Popup_Busy.active)
    answers={paired=true}
    popup_update(0.016)
    assert(not Popup_Busy.active and pushed[2]==ifs_gct_sides)
    -- Back from the setup (the Versus screen again) forgets a launch not taken.
    ifs_gct_versus.Enter(ifs_gct_versus,false)
    calls,stack={},{}
    ifs_freeform_customsetup:Input_Accept()
    assert(called('start_custom') and on_stack('push ifs_freeform_main'))
    -- Load: the stock load screen once paired, as Co-op's Load.
    pushed,sent={},{}
    answers={paired=true}
    press(ifs_gct_versus,'vs_load')
    popup_update(0.016)
    assert(said('GCOBS:menu:invite:vs_load') and ifs_freeform_load.Mode=='Load' and pushed[1]==ifs_freeform_load)

    -- The side screen on the host: the era's sides, the picks, Ready, the start.
    versus_shell()
    press(ifs_gct_versus,'vs_new')
    ifs_freeform_customsetup:Input_Accept()
    answers={paired=true,versus=true,versus_host=true}
    popup_update(0.016)
    sent={}
    ifs_gct_sides.Enter(ifs_gct_sides,true)
    assert(said('GCOBS:versus:sides') and said('GCOBS:versus:era:1'))
    assert(ifs_gct_sides.buttons.side1.label.ustring=='U:Republic' and ifs_gct_sides.buttons.side2.label.ustring=='U:CIS')
    press(ifs_gct_sides,'ready')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'Pick a side',1,true) and not said('GCOBS:versus:ready:1'))
    Popup_Ok.active=false
    press(ifs_gct_sides,'side2')
    assert(said('GCOBS:versus:pick:2'))
    answers.versus_mine_2=true
    answers.versus_peer_1=true
    ifs_gct_sides.Update(ifs_gct_sides,0.016)
    assert(ifs_gct_sides.buttons.side2.label.ustring=='U:CIS (you)' and ifs_gct_sides.buttons.side1.label.ustring=='U:Republic (your friend)')
    press(ifs_gct_sides,'ready')
    assert(said('GCOBS:versus:ready:1'))
    answers.versus_mine_ready=true
    press(ifs_gct_sides,'side1')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'Not ready',1,true) and not said('GCOBS:versus:pick:1'))
    Popup_Ok.active=false
    -- Ready on the side the friend is ready on is refused before it reaches the DLL.
    answers.versus_mine_ready=nil; answers.versus_peer_2=true; answers.versus_peer_1=nil; answers.versus_peer_ready=true
    sent={}
    press(ifs_gct_sides,'ready')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'ready on that side',1,true) and not said('GCOBS:versus:ready:1'))
    Popup_Ok.active=false
    -- Both ready on different sides: the host starts the galaxy from the kept setup, its one
    -- controller on its own side.
    answers.versus_peer_2=nil; answers.versus_peer_1=true; answers.versus_mine_ready=true; answers.versus_go=true
    calls,stack,sent={},{},{}
    for _=1,10 do ifs_gct_sides.Update(ifs_gct_sides,0.016) end
end
-- Versus: starting and loading a game.
local loaded
do
    local start=called('start_custom')
    assert(start and start.arg[1]==ifs_freeform_main and start.arg[2].iEra==1 and start.arg[2].iVictoryType==3)
    assert(start.arg[2]~=ifs_freeform_customsetup.Prefs)  -- a copy, kept from the launch
    local mapped=called('controllers')
    assert(mapped and mapped.arg[1]==ifs_freeform_main and mapped.arg[2][0]==2 and mapped.arg[2][1]==nil)
    assert(said('GCOBS:versus:started') and said('GCOBS:versus:host_side:2') and called('SetQuitPlayer'))
    assert(table.concat(stack,',')=='pop ,push ifs_freeform_main')
    calls,stack={},{}
    for _=1,20 do ifs_gct_sides.Update(ifs_gct_sides,0.016) end
    assert(not called('start_custom'))                       -- once
    -- Back leaves the Versus game for both.
    sent,stack={},{}
    ifs_gct_sides:Input_Back()
    assert(said('GCOBS:menu:leave') and on_stack('pop '))

    -- The friend's PC waits on a popup until the host reaches its side screen, then opens the side
    -- screen with the host's era.
    versus_shell()
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or Popup_Ok.active end
    ifs_sp_campaign={buttons={},Update=function() end}
    AddIFScreen(ifs_sp_campaign,'ifs_sp_campaign')
    local function friend_frames(n)
        for _=1,n do
            ifs_sp_campaign.Update(ifs_sp_campaign,0.016)
            if Popup_Busy.active then popup_update(0.016) end
        end
    end
    answers={guest_wait=true,versus=true}
    pushed={}
    friend_frames(15)
    assert(Popup_Busy.active and string.find(Popup_Busy.title.ustring,'Versus game',1,true))
    answers.versus_heard=true
    friend_frames(1)
    assert(not Popup_Busy.active and pushed[1]==ifs_gct_sides and said('GCOBS:menu:guest_sides'))
    friend_frames(45)                          -- a slow screen transition: pushed once
    assert(table.getn(pushed)==1)
    answers.versus_era_2=true
    sent={}
    ifs_gct_sides.Enter(ifs_gct_sides,true)
    assert(said('GCOBS:versus:sides') and not said_prefix('GCOBS:versus:era:'))
    assert(ifs_gct_sides.buttons.side1.label.ustring=='U:Rebellion' and ifs_gct_sides.buttons.side2.label.ustring=='U:Empire')
    -- The friend never starts the galaxy; once the host has, the picks are final.
    answers.versus_go=true; answers.versus_mine_1=true; answers.versus_mine_ready=true
    calls={}
    for _=1,20 do ifs_gct_sides.Update(ifs_gct_sides,0.016) end
    assert(not called('start_custom'))
    answers.versus_started=true
    sent={}
    ifs_gct_sides.Update(ifs_gct_sides,0.016)
    for _=1,10 do ifs_gct_sides.Update(ifs_gct_sides,0.016) end
    assert(ifs_gct_sides.buttons.ready.label.ustring=='U:Starting the galaxy...')
    press(ifs_gct_sides,'ready')
    assert(not said_prefix('GCOBS:versus:ready'))
    -- The host has left: the side screen closes and says so.
    answers={}
    stack,sent={},{}
    for _=1,10 do ifs_gct_sides.Update(ifs_gct_sides,0.016) end
    assert(on_stack('pop ') and said('GCOBS:versus:sides_closed') and Popup_Ok.active and
        string.find(Popup_Ok.title.ustring,'left the Versus game',1,true))
    Popup_Ok.active=false
    -- The host's PC never takes the friend's way to the side screen.
    answers={guest_wait=true,versus=true,versus_heard=true,versus_host=true}
    pushed={}
    friend_frames(30)
    assert(not pushed[1])
    answers={}
    friend_frames(30)
    ScriptCB_IsPopupOpen=nil

    -- The Versus galaxy: no AI screen during the other side's turn, no console side screen, and the
    -- game's end told to the other PC.
    versus_shell({versus=true})
    local galaxy_main={setupSides=1}
    galaxy_main.Enter=function(self,forward) ScriptCB_PushScreen('ifs_freeform_ai'); return 'entered',forward end
    galaxy_main.OneTimeInit=function(self) self.setupSides=1; return 'once' end
    galaxy_main.NextTurn=function(self) ScriptCB_PushScreen('ifs_freeform_ai'); return 'next' end
    AddIFScreen(galaxy_main,'ifs_freeform_main')
    local push_before=ScriptCB_PushScreen
    local e1,e2=galaxy_main.Enter(galaxy_main,true)
    assert(e1=='entered' and e2==true and stack[1]=='push ifs_freeform_fleet' and said('GCOBS:versus:ai_screen_skipped'))
    assert(ScriptCB_PushScreen==push_before)
    assert(galaxy_main:OneTimeInit()=='once' and galaxy_main.setupSides==nil and said('GCOBS:versus:sides_skipped'))
    stack={}
    galaxy_main.turnNumber=7
    assert(galaxy_main:NextTurn()=='next' and stack[1]=='push ifs_freeform_fleet' and not said_prefix('GCOBS:versus:over'))
    galaxy_main.teamVictory=2
    galaxy_main:NextTurn()
    assert(said('GCOBS:versus:over:2:7'))
    -- Only a build's first galaxy screen is wrapped; registering another leaves its Enter in place.
    galaxy_main.teamVictory=nil
    local raising={Enter=function() error('boom') end}
    AddIFScreen(raising,'ifs_freeform_main')
    assert(raising.Enter~=nil)
    -- An AI screen that ever shows in a Versus galaxy is logged.
    local ai={Enter=function() return 'ai' end}
    AddIFScreen(ai,'ifs_freeform_ai')
    assert(ai.Enter()=='ai' and said('GCOBS:versus:ai_entered'))
    -- Not Versus: the stock pushes, and the console side screen stays.
    answers={}
    sent,stack={},{}
    galaxy_main.Enter(galaxy_main,true)
    assert(stack[1]=='push ifs_freeform_ai' and not said('GCOBS:versus:ai_screen_skipped'))
    galaxy_main:OneTimeInit()
    assert(galaxy_main.setupSides==1)

    -- A loaded Versus galaxy: the stock LoadState gives this PC's controller the team saved under its
    -- profile name, else side 1. The save's third value is its profile table.
    local function load_shell(profiles,versus_answers)
        versus_shell(versus_answers)
        ScriptCB_GetProfileName=function(n) assert(n==1); return 'U:Ann' end
        ScriptCB_ununicode=function(u) return string.sub(u,3) end
        ScriptCB_LoadMetagameState=function()
            return {iEra=2},'custom',profiles,{},{},{},{},'pol',{},{},{},{},2,7,nil,7,'kas','pol',nil,{},nil,nil,{},{},{},false
        end
        local loader=ScriptCB_LoadMetagameState  -- the stock code keeps its own loader
        local main={}
        main.LoadState=function(self)
            local _,_,saved=loader()
            self.controllerTeam={[0]=saved[ScriptCB_ununicode(ScriptCB_GetProfileName(1))] or 1}
            ifs_freeform_controllers(self,{[0]=self.controllerTeam[0]})
            return 'loaded'
        end
        AddIFScreen(main,'ifs_freeform_main')
        calls={}
        return main
    end
    -- The sides mapped to this PC's controller, in call order.
    local function mapped_sides()
        local sides={}
        for _,c in ipairs(calls) do if c.name=='controllers' then table.insert(sides,c.arg[2][0]) end end
        return table.concat(sides,',')
    end
    -- The PC that saved hosts the load: its side, as LoadState gave it.
    loaded=load_shell({Ann=2},{versus=true})
    assert(loaded:LoadState()=='loaded' and mapped_sides()=='2' and said('GCOBS:versus:load:2:saved'))
    -- OneTimeInit runs LoadState twice a load; the side is reported once.
    sent={}
    loaded:LoadState()
    assert(not said_prefix('GCOBS:versus:load'))
    -- A save from the opponent's PC names only their profile: this player takes the other side.
    loaded=load_shell({Bob=1},{versus=true})
    loaded:LoadState()
    assert(mapped_sides()=='1,2' and said('GCOBS:versus:load:2:other'))
    -- Nothing to go by (two other profiles, or none): LoadState's mapping stands.
    loaded=load_shell({Bob=1,Cy=2},{versus=true})
    loaded:LoadState()
    assert(mapped_sides()=='1' and said('GCOBS:versus:load:1:start'))
    loaded=load_shell({},{versus=true})
    loaded:LoadState()
    assert(mapped_sides()=='1' and said('GCOBS:versus:load:1:start'))
    -- Without the game's loader nothing is mapped, and it says so.
    loaded=load_shell({Bob=1},{versus=true})
    ScriptCB_LoadMetagameState=nil
    assert(loaded:LoadState()=='loaded' and mapped_sides()=='1')
    assert(said('GCOBS:versus:load_unread') and not said_prefix('GCOBS:versus:load:'))
    -- Not Versus: the stock load, nothing changed or said.
    loaded=load_shell({Bob=1},{})
    loaded:LoadState()
    assert(mapped_sides()=='1' and not said_prefix('GCOBS:versus:load'))
    -- In both modes a galaxy load reports the save's first three values ("db" for Remaster settings) and
    -- rema_database, then that it loaded; a raised error's text is reported and the error goes on.
    loaded=load_shell({Ann=2},{})
    sent={}
    loaded:LoadState()
    assert(said('GCOBS:diag:galaxy_load:table:string:table:nil') and said('GCOBS:diag:galaxy_loaded'))
    ScriptCB_LoadMetagameState=function() return {isRemaDatabase=true},nil,'rep' end
    rema_database={data={}}
    sent={}
    loaded:LoadState()
    assert(said('GCOBS:diag:galaxy_load:db:nil:string:table') and said('GCOBS:diag:galaxy_loaded'))
    rema_database=nil
end
-- Versus: setup problems and the side select.
local card_events, pending, stock_card
do
    -- 27 values (Remaster settings in front of a whole galaxy): the first three are still reported.
    local many={{isRemaDatabase=true}}
    for i=2,27 do many[i]=i end
    ScriptCB_LoadMetagameState=function() return unpack(many) end
    sent={}
    loaded:LoadState()
    assert(said('GCOBS:diag:galaxy_load:db:number:number:nil'))
    ScriptCB_LoadMetagameState=nil
    sent={}
    loaded:LoadState()
    assert(said('GCOBS:diag:galaxy_load:unread:nil') and said('GCOBS:diag:galaxy_loaded'))
    versus_shell({})
    local broken={}
    broken.LoadState=function() error('bad save',0) end
    broken.OneTimeInit=function(self) self:LoadState() end
    AddIFScreen(broken,'ifs_freeform_main')
    sent={}
    local init_ok,init_problem=pcall(broken.OneTimeInit,broken)
    assert(not init_ok and init_problem=='bad save',tostring(init_problem))
    assert(said('GCOBS:diag:galaxy_init') and said('GCOBS:diag:galaxy_load_error:bad save'))
    assert(said('GCOBS:diag:galaxy_init_error:bad save') and not said('GCOBS:diag:galaxy_loaded'))
    ScriptCB_GetProfileName,ScriptCB_ununicode,ScriptCB_LoadMetagameState=nil,nil,nil

    -- The Versus lock: only the side taking its turn acts. The friend's co-op answer (galaxy_follow)
    -- plays no part.
    answers={versus=true}
    galaxy_shell()
    ScriptCB_PopScreen=function(name) table.insert(did,'pop '..(name or '')) end
    ifs_freeform_main.joystick=0           -- this PC's side is taking its turn
    ifs_freeform_main.planetNext='cor'
    click('cor')
    assert(done('SelectFleet') and not asked_dll('galaxy_follow'))
    -- Versus refusals only play the error sound, with no popup: Load from the pause menu, even in this
    -- PC's turn, and anything that would act in the other side's turn.
    ifs_freeform_menu.errorSound='error'   -- every shell screen has one
    local sounds=error_sounds()
    ifs_freeform_menu.CurButton='load'; ifs_freeform_menu:Input_Accept()
    assert(not done('menu load') and not Popup_Ok.active and error_sounds()==sounds+1 and said('GCOBS:galaxy:refused:load'))
    did={}
    ifs_freeform_main.joystick=nil         -- the other side's turn
    ifs_freeform_fleet.fleetSelected=nil
    click('cor'); click('cor'); key(13); key(9)
    ifs_freeform_fleet.CurButton='_tab_bonus'; ifs_freeform_fleet:Input_Accept()
    assert(table.getn(did)==0 and said('GCOBS:galaxy:refused:accept') and said('GCOBS:galaxy:refused:tab'))
    assert(not Popup_Ok.active and not said('GCOBS:galaxy:told') and error_sounds()==sounds+6)
    ifs_freeform_menu.CurButton='load'; ifs_freeform_menu:Input_Accept()
    assert(not done('menu load') and not Popup_Ok.active and error_sounds()==sounds+7)
    ifs_freeform_menu.CurButton='save'; ifs_freeform_menu:Input_Accept()
    assert(done('menu save'))
    -- Notes: no popup on either side of a hand-off; this PC's turn starts after the hand-off, and the
    -- game's end shows from this side.
    Popup_Ok.active=false
    sent={}
    for _=1,3 do _GCT_battle_note() end      -- the other side's turn: nothing shows
    assert(not Popup_Ok.active and not said_prefix('GCOBS:versus:wait'))
    ifs_freeform_main.joystick=0
    _GCT_versus_turn_start=5
    did={}
    _GCT_battle_note()
    assert(table.concat(did,',')=='pop ifs_freeform_main,push ifs_freeform_fleet' and said('GCOBS:versus:turn_start:5'))
    assert(_GCT_versus_turn_start==nil and not Popup_Ok.active)
    for _=1,3 do _GCT_battle_note() end      -- the new fleet screen's notes: nothing shows
    assert(not Popup_Ok.active and table.getn(did)==2 and not said_prefix('GCOBS:versus:turn_told'))
    did={}
    answers.versus_over_1=true
    ifs_freeform_main.joystick=nil
    _GCT_battle_note()
    assert(ifs_freeform_main.teamVictory==1 and said('GCOBS:versus:end_shown:1'))
    assert(table.concat(did,',')=='pop ifs_freeform_main,push ifs_freeform_end')
    did={}
    _GCT_battle_note()
    assert(table.getn(did)==0)                -- once
    -- The two galaxies no longer agree: the first refusal says so, as in co-op.
    answers={versus=true,galaxy_stopped=true}
    galaxy_shell()
    click('cor')
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'no longer agree',1,true) and not done('SelectFleet'))
    assert(said('GCOBS:galaxy:told'))
    accept_ok()
    ScriptCB_IsPopupOpen,ScriptCB_PushScreen,ScriptCB_SetIFScreen,ScriptCB_PopScreen=nil,nil,nil,nil
    ifs_freeform_customsetup,ifs_freeform_start_custom,ifs_freeform_controllers=nil,nil,nil
    answers={}

    -- Versus battles: the attacker's PC hosts, and the joiner fights on its own side, not the announced one.
    join_shell()
    answers={versus=true}
    ifs_freeform_main.controllerTeam={[0]=2}
    ifs_freeform_main.teamCode={[1]='all',[2]='imp'}
    -- The busy popup's elapsed-time line is emptied and stays so; these waits have their own timeouts.
    Popup_Busy.BusyTimeStr={ustring='U:0:42'}
    assert(_GCT_battle_join('GCT-0a1b2c3d\npol1g_con\nall\npol\nall=1111100,imp=1111111\n'))
    assert(sides[1][1]==0 and sides[1][2]=='imp' and said('GCOBS:battle:own_side:imp'))
    assert(string.find(Popup_Busy.title.ustring,"opponent's battle",1,true))
    assert(Popup_Busy.bCallerSetsString==1 and Popup_Busy.BusyTimeStr.ustring=='U:')
    popup_back()
    assert(Popup_Busy.bCallerSetsString==nil)
    Popup_Busy.BusyTimeStr=nil
    -- Co-op: the joiner takes the host's side.
    join_shell()
    answers={}
    ifs_freeform_main.controllerTeam={[0]=2}
    ifs_freeform_main.teamCode={[1]='all',[2]='imp'}
    assert(_GCT_battle_join('GCT-0a1b2c3d\npol1g_con\nall\npol\nall=1111100,imp=1111111\n'))
    assert(sides[1][2]=='all' and not said_prefix('GCOBS:battle:own_side'))
    assert(string.find(Popup_Busy.title.ustring,"friend's battle",1,true))
    popup_back()
    -- In Versus the cursor moves to the battle's planet before the galaxy is saved, so a kept result
    -- lands there. Not for a world that is no planet here (spa), nor in co-op.
    local function cursor_join(versus_answers,world)
        join_shell()
        answers=versus_answers
        ifs_freeform_main.controllerTeam={[0]=2}
        ifs_freeform_main.teamCode={[1]='all',[2]='imp'}
        ifs_freeform_main.modelMatrix={pol={},kas={}}
        ifs_freeform_main.planetNext='kas'
        ifs_freeform_main.SaveState=function(self) table.insert(saves,'SaveState:'..tostring(self.planetNext)) end
        assert(_GCT_battle_join('GCT-0a1b2c3d\npol1g_con\nall\n'..world..'\nall=1111100,imp=1111111\n'))
        popup_back()
    end
    cursor_join({versus=true},'pol')
    assert(saves[1]=='SaveState:pol' and said('GCOBS:battle:planet_next:pol'))
    cursor_join({versus=true},'spa')
    assert(saves[1]=='SaveState:kas' and not said_prefix('GCOBS:battle:planet_next'))
    cursor_join({},'pol')
    assert(saves[1]=='SaveState:kas' and not said_prefix('GCOBS:battle:planet_next'))
    ifs_freeform_main.modelMatrix,ifs_freeform_main.planetNext=nil,nil
    -- Versus battle notes speak of the opponent, and a Versus quit loses the battle. Co-op's are unchanged.
    Popup_Busy.active=false; Popup_Ok.active=false
    ScriptCB_IsPopupOpen=function() return Popup_Busy.active or Popup_Ok.active end
    answers={versus=true,battle_note_friend_quit=true}
    _GCT_battle_note()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'counts as their loss',1,true) and said('GCOBS:battle:noted:friend_quit'))
    accept_ok()
    answers={battle_note_friend_quit=true}
    _GCT_battle_note()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'Your friend left that battle',1,true))
    accept_ok()
    answers={versus=true,battle_note_join_failed=true}
    _GCT_battle_note()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,"Could not join your opponent's battle",1,true))
    accept_ok()
    -- An OK popup's text is laid out by the stock gPopup_fnSetTitleUStr once the popup is open. If the
    -- helper fails, the text is still set.
    local laid={}
    gPopup_fnSetTitleUStr=function(popup,u) assert(popup==Popup_Ok and popup.active); table.insert(laid,u); popup.title.ustring=u end
    answers={battle_note_friend_quit=true}
    _GCT_battle_note()
    assert(Popup_Ok.active and table.getn(laid)==1 and laid[1]=='U:Your friend left that battle before it ended.')
    accept_ok()
    gPopup_fnSetTitleUStr=function() error('no layout') end
    answers={versus=true,battle_note_friend_quit=true}
    _GCT_battle_note()
    assert(Popup_Ok.active and string.find(Popup_Ok.title.ustring,'counts as their loss',1,true))
    accept_ok()
    gPopup_fnSetTitleUStr=nil
    ScriptCB_IsPopupOpen=nil
    answers={}

    -- Versus: the defender picks its own bonus card on the battle card screen.
    -- The stock battle card screen on PC, with each step recorded; the AI's draw is always the last card.
    function stock_card()
        local c={title={text={}},info={caption={},text={}},vis={}}
        c.SetSelected=function(self,sel)
            if self.selected==sel then return end
            self.selected=sel
            self:UpdateAction()
        end
        c.UpdateAction=function(self)
            local item=self.selected and self.useActive[self.selected]
            IFObj_fnSetVis(self.info,item and item.using>0)
            if item and item.using>0 then
                IFText_fnSetString(self.info.caption,item.name)
                IFText_fnSetString(self.info.text,item.name..'_info')
            end
        end
        c.Enter=function(self)
            local main=ifs_freeform_main
            ifs_freeform_SetButtonVis(self,'misc',main.joystick)
            ifs_freeform_SetButtonVis(self,'accept',main.joystick)
            IFText_fnSetString(self.title.text,'ifs.freeform.usecard')
            self.useActive={}
            local count,first=0,nil
            for i,using in ipairs(ifs_purchase_tech_using[main.playerTeam]) do
                if using>0 then
                    local t=ifs_purchase_tech_table[using]
                    local item={slot=i,using=using,weight=0,name=t.name,bonus=t.bonus}
                    for _,hint in ipairs(t.hints) do
                        if string.find(main.launchMission,hint[1]) then item.weight=hint[2]; break end
                    end
                    count=count+1
                    self.useActive[count]=item
                    if not first and item.weight>0 then first=count end
                end
            end
            self.selected=nil
            if count==0 then
                self:Next()
            elseif main.joystick then
                self:SetSelected(first)
                ifs_freeform_SetButtonVis(self,'accept',self.selected)
                table.insert(card_events,'wait')
            else
                self:SetSelected(count)
                self:AcceptBonus()
            end
            self:UpdateAction()
        end
        c.AcceptBonus=function(self)
            local main=ifs_freeform_main
            local item=self.selected and self.useActive[self.selected]
            if item then
                main.activeBonus[main.playerTeam]=item.bonus
                ActivateBonus(main.playerTeam,'team_bonus_'..item.bonus)
                ifs_purchase_tech_using[main.playerTeam][item.slot]=0
                self.displayTimer=3
            else
                self.displayTimer=0.5
            end
        end
        c.Next=function(self)
            local main=ifs_freeform_main
            if self.defending then
                self.defending=nil
                main:SetActiveTeam(3-main.playerTeam)
                table.insert(card_events,'launch')    -- SaveState, SaveMissionSetup and EnterMission
            else
                self.defending=true
                main:SetActiveTeam(3-main.playerTeam)
                ScriptCB_PushScreen('ifs_freeform_battle_card')
            end
        end
        -- A click picks the card under the mouse or presses Accept or Skip (_next); Enter and Space are
        -- Accept and Skip.
        c.Input_Accept=function(self)
            if self.CurButton=='_accept' then
                if not self.selected then return end
                if self.displayTimer then self.displayTimer=0 else self:AcceptBonus() end
            elseif self.CurButton=='_next' then
                self:Next()
            elseif self.CurButton~='_back' and self.rollover then
                self:SetSelected(self.rollover)
            end
        end
        c.Input_KeyDown=function(self,key)
            if key==13 then self.CurButton='_accept'; self:Input_Accept(-1)
            elseif key==32 then self.CurButton='_next'; self:Input_Accept(-1) end
        end
        -- The right arrow moves to the next card that suits the battle.
        c.Input_GeneralRight=function(self)
            if not self.selected or self.displayTimer then return end
            for i=self.selected+1,table.getn(self.useActive) do
                if self.useActive[i].weight>0 then self:SetSelected(i); return end
            end
        end
        c.Input_Back=function(self) end
        c.Input_Start=function(self)
            if self.displayTimer then return end
            ScriptCB_PushScreen('ifs_freeform_menu')
        end
        c.Update=function(self,dt)
            if self.displayTimer then
                self.displayTimer=self.displayTimer-dt
                if self.displayTimer<0 then self.displayTimer=nil; self:Next() end
            end
        end
        return c
    end
end
-- Versus: the defender's card pick.
local settle, card_frames, last_event, sends, card, own
do
    -- A battle card shell: team takes its turn, and this PC's controller plays local_team.
    local function card_shell(team,local_team,with)
        shell()
        card_events,pending={},{}
        answers=with
        ScriptCB_PushScreen=function(name) table.insert(card_events,'push '..name); table.insert(pending,name) end
        ScriptCB_PopScreen=function(...) table.insert(card_events,'pop'); assert(arg.n==0) end
        ScriptCB_IsPopupOpen=function() return Popup_Busy.active or Popup_Ok.active end
        ActivateBonus=function(t,name) table.insert(card_events,'activate '..t..' '..name) end
        IFText_fnSetString=function(t,s) assert(type(t)=='table'); t.string=s; t.ustring=nil end
        IFObj_fnSetVis=function(o,v) assert(type(o)=='table'); o.shown=v and true or false end
        ifs_freeform_SetButtonVis=function(screen,name,v) screen.vis[name]=v and true or false end
        ifs_purchase_tech_table={[4]={name='bacta',bonus='bacta',hints={{'yav',5}}},[7]={name='turrets',bonus='autoturrets',hints={{'.*',1}}}}
        assert(assert(loadstring(source))()==6217431)
        build_gc_tab()
        ifs_freeform_battle_card=stock_card()
        AddIFScreen(ifs_freeform_battle_card,'ifs_freeform_battle_card')
        assert(said('GCOBS:versus:card_wrapped'))
        local main={playerTeam=team,teamController={[local_team]=0},controllerTeam={[0]=local_team},
            teamCode={[1]='all',[2]='imp'},activeBonus={},launchMission='yav1g_con'}
        main.joystick=main.teamController[team]
        main.SetActiveTeam=function(self,t) self.playerTeam=t; self.joystick=self.teamController[t]; table.insert(card_events,'active '..t) end
        ifs_freeform_main=main
        Popup_Busy.active=false; Popup_Ok.active=false
    end
    -- A pushed screen enters after the call that pushed it returns, as in the game.
    function settle()
        while pending[1] do
            if table.remove(pending,1)=='ifs_freeform_battle_card' then ifs_freeform_battle_card:Enter() end
        end
    end
    function card_frames(n,dt) for _=1,n do ifs_freeform_battle_card:Update(dt or 0.016); settle() end end
    function last_event() return card_events[table.getn(card_events)] end
    -- How many messages to the DLL start with prefix.
    function sends(prefix)
        local n=0
        for _,s in ipairs(sent) do if string.sub(s,1,string.len(prefix))==prefix then n=n+1 end end
        return n
    end
    -- Attacker's PC: its player picks its own card; for the defender the AI's draw is held while the
    -- defender's PC is asked, shown in the title and info panel since the cards would cover a popup.
    card_shell(1,1,{versus=true,versus_paired=true})
    ifs_purchase_tech_using={[1]={4,0,0},[2]={4,0,7}}
    card=ifs_freeform_battle_card
    card:Enter(); settle()
    assert(card_events[1]=='wait' and card.selected==1 and card.vis.accept and not said_prefix('GCOBS:versus:card_ask'))
    card:Input_Start()                                         -- its own pass: the pause menu, as in the stock game
    assert(last_event()=='push ifs_freeform_menu'); pending={}
    card:AcceptBonus()
    assert(ifs_freeform_main.activeBonus[1]=='bacta' and ifs_purchase_tech_using[1][1]==0)
    card_frames(1,4)
    assert(card.defending and ifs_freeform_main.playerTeam==2 and ifs_freeform_main.joystick==nil)
    assert(said('GCOBS:versus:card_ask:yav1g_con:4,0,7') and card.card_wait and not Popup_Busy.active)
    assert(string.find(card.title.text.ustring,'picking a card',1,true))
    assert(card.info.shown and string.find(card.info.caption.ustring,'picking a card',1,true))
    assert(string.find(card.info.text.ustring,'Esc lets the AI pick',1,true))
    assert(card.selected==nil and not card.vis.accept and not card.vis.misc)
    assert(ifs_freeform_main.activeBonus[2]==nil and ifs_purchase_tech_using[2][3]==7)
    -- Its player cannot pick for the defender: clicks, keys and arrows do nothing.
    card.rollover=1; card.CurButton=nil; card:Input_Accept()
    card:Input_KeyDown(13); card:Input_KeyDown(32); card:Input_GeneralRight()
    assert(card.selected==nil and card.card_wait and card.defending and last_event()~='launch')
    card_frames(10)
    assert(card.card_wait and not said_prefix('GCOBS:versus:card_done'))   -- no answer yet
    -- The defender picks the card in slot 1: it is shown and taken, and the stock Next launches.
    answers.versus_card_answered=true; answers.versus_card_slot_1=true
    card_frames(5)
    assert(not card.card_wait and said('GCOBS:versus:card_done:1') and card.selected==1)
    assert(card.title.text.string=='ifs.freeform.usecard' and card.title.text.ustring==nil and card.info.caption.string=='bacta')
    assert(ifs_freeform_main.activeBonus[2]=='bacta' and ifs_purchase_tech_using[2][1]==0 and ifs_purchase_tech_using[2][3]==7)
    card_frames(1,4)
    assert(last_event()=='launch' and ifs_freeform_main.playerTeam==1)
    -- No card picked: none for the defender. With no attacker card, the defender's wait shows after the
    -- stock Enter hides the info panel.
    card_shell(1,1,{versus=true,versus_paired=true})
    ifs_purchase_tech_using={[1]={0,0,0},[2]={4,0,7}}
    card=ifs_freeform_battle_card
    card:Enter(); settle()
    assert(card.defending and card.card_wait and card.info.shown and not Popup_Busy.active)
    answers.versus_card_answered=true
    card_frames(5)
    assert(said('GCOBS:versus:card_done:0') and ifs_freeform_main.activeBonus[2]==nil and ifs_purchase_tech_using[2][3]==7)
    assert(card.displayTimer and card.displayTimer<0.5 and not card.info.shown)
    -- No answer in 60 s, or Esc (Back or Start): the AI's draw, and no pause menu.
    card_shell(1,1,{versus=true,versus_paired=true})
    ifs_purchase_tech_using={[1]={0,0,0},[2]={4,0,7}}
    card=ifs_freeform_battle_card
    card:Enter(); settle()
    card_frames(5,11)
    assert(card.card_wait and not said_prefix('GCOBS:versus:card_done'))
    card_frames(5,2)
    assert(not card.card_wait and said('GCOBS:versus:card_done:ai') and ifs_freeform_main.activeBonus[2]=='autoturrets')
    for _,key in ipairs({'Input_Back','Input_Start'}) do
        card_shell(1,1,{versus=true,versus_paired=true})
        ifs_purchase_tech_using={[1]={0,0,0},[2]={4,0,7}}
        card=ifs_freeform_battle_card
        card:Enter(); settle()
        card[key](card)
        assert(said('GCOBS:versus:card_done:ai') and ifs_freeform_main.activeBonus[2]=='autoturrets')
        assert(not card.card_wait and last_event()~='push ifs_freeform_menu')
    end
    -- Not paired, the defender without a card, or co-op: the stock draw at once.
    for _,with in ipairs({{versus=true},{}}) do
        card_shell(1,1,with)
        ifs_purchase_tech_using={[1]={0,0,0},[2]={4,0,7}}
        ifs_freeform_battle_card:Enter(); settle()
        assert(not said_prefix('GCOBS:versus:card_ask') and not ifs_freeform_battle_card.card_wait)
        assert(ifs_freeform_main.activeBonus[2]=='autoturrets')
    end
    card_shell(1,1,{versus=true,versus_paired=true})
    ifs_purchase_tech_using={[1]={0,0,0},[2]={0,0,0}}
    ifs_freeform_battle_card:Enter(); settle()
    assert(not said_prefix('GCOBS:versus:card_ask') and last_event()=='launch')
    -- The defender's PC: the DLL shows this side's pass with the attacker's copy of its slots, and the
    -- galaxy is put back afterwards.
    card_shell(1,2,{versus=true})
    own={4,4,7}
    ifs_purchase_tech_using={[1]={4,0,0},[2]=own}
    ifs_freeform_main.launchMission=nil
    card=ifs_freeform_battle_card
    assert(_GCT_battle_join_ready())
    assert(_GCT_versus_card('3\nyav1g_con\n4,0,7')==true)
    assert(said('GCOBS:versus:card_shown:3') and ifs_freeform_main.playerTeam==2 and ifs_freeform_main.joystick==0)
    assert(ifs_purchase_tech_using[2]~=own and ifs_purchase_tech_using[2][2]==0 and ifs_freeform_main.launchMission=='yav1g_con')
    assert(card_events[1]=='active 2' and card_events[2]=='push ifs_freeform_battle_card')
    settle()
    assert(card_events[3]=='wait' and card.selected==1 and card.vis.accept and not card.card_wait)
    assert(not _GCT_battle_join_ready())                       -- the join waits for the pick
    assert(_GCT_versus_card('4\nyav1g_con\n4,0,7')==false)     -- one at a time
    card:Input_GeneralRight()                                  -- the card in slot 3
    card.CurButton='_accept'; card:Input_Accept()              -- answered at once
    assert(card.selected==2 and said('GCOBS:versus:card:3:3') and card.displayTimer==3)
    card:Input_Start()                                         -- no pause menu while the card shows
    assert(last_event()~='push ifs_freeform_menu' and last_event()~='pop')
    card_frames(1,4)                                           -- Next puts the galaxy back and pops
    assert(ifs_purchase_tech_using[2]==own and own[3]==7 and ifs_freeform_main.launchMission==nil)
    assert(ifs_freeform_main.activeBonus[2]==nil and ifs_freeform_main.playerTeam==1 and ifs_freeform_main.joystick==nil)
    assert(last_event()=='pop' and said('GCOBS:versus:card_closed:answered') and sends('GCOBS:versus:card:3:')==1)
    assert(_GCT_battle_join_ready())
    -- Skip, Back or Start: no card, and never the pause menu, whose Save would keep the borrowed slots.
    assert(_GCT_versus_card('5\nyav1g_con\n4,0,7')); settle()
    card.CurButton='_next'; card:Input_Accept()
    assert(said('GCOBS:versus:card:5:0') and last_event()=='pop' and ifs_freeform_main.playerTeam==1)
    assert(_GCT_versus_card('6\nyav1g_con\n4,0,7')); settle()
    card:Input_Back()
    assert(said('GCOBS:versus:card:6:0') and last_event()=='pop' and ifs_purchase_tech_using[2]==own)
    assert(_GCT_versus_card('7\nyav1g_con\n4,0,7')); settle()
    card:Input_Start()
    assert(said('GCOBS:versus:card:7:0') and last_event()=='pop' and ifs_purchase_tech_using[2]==own)
    -- A patch of the attacker's turn during the pick replaces the purchase and bonus tables; the newer
    -- tables stay as the patch left them.
    assert(_GCT_versus_card('12\nyav1g_con\n4,0,7')); settle()
end
-- Bonus cards, quitting and the Remaster settings.
do
    local patched_using,patched_bonus={[1]={4,0,0},[2]={0,4,7}},{[1]='bacta'}
    ifs_purchase_tech_using,ifs_freeform_main.activeBonus=patched_using,patched_bonus
    card.CurButton='_next'; card:Input_Accept()
    assert(said('GCOBS:versus:card:12:0') and last_event()=='pop' and ifs_freeform_main.playerTeam==1)
    assert(ifs_purchase_tech_using==patched_using and patched_using[2][1]==0 and patched_using[2][2]==4)
    assert(ifs_freeform_main.activeBonus==patched_bonus and patched_bonus[2]==nil)
    ifs_purchase_tech_using,ifs_freeform_main.activeBonus={[1]={4,0,0},[2]=own},{}
    -- Once the attacker moves past the request, the screen closes unanswered.
    assert(_GCT_versus_card('8\nyav1g_con\n4,0,7')); settle()
    answers.versus_card_open=true
    card_frames(20)
    assert(last_event()~='pop')
    answers.versus_card_open=nil
    card_frames(15)
    assert(last_event()=='pop' and said('GCOBS:versus:card_closed:unanswered') and sends('GCOBS:versus:card:8:')==0)
    assert(ifs_freeform_main.playerTeam==1 and ifs_purchase_tech_using[2]==own and _GCT_battle_join_ready())
    -- No card in the request: answered without a screen. With a popup open: later.
    local pushes=table.getn(card_events)
    assert(_GCT_versus_card('9\nyav1g_con\n0,0,0')==true and said('GCOBS:versus:card:9:0') and table.getn(card_events)==pushes)
    Popup_Ok.active=true
    assert(_GCT_versus_card('10\nyav1g_con\n4,0,7')==false and table.getn(card_events)==pushes)
    Popup_Ok.active=false
    assert(not pcall(_GCT_versus_card,'junk'))
    ScriptCB_IsPopupOpen,ScriptCB_PushScreen,ScriptCB_PopScreen,ActivateBonus=nil,nil,nil,nil
    IFText_fnSetString,IFObj_fnSetVis,ifs_freeform_SetButtonVis=nil,nil,nil
    ifs_purchase_tech_using,ifs_purchase_tech_table=nil,nil
    answers={}

    -- Exit to Windows: the DLL hears it first so the other PC leaves at once, then the stock call runs.
    -- A shell without the call gets no wrapper.
    shell()
    local quits={}
    ScriptCB_QuitToWindows=function(...) table.insert(quits,{n=arg.n,told=said('GCOBS:menu:closing')}) end
    local stock_quit=ScriptCB_QuitToWindows
    assert(assert(loadstring(source))()==6217431)
    assert(ScriptCB_QuitToWindows~=stock_quit and not said('GCOBS:menu:closing'))
    ScriptCB_QuitToWindows()
    assert(table.getn(quits)==1 and quits[1].told and quits[1].n==0)
    shell()
    ScriptCB_QuitToWindows=nil
    assert(assert(loadstring(source))()==6217431 and ScriptCB_QuitToWindows==nil)

    -- The Remaster's SetState and EnterMission fail without its settings (rema_database), so its
    -- defaults are put in place once, before either runs.
    local made=0
    local function remaster_defaults() made=made+1; return {data={},scripts_IF={},scripts_IG={}} end
    answers={}
    battle_shell({defaults=remaster_defaults})
    sent={}
    SetState('shell')
    assert(type(rema_database)=='table' and made==1 and said('GCOBS:diag:remaster_defaults:state'))
    assert(net_called('SetState').arg[1]=='shell')
    net={}; sent={}
    SetState('shell')
    assert(made==1 and not said_prefix('GCOBS:diag:remaster') and net_called('SetState').arg[1]=='shell')
    -- Its own settings stay.
    local own={data={customColor=2}}
    battle_shell({settings=own,defaults=remaster_defaults})
    made=0
    SetState('shell')
    assert(rema_database==own and made==0 and not said_prefix('GCOBS:diag:remaster') and net_called('SetState'))
    -- Without the Remaster, SetState is unchanged.
    battle_shell()
    SetState('shell')
    assert(rema_database==nil and net_called('SetState').arg[1]=='shell' and not said_prefix('GCOBS:diag:remaster'))
    -- Defaults that cannot be made are reported, and the Remaster's own error stands.
    battle_shell({defaults=function() error('no defaults') end})
    assert(not pcall(SetState,'shell') and said('GCOBS:diag:remaster_defaults_failed:state') and not net_called('SetState'))
    battle_shell({defaults=function() return 7 end})
    assert(not pcall(SetState,'shell') and said('GCOBS:diag:remaster_defaults_failed:state'))
    -- Before a battle: the stock launch, the hosted one and the friend's join.
    battle_shell({defaults=remaster_defaults})
    ifs_freeform_battle_card:Next()
    assert(said('GCOBS:diag:remaster_defaults:battle') and remaster_saves==1 and entered[1]==0)
    answers={battle_host=true}
    battle_shell({defaults=remaster_defaults})
    ifs_freeform_battle_card:Next()
    hosting_frames(9)
    assert(said('GCOBS:battle:step:wrappers') and remaster_saves==1 and entered[1]==1)
    assert(said('GCOBS:diag:remaster_defaults:battle'))
    answers={}
    join_shell({defaults=remaster_defaults})
    assert(_GCT_battle_join(announced)==true and said('GCOBS:diag:remaster_defaults:join') and remaster_saves==1)
    rema_database,swbf2Remaster_getDefaultSettings=nil,nil

    -- Missing shell helpers: nothing is touched.
    shell()
    AddVerticalButtons=nil
    local list=custom_GetGCButtonList
    assert(assert(loadstring(source))()==-1)
    assert(custom_GetGCButtonList==list and not said('GCOBS:menu:installed'))
    print('player_menu_test PASS: Online menu, co-op party, battles, notes, locks and Versus')
end
