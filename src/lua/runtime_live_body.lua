    -- Live script body: compares the running galaxy with the received state; no save, no load.
    assert(type(ifs_freeform_main) == 'table' and type(ifs_freeform_load) == 'table')
    assert(type(ifs_freeform_load_LoadMetagameSuccess) == 'function')
    assert(ifs_freeform_main.wasSplit == nil or ifs_freeform_main.wasSplit == 1)
    -- Deep comparison of two values, at most 16 tables deep.
    local function equal(a,b,depth)
        assert(depth <= 16)
        if type(a) ~= type(b) then return false end
        if type(a) ~= 'table' then return a == b end
        for k,v in pairs(a) do if not equal(v,b[k],depth+1) then return false end end
        for k,v in pairs(b) do if a[k] == nil then return false end end
        return true
    end
    -- Screen navigation fields change as the galaxy starts, so only these saved values are compared.
    local fields = {[2]='scenario',[6]='planetTeam',[7]='planetFleet',
        [9]='teamResources',[13]='playerTeam',[14]='turnNumber',[21]='winnerTeam'}
    for index,field in pairs(fields) do
        assert(equal(rawget(ifs_freeform_main,field),expected[index],0))
    end
    -- The purchase globals, compared the same way.
    local purchases = {[10]='ifs_purchase_unit_owned',[11]='ifs_purchase_tech_cards',
        [12]='ifs_purchase_tech_using'}
    for index,field in pairs(purchases) do
        assert(equal(rawget(_G,field),expected[index],0))
    end
    return 6217431  -- the DLL's success value
end
