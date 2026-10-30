    -- Patch script body: brings the running galaxy to the host's revision in place, without a load.
    -- human, versus and hints come from the shared checks (tools/embed-player-apply.ps1).
    local main = ifs_freeform_main
    local fleet = ifs_freeform_fleet
    -- All checks run before anything is changed.
    assert(type(main) == 'table' and main.scenario == expected[2])
    -- The side taking its turn before this revision, for the Versus hand-off below.
    local was = main.playerTeam
    assert((main.playerTeam == 1 or main.playerTeam == 2) and (expected[13] == 1 or expected[13] == 2))
    assert(type(main.fleetPtr) == 'table' and type(main.fleetClass) == 'table' and type(main.modelMatrix) == 'table')
    assert(type(CreateEntity) == 'function' and type(DeleteEntity) == 'function')
    -- SetActiveTeam, which switches the team taking its turn as NextTurn does, reads these.
    assert(type(main.SetActiveTeam) == 'function' and type(main.teamCode) == 'table')
    assert(type(main.teamController) == 'table' and type(ScriptCB_SetHotController) == 'function')
    assert(type(ifs_freeform_purchase_unit) == 'table' and type(ifs_freeform_purchase_unit.SetActiveSide) == 'function')
    -- A planetFleet value of 0 stands for both teams' fleets.
    local function teams(team)
        if team == 0 then return {1, 2} end
        return {team}
    end
    for planet, team in pairs(expected[7]) do
        assert(type(planet) == 'string' and type(main.modelMatrix[planet]) == 'table')
        assert(team == 0 or team == 1 or team == 2)
        for _, t in ipairs(teams(team)) do
            assert(main.fleetClass[t] ~= nil and main.modelMatrix[planet][t] ~= nil)
        end
    end
    for _, models in pairs(main.fleetPtr) do assert(type(models) == 'table') end
    -- The zoom level shown now: the fleet screen's on the host's turn (either side's in Versus), else
    -- level 1, which the AI screen sets.
    local shown = 1
    if type(fleet) == 'table' and (main.playerTeam == human or versus) then shown = fleet.zoomLevel end
    -- Whether two tables hold the same keys and values, one level deep.
    local function same(a, b)
        for k, v in pairs(a) do if b[k] ~= v then return false end end
        for k, v in pairs(b) do if a[k] ~= v then return false end end
        return true
    end
    -- After a fleet move, the fleet models are rebuilt from planetFleet, as Enter builds them.
    if type(main.planetFleet) ~= 'table' or not same(main.planetFleet, expected[7]) then
        for _, models in pairs(main.fleetPtr) do
            for _, entity in pairs(models) do DeleteEntity(entity) end
        end
        main.fleetPtr = {[1] = {}, [2] = {}}
        for planet, team in pairs(expected[7]) do
            for _, t in ipairs(teams(team)) do
                main.fleetPtr[t][planet] = CreateEntity(main.fleetClass[t], main.modelMatrix[planet][t])
            end
        end
    end
    -- The saved values LoadState assigns, except the ones the host leaves out.
    main.custom = expected[1]
    main.lastSelected = expected[4]
    main.lastFleet = expected[5]
    main.planetTeam = expected[6]
    main.planetFleet = expected[7]
    main.teamResources = expected[9]
    ifs_purchase_unit_owned = expected[10]
    ifs_purchase_tech_cards = expected[11]
    ifs_purchase_tech_using = expected[12]
    main.turnNumber = expected[14]
    main.activeBonus = expected[20]
    main.recentPlanets = expected[23] or {}
    main.planetResources = expected[24]
    main.battleResources = expected[25]
    -- The battle values the host leaves out are cleared, as LoadState clears them; a Versus PC's own
    -- battle leaves them behind.
    main.curScreen = expected[15]
    main.winnerTeam = expected[21]
    main.fleetBattle = expected[22]
    if main.playerTeam ~= expected[13] then main:SetActiveTeam(expected[13]) end
    -- Planets and fleet icons are drawn from these tables every frame; the credits text is refreshed
    -- the way the fleet screen refreshes it.
    if type(fleet) == 'table' then
        fleet.turnNumber = expected[16]
        fleet.planetStart = expected[17]
        fleet.planetNext = expected[18]
        if fleet.player ~= nil then main:UpdatePlayerText(fleet.player) end
    end
    -- The camera shows the planet the host's camera shows (planetSelected, which every SelectPlanet
    -- sets). Without that hint the cursor stands in.
    local camera = hints[2]
    if camera == nil then camera = expected[8] end
    if type(camera) == 'string' and type(main.modelMatrix[camera]) == 'table' and type(main.SelectPlanet) == 'function' then
        local info = nil
        if type(fleet) == 'table' then info = fleet.info end
        main:SelectPlanet(info, camera)
    end
    -- SelectPlanet also moves the cursor, and the AI marks its destination with it before its fleet
    -- flies, so the cursor is set afterwards.
    main.planetNext = expected[8]
    -- The host's zoom level, or level 1 on the AI's turn. In Versus the other side's turn comes from
    -- its player's fleet screen, with that level.
    local zoom = hints[3]
    if zoom ~= 0 and zoom ~= 1 then zoom = nil end
    local view = zoom
    if expected[13] ~= human and not versus then view = 1 end
    if type(fleet) == 'table' and type(main.SetZoom) == 'function' then
        if zoom ~= nil then fleet.zoomLevel = zoom end
        if view ~= nil and view ~= shown then main:SetZoom(view) end
    end
    -- Versus: this revision hands the turn to this PC. The fleet screen's notes start it on a frame
    -- with no popup open, as NextTurn does (player_menu.lua versus_note).
    if versus and was ~= human and expected[13] == human then
        rawset(_G, '_GCT_versus_turn_start', expected[14])
    end
    return 6217431  -- the DLL's success value
end
