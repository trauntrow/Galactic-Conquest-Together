    -- Verify script body: reads the saved galaxy state back and checks it equals the received values.
    assert(type(ifs_freeform_load_LoadMetagameSuccess) == 'function')
    assert(type(ifs_freeform_main) == 'table' and type(ifs_freeform_load) == 'table')
    assert(ifs_freeform_main.wasSplit == nil or ifs_freeform_main.wasSplit == 1)
    assert(type(ScriptCB_LoadMetagameState) == 'function')
    -- Deep comparison of two values, at most 16 tables deep.
    local function equal(a,b,depth)
        assert(depth <= 16)
        if type(a) ~= type(b) then return false end
        if type(a) ~= 'table' then return a == b end
        for k,v in pairs(a) do if not equal(v,b[k],depth+1) then return false end end
        for k,v in pairs(b) do if a[k] == nil then return false end end
        return true
    end
    -- The loader's results with their count, nils included.
    local function packed(...) return arg end
    local actual = packed(ScriptCB_LoadMetagameState())
    assert(actual.n == expected.n)
    for i=1,expected.n do assert(equal(actual[i],expected[i],0)) end
    return 6217431  -- the DLL's success value
end
