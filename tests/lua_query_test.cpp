// Tests RearmGate and the Lua query runner (src/lua_query.cpp) against a fake Lua 5.0 state and C API.
#include "../src/lua_query.h"
#include "../src/rearm_gate.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

// The fake lua_State (h, fields at the engine's offsets), its stack and call info, and API call counts.
static DWORD h[22], slots[160], relocated[160], ci[2], new_ci[2];
static int mode, loads, calls, resets;
// A Lua 5.0 TString for the error tests: length at +12, bytes at +16.
alignas(8) static unsigned char error_string[16 + 256];
static void set_error(const char* text) {
    const DWORD length = static_cast<DWORD>(strlen(text));
    memset(error_string, 0, sizeof error_string);
    memcpy(error_string + 12, &length, 4);
    memcpy(error_string + 16, text, length + 1);
}
// The fake C API; `mode` picks what the load and the call do, from success to each failure.
static int __cdecl gettop(void*) { return (h[2] - h[3]) / 8 + (mode == 7 ? 1 : 0); }
static void __cdecl settop(void*, int n) {
    ++resets;
    h[2] = h[3] + n * 8;
}
static int __cdecl load(void*, const char* source, unsigned size, const char*) {
    ++loads;
    if (size != 14 || memcmp(source, "return 6217431", 14)) return 99;
    if (mode == 3) {
        memcpy(relocated, slots, sizeof slots);
        const DWORD delta = reinterpret_cast<DWORD>(relocated) - reinterpret_cast<DWORD>(slots);
        for (int i : {2, 3, 6, 7}) h[i] += delta;
        new_ci[0] = h[3];
        new_ci[1] = h[6];
        h[5] = h[10] = reinterpret_cast<DWORD>(new_ci);
    }
    auto top = reinterpret_cast<DWORD*>(h[2]);
    top[0] = mode == 1 || mode == 13 ? 4 : 6;
    top[1] = 0;
    if (mode == 13) top[1] = reinterpret_cast<DWORD>(error_string);
    h[2] += 8;
    return mode == 1 || mode == 13 ? 3 : 0;
}
static int __cdecl pcall(void*, int nargs, int nresults, int errorfunc) {
    ++calls;
    if (mode >= 8 && mode <= 11) {
        assert(nresults == 1 && errorfunc == 0);
        if (calls == 1) {
            assert(nargs == 0);
            reinterpret_cast<DWORD*>(h[2] - 8)[0] = mode == 10 ? 3 : 6;
            return 0;
        }
        assert(nargs == 1 && calls == 2);
        h[2] -= 8;
        auto value = reinterpret_cast<DWORD*>(h[2] - 8);
        value[0] = mode == 9 ? 4 : 3;
        const float number = 6217431.0f;
        memcpy(value + 1, &number, 4);
        if (mode == 11) *reinterpret_cast<DWORD*>(h[3]) = 0xdead;
        return mode == 9 ? 1 : 0;
    }
    if (nargs || nresults != 1 || errorfunc) return 99;
    auto value = reinterpret_cast<DWORD*>(h[2] - 8);
    if (mode == 12) {
        value[0] = 4;
        value[1] = reinterpret_cast<DWORD>(error_string);
        return 2;
    }
    // A table, not a string.
    if (mode == 14) {
        value[0] = 5;
        value[1] = reinterpret_cast<DWORD>(error_string);
        return 2;
    }
    value[0] = mode == 2 ? 4 : 3;
    const float number = 6217431.0f;
    memcpy(value + 1, &number, 4);
    if (mode == 4) *reinterpret_cast<DWORD*>(h[3]) = 0xdead;
    if (mode == 5) h[2] = h[3];
    return mode == 2 ? 2 : 0;
}
static void __cdecl pushstring(void*, const char* data, unsigned size) {
    assert(size == 3 && data[0] == 'a' && data[1] == 0 && data[2] == 'b');
    auto value = reinterpret_cast<DWORD*>(h[2]);
    value[0] = 4;
    value[1] = 0;
    h[2] += 8;
}
// A fresh fake state: idle, with 8 values on its stack.
static void setup(int m) {
    mode = m;
    loads = calls = resets = 0;
    memset(h, 0, sizeof h);
    memset(slots, 0, sizeof slots);
    h[7] = reinterpret_cast<DWORD>(slots);
    h[3] = h[7] + 8;
    h[2] = h[3] + 64;
    h[6] = h[7] + sizeof(slots) - 8;
    ci[0] = h[3];
    ci[1] = h[6];
    h[5] = h[10] = reinterpret_cast<DWORD>(ci);
    for (int i = 2; i < 18; ++i) slots[i] = 0x100 + i;
}
int main() {
    {
        // RearmGate: after a successful install, each new shell rebuild runs once after 120 stable frames.
        gc::RearmGate gate;
        using Gate = gc::RearmGate;
        auto tick = [&](unsigned epoch, unsigned context = 10, unsigned thread = 20, unsigned flow = 3,
                        bool guards = true) { return gate.step(epoch, context, thread, flow, guards); };
        // A failed install never runs, and a successful one waits for the next rebuild.
        gate.arm(1, 10, 20, false);
        for (int i = 0; i < 130; ++i) assert(tick(2) == Gate::wait);
        gate.arm(2, 10, 20, true);
        for (int i = 0; i < 130; ++i) assert(tick(2) == Gate::wait);
        // A different context settles without spending an attempt.
        for (int i = 0; i < 119; ++i) assert(tick(3, 30) == Gate::wait);
        assert(tick(3, 30) == Gate::context_mismatch && gate.attempts == 0);
        // Coming back needs a new rebuild, not just the same address again.
        for (int i = 0; i < 130; ++i) assert(tick(3) == Gate::wait);
        for (int i = 0; i < 119; ++i) assert(tick(4) == Gate::wait);
        assert(tick(4, 10, 99) == Gate::wait);  // another thread restarts the count
        for (int i = 0; i < 119; ++i) assert(tick(4) == Gate::wait);
        assert(tick(4, 10, 20, 3, false) == Gate::wait);  // so does a failed guard
        for (int i = 0; i < 119; ++i) assert(tick(4) == Gate::wait);
        assert(tick(4, 30) == Gate::wait);  // and another context
        for (int i = 0; i < 119; ++i) assert(tick(4) == Gate::wait);
        assert(tick(4, 10, 20, 4) == Gate::wait);  // and another flow
        for (int i = 0; i < 130; ++i) assert(tick(4, 10, 20, 5) == Gate::wait);
        for (int i = 0; i < 119; ++i) assert(tick(4) == Gate::wait);
        assert(tick(4) == Gate::execute && gate.attempts == 1);
        assert(tick(4) == Gate::wait);
        // Each later rebuild runs once more, up to the four-attempt limit.
        for (unsigned epoch = 5; epoch <= 7; ++epoch) {
            for (int i = 0; i < 119; ++i) assert(tick(epoch) == Gate::wait);
            assert(tick(epoch) == Gate::execute);
        }
        for (int i = 0; i < 119; ++i) assert(tick(8) == Gate::wait);
        assert(tick(8) == Gate::attempt_limit && gate.attempts == 4);
        gate.enabled = false;  // disabled: never runs
        for (int i = 0; i < 130; ++i) assert(tick(9) == Gate::wait);
        puts(
            "PASS: rearm success gate, reload boundary, context/thread/flow stability, failure disable and attempt bound");
    }
    // run_lua_query, modes 0 to 7: success, load and call errors, a moved stack, a corrupted or
    // shrunk stack, and two states it refuses.
    const gc::LuaQueryApi api{gettop, settop, load, pcall};
    for (int m = 0; m <= 7; ++m) {
        setup(m);
        if (m == 6) h[11] = 1 << 16;
        const auto r = gc::run_lua_query(h, api);
        if (m == 6 || m == 7) {
            if (r.called || loads || calls || resets) return 10 + m;
        } else if (m == 5) {
            if (r.restored || resets || r.stage != 2) return 20;
        } else {
            if (!r.called || !r.restored || r.before != 8 || r.after != 8 || resets != 1) return 30 + m;
            if (r.prefix_equal != (m != 4)) return 40 + m;
            if (r.value_ok != (m != 1 && m != 2)) return 50 + m;
            if (loads != 1 || calls != (m == 1 ? 0 : 1)) return 60 + m;
        }
    }
    if (gc::run_lua_query(reinterpret_cast<void*>(1), api).called) return 70;  // an unreadable state
    {
        // A failed load or call keeps a string error for the log, cut to fit, control characters as
        // spaces; nothing for any other value or on success.
        set_error("[string \"GCT\"]:1: attempt to index global `rema_database' (a nil value)");
        setup(12);
        auto r = gc::run_lua_query(h, api);
        assert(r.call_status == 2 && !r.value_ok && r.restored && r.prefix_equal);
        assert(!strcmp(r.error, "[string \"GCT\"]:1: attempt to index global `rema_database' (a nil value)"));
        set_error("no\tsuch\nfile");
        setup(13);
        r = gc::run_lua_query(h, api);
        assert(r.load_status == 3 && calls == 0 && r.restored && !strcmp(r.error, "no such file"));
        std::string long_text(300, 'x');
        set_error(long_text.substr(0, 250).c_str());
        setup(12);
        r = gc::run_lua_query(h, api);
        assert(strlen(r.error) == sizeof r.error - 1 && r.error[0] == 'x');
        setup(14);  // not a string
        r = gc::run_lua_query(h, api);
        assert(r.call_status == 2 && !r.error[0]);
        setup(0);
        r = gc::run_lua_query(h, api);
        assert(r.value_ok && !r.error[0]);
        puts(
            "PASS: a failed call's or load's error kept for the log (cut to fit, control characters as spaces), none otherwise");
    }
    // run_lua_data_query: the loaded chunk returns a function, called with a binary string (embedded NUL).
    const gc::LuaQueryApi data_api{gettop, settop, load, pcall, pushstring};
    for (int m = 8; m <= 11; ++m) {
        setup(m);
        const auto r = gc::run_lua_data_query(h, data_api, "return 6217431", 14, "a\0b", 3);
        assert(r.stage == 3 && r.restored && r.before == 8 && r.after == 8);
        assert(r.prefix_equal == (m != 11));
        assert(r.value_ok == (m == 8 || m == 11));
        assert(calls == (m == 10 ? 1 : 2));
    }
    // Without pushstring the data query stops before the call, with the stack restored.
    setup(8);
    const auto missing = gc::run_lua_data_query(h, api, "return 6217431", 14, "a\0b", 3);
    assert(missing.call_status == -2 && missing.restored && missing.prefix_equal && calls == 1);
    puts("PASS: binary argument factory, embedded NUL, body error, wrong factory type and stack prefix checks");
    {
        // Inside a C call (the shell's custom_gc_0.lvl probe) the frame-hook entry refuses to run; the
        // nested entry runs with the expected argument count and restores the stack exactly.
        DWORD outer_ci[2]{};
        // A call in progress: its own call info, nCcalls and an error jump set.
        auto in_call = [&] {
            setup(0);
            outer_ci[0] = h[3];
            outer_ci[1] = h[6];
            h[5] = reinterpret_cast<DWORD>(outer_ci);
            h[11] = 3u << 16;
            h[20] = 0x1234;
        };
        in_call();
        const auto idle_only = gc::run_lua_query(h, api);
        assert(!idle_only.called && !loads && !calls);
        in_call();
        const auto nested = gc::run_nested_lua_query(h, api, "return 6217431", 14, 8);
        assert(nested.called && nested.stage == 3 && nested.restored && nested.prefix_equal && nested.value_ok);
        assert(nested.before == 8 && nested.after == 8 && loads == 1 && calls == 1 && resets == 1);
        in_call();
        const auto wrong_args = gc::run_nested_lua_query(h, api, "return 6217431", 14, 1);
        assert(!wrong_args.called && !loads);
        in_call();
        mode = 4;  // the script disturbs the caller's arguments
        const auto disturbed = gc::run_nested_lua_query(h, api, "return 6217431", 14, 8);
        assert(disturbed.restored && !disturbed.prefix_equal);
        in_call();
        h[3] += 4;  // a misaligned base, not a stack the engine leaves
        assert(!gc::run_nested_lua_query(h, api, "return 6217431", 14, 8).called && !loads);
        // The probe's argument: a Lua 5.0 TString (length at +12, bytes at +16).
        alignas(8) unsigned char name[16 + 32]{};
        const char probe[] = "custom_gc_0.lvl";
        const DWORD length = sizeof probe - 1;
        memcpy(name + 12, &length, 4);
        memcpy(name + 16, probe, sizeof probe);
        setup(0);
        h[2] = h[3] + 8;
        slots[2] = 4;
        slots[3] = reinterpret_cast<DWORD>(name);
        assert(gc::lua_string_argument(h, 1, "custom_gc_0.lvl"));
        assert(!gc::lua_string_argument(h, 1, "custom_gc_1.lvl") && !gc::lua_string_argument(h, 1, "custom_gc_0.lv"));
        assert(!gc::lua_string_argument(h, 2, "custom_gc_0.lvl"));  // only one argument
        slots[2] = 3;                                               // a number, not a string
        assert(!gc::lua_string_argument(h, 1, "custom_gc_0.lvl"));
        puts(
            "PASS: nested entry inside a C call (argument count, stack restore, caller prefix, alignment) and the probe's string argument");
    }
    puts("PASS: query success, parser/call errors, stack relocation, corruption, shrink refusal and idle/count guards");
    return 0;
}
