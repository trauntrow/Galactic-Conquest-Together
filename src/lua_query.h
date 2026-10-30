// Runs fixed Lua chunks on the game's own Lua state and leaves its stack as it found it.

#pragma once
#include "frame_probe.h"

namespace gc {
// The engine's own Lua 5.0 C API; load is luaL_loadbuffer and pushstring is lua_pushlstring.
struct LuaQueryApi {
    int(__cdecl* gettop)(void*);
    void(__cdecl* settop)(void*, int);
    int(__cdecl* load)(void*, const char*, unsigned, const char*);
    int(__cdecl* pcall)(void*, int, int, int);
    void(__cdecl* pushstring)(void*, const char*, unsigned) = nullptr;
};
// What a query did: how far it got, its load and call statuses, and whether the stack is unchanged.
struct LuaQueryResult {
    int stage = 0, before = -1, after = -1, load_status = -1, call_status = -1;
    bool called = false, restored = false, prefix_equal = false, value_ok = false;
    char error[120] = {};  // a failed load or call's error string, truncated
};
// Runs the built-in query on an idle Lua state, between script calls.
LuaQueryResult run_lua_query(void* state, const LuaQueryApi& api);
// Runs source, which returns a function, and calls that function with data as its string argument.
LuaQueryResult run_lua_data_query(void* state, const LuaQueryApi& api, const char* source, unsigned source_size,
                                  const char* data, unsigned data_size);
// Runs source as a nested call inside a C function the game is running with exactly args arguments.
LuaQueryResult run_nested_lua_query(void* state, const LuaQueryApi& api, const char* source, unsigned source_size,
                                    int args);
// True when argument index of the C function running on state is exactly the string expected.
bool lua_string_argument(void* state, int index, const char* expected);
// Checks the game's Lua API and arms the install; false when the API differs.
bool initialize_lua_query(HMODULE game);
// Called by the frame hook on every frame, on the game thread.
void step_lua_query(const FrameSample& sample);
// Logs the install's progress and result; called from the proxy's worker thread.
void report_lua_query();
}
