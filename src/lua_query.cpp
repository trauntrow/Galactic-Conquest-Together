// Runs Lua chunks on the game's own Lua state from the frame hook: the shell scripts' install,
// applying the host's galaxy on a friend's PC, and the shared network battles.

#include "lua_query.h"
#include "rearm_gate.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#if GC_PLAYER_APPLY
#include "save_transaction.h"
#include "apply_logic.h"
#include "apply_mailbox.h"
#include "session_logic.h"
#include "transition_guards.h"
#endif
#if GC_PLAYER_MENU
#include "battle_net.h"
#include "bots_hook.h"
#include "../third_party/minhook/include/MinHook.h"
#endif

namespace gc {
namespace {
// The fixed scripts, embedded at build time. Every chunk returns 6217431 when it succeeds.
#if GC_SCREEN_OBSERVER
#include "screen_observer.inc"
#else
constexpr char query[] = "return 6217431";
#endif
#if GC_PLAYER_APPLY
#include "player_apply.inc"
#endif
// The start of a Lua 5.0 lua_State, read as DWORDs: top w[2], base w[3], ci w[5], stack_last w[6],
// stack w[7], base_ci w[10], nCcalls in the high half of w[11], errorJmp w[20].
struct Header {
    DWORD w[22];
};
bool header(void* state, Header& h) { return state && read_memory(state, &h, sizeof h); }
// The stack pointers are in order, 8-byte aligned and of plausible size.
bool coherent(const Header& h) {
    const DWORD top = h.w[2], base = h.w[3], last = h.w[6], stack = h.w[7];
    return stack && base >= stack && top >= base && last >= top && ((base - stack) % 8 == 0) &&
           ((top - base) % 8 == 0) && top - base <= 65 * 8 && last - stack <= 1024 * 1024 && h.w[5];
}
// No Lua call is running: ci is base_ci, nCcalls is 0 and no protected call is active.
bool idle(const Header& h) { return coherent(h) && h.w[5] == h.w[10] && (h.w[11] >> 16) == 0 && h.w[20] == 0; }
// Copies the first count stack slots, to check that a query left them unchanged.
bool prefix(const Header& h, unsigned count, void* bytes) {
    return !count || read_memory(reinterpret_cast<void*>(h.w[3]), bytes, count * 8);
}
LuaQueryApi native_api{};        // the engine's own Lua API
unsigned char* image = nullptr;  // base of the game's executable image
#if GC_PLAYER_APPLY
// The engine's metagame save buffer: its length at +0x1B9C7C8 and its 512 bytes at +0x1B9C8D0.
struct NativeSaveIO {
    // Fails if the length changes while the bytes are read.
    bool read(SavedBuffer& out) {
        DWORD after = 0;
        return read_memory(image + 0x1b9c7c8, &out.length, 4) &&
               read_memory(image + 0x1b9c8d0, out.bytes.data(), out.bytes.size()) &&
               read_memory(image + 0x1b9c7c8, &after, 4) && after == out.length;
    }
    bool write(DWORD rva, const void* bytes, SIZE_T size) {
        SIZE_T written = 0;
        return WriteProcessMemory(GetCurrentProcess(), image + rva, bytes, size, &written) && written == size;
    }
    bool length(std::uint32_t value) { return write(0x1b9c7c8, &value, 4); }
    bool buffer(const std::array<unsigned char, 512>& bytes) { return write(0x1b9c8d0, bytes.data(), bytes.size()); }
};
// Checks that the engine's metagame save and load routines still use that buffer and length.
bool native_save_guards() {
    unsigned char save[] = {0x68, 0x00, 0x02, 0x00, 0x00, 0xba, 0, 0,    0,    0, 0xe8, 0x31, 0x07,
                            0,    0,    0xa3, 0,    0,    0,    0, 0x83, 0xc4, 4, 0x33, 0xc0, 0xc3};
    unsigned char load[] = {0xff, 0x35, 0, 0, 0, 0, 0xba, 0, 0, 0, 0, 0xe8, 0x90, 0x07, 0, 0, 0x83, 0xc4, 4, 0xc3};
    const DWORD buffer = reinterpret_cast<DWORD>(image) + 0x1b9c8d0;
    const DWORD length = reinterpret_cast<DWORD>(image) + 0x1b9c7c8;
    memcpy(save + 6, &buffer, 4);
    memcpy(save + 16, &length, 4);
    memcpy(load + 2, &length, 4);
    memcpy(load + 7, &buffer, 4);
    unsigned char actual_save[sizeof save], actual_load[sizeof load];
    return read_memory(image + 0x18a1a0, actual_save, sizeof actual_save) &&
           read_memory(image + 0x18a1c0, actual_load, sizeof actual_load) && !memcmp(save, actual_save, sizeof save) &&
           !memcmp(load, actual_load, sizeof load);
}
// Checks the engine's SetState callback and helper against known bytes, relocated to this image.
bool transition_guards() {
    auto check = [](DWORD rva, const unsigned char* expected, unsigned size, const unsigned* offsets, unsigned count) {
        unsigned char adjusted[128]{}, actual[128]{};
        if (size > sizeof adjusted) return false;
        memcpy(adjusted, expected, size);
        for (unsigned i = 0; i < count; ++i) {
            DWORD address = 0;
            memcpy(&address, adjusted + offsets[i], 4);
            address += reinterpret_cast<DWORD>(image) - 0x400000;
            memcpy(adjusted + offsets[i], &address, 4);
        }
        return read_memory(image + rva, actual, size) && !memcmp(actual, adjusted, size);
    };
    const unsigned callback_offsets[] = {10};
    const unsigned helper_offsets[] = {2, 29, 37, 41, 47, 58, 62, 68, 79, 89};
    return check(0x18b5f0, setstate_callback, sizeof setstate_callback, callback_offsets, 1) &&
           check(0x13ae10, setstate_helper, sizeof setstate_helper, helper_offsets, 10);
}
#endif
// The one-shot install's deadline, and the thread and Lua state its stable frames are counted on.
ULONGLONG deadline = 0;
DWORD observed_thread = 0, observed_context = 0;
#if GC_PLAYER
// The shell is ready once the Galactic Conquest screens exist. This check only reads Lua state and
// is retried after each stable interval.
constexpr char ready_query[] =
    "if type(ScriptCB_IsFileExist)=='function' and type(AddIFScreen)=='function' and "
    "type(ifs_freeform_main)=='table' and type(ifs_main)=='table' then return 6217431 end return 0";
bool shell_ready = false;
bool apply_enabled = false;
std::atomic<unsigned> ready_probes{0};
std::atomic<bool> ready_published{false};
#endif
unsigned stable_frames = 0;
bool finished = false;
// A shell rebuild orphans the installed script, so it is installed again, but only into the Lua
// state of the first install.
#if GC_CAMPAIGN_CAPTURE
RearmGate rearm;
ULONGLONG probe_last_log = 0;
#endif
// One slot per install attempt, each written once, so the logging thread never reads a partial result.
LuaQueryResult results[1 + RearmGate::max_attempts];
unsigned result_count = 0;
std::atomic<unsigned> published_count{0};
// Appends a result and makes it visible to the logging thread.
void publish(const LuaQueryResult& result) {
    if (result_count >= 1 + RearmGate::max_attempts) return;
    results[result_count++] = result;
    published_count.store(result_count, std::memory_order_release);
}
#if GC_CAMPAIGN_CAPTURE
// The chunk ran, returned 6217431 and left the stack as it found it.
bool succeeded(const LuaQueryResult& r) {
    return r.stage == 3 && r.called && !r.load_status && !r.call_status && r.restored && r.prefix_equal && r.value_ok;
}
#endif
}

// Copies the Lua string in a stack slot (TValue type 4; TString length at +12, bytes at +16),
// truncated, with control characters as spaces. Empty for any other value.
static void lua_text(DWORD slot, char* out, size_t size) {
    DWORD value[2]{}, length = 0;
    if (!size) return;
    out[0] = 0;
    if (size < 2 || !read_memory(reinterpret_cast<void*>(slot), value, sizeof value) || value[0] != 4 || !value[1] ||
        !read_memory(reinterpret_cast<void*>(value[1] + 12), &length, 4))
        return;
    const size_t n = length < size - 1 ? length : size - 1;
    if (!read_memory(reinterpret_cast<void*>(value[1] + 16), out, n)) {
        out[0] = 0;
        return;
    }
    out[n] = 0;
    for (size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(out[i]) < 32) out[i] = ' ';
}
// A query's error text for a log line, or "-".
static inline const char* lua_error(const LuaQueryResult& r) { return r.error[0] ? r.error : "-"; }
// Loads and calls a chunk, then restores the stack. With nested < 0 it runs between script calls;
// otherwise inside a running C function that has exactly `nested` arguments.
static LuaQueryResult run_source(void* state, const LuaQueryApi& api, const char* source, unsigned source_size,
                                 const char* data = nullptr, unsigned data_size = 0, int nested = -1) {
    LuaQueryResult r;
    Header initial{}, current{};
    unsigned char saved[64 * 8]{}, comparison[64 * 8]{};
    // Between script calls the state must be idle, inside a C call only coherent; either way it needs
    // 4 free slots on the stack and in the current call's frame.
    const auto ready = [nested](const Header& h) { return nested < 0 ? idle(h) : coherent(h); };
    if (!header(state, initial) || !ready(initial) || initial.w[6] - initial.w[2] < 32) return r;
    DWORD ci[2]{};
    if (!read_memory(reinterpret_cast<void*>(initial.w[5]), ci, sizeof ci) || ci[0] != initial.w[3] ||
        ci[1] < initial.w[2] || ci[1] - initial.w[2] < 32)
        return r;
    r.stage = 1;
    r.before = api.gettop(state);
    if (r.before < 0 || r.before > 64 || static_cast<DWORD>(r.before) != (initial.w[2] - initial.w[3]) / 8 ||
        (nested >= 0 && r.before != nested) || !prefix(initial, r.before, saved))
        return r;
    r.called = true;
    r.load_status = api.load(state, source, source_size, "=GCT-single-shot-query");
    if (r.load_status == 0) r.call_status = api.pcall(state, 0, 1, 0);
    if (data && !r.load_status && !r.call_status) {
        // The chunk returned a function: call it with the data as a string, so received bytes are never
        // parsed as source.
        DWORD value[2]{};
        if (!api.pushstring || data_size > 4096 || !header(state, current) || !ready(current) ||
            api.gettop(state) != r.before + 1 || !read_memory(reinterpret_cast<void*>(current.w[2] - 8), value, 8) ||
            value[0] != 6) {
            r.call_status = -2;
        } else {
            api.pushstring(state, data, data_size);
            r.call_status = api.pcall(state, 1, 1, 0);
        }
    }
    r.stage = 2;
    // The call may have moved the stack: re-read the header and compare offsets, never old pointers.
    if (!header(state, current) || !ready(current) || current.w[3] - current.w[7] != initial.w[3] - initial.w[7] ||
        (current.w[2] - current.w[3]) / 8 < static_cast<DWORD>(r.before))
        return r;
    const int count = api.gettop(state);
    if (count != static_cast<int>((current.w[2] - current.w[3]) / 8)) return r;
    // A failed load or call leaves its error message on top.
    if ((r.load_status || r.call_status) && count > r.before) lua_text(current.w[2] - 8, r.error, sizeof r.error);
    if (r.load_status == 0 && r.call_status == 0 && count == r.before + 1) {
        DWORD value[2]{};
        if (read_memory(reinterpret_cast<void*>(current.w[2] - 8), value, sizeof value)) {
            float number;
            memcpy(&number, value + 1, sizeof number);
            // A TValue is its type then its payload; type 3 is a number, a 32-bit float in this engine.
            r.value_ok = value[0] == 3 && number == 6217431.0f;
        }
    }
    r.prefix_equal = prefix(current, r.before, comparison) && !memcmp(saved, comparison, r.before * 8);
    // Pop the result, or the error, back to the original top.
    api.settop(state, r.before);
    r.stage = 3;
    r.after = api.gettop(state);
    r.restored = header(state, current) && ready(current) && r.after == r.before &&
                 current.w[2] - current.w[3] == static_cast<DWORD>(r.before * 8) &&
                 current.w[3] - current.w[7] == initial.w[3] - initial.w[7];
    r.prefix_equal =
        r.prefix_equal && prefix(current, r.before, comparison) && !memcmp(saved, comparison, r.before * 8);
    return r;
}

LuaQueryResult run_lua_query(void* state, const LuaQueryApi& api) {
    return run_source(state, api, query, sizeof(query) - 1);
}
LuaQueryResult run_lua_data_query(void* state, const LuaQueryApi& api, const char* source, unsigned source_size,
                                  const char* data, unsigned data_size) {
    return run_source(state, api, source, source_size, data, data_size);
}
LuaQueryResult run_nested_lua_query(void* state, const LuaQueryApi& api, const char* source, unsigned source_size,
                                    int args) {
    return run_source(state, api, source, source_size, nullptr, 0, args);
}
#if GC_PLAYER
// The string on top of the engine's current Lua state, if any, for the log line of the game's exit.
void current_lua_text(char* out, size_t size) {
    DWORD state = 0;
    Header h{};
    if (size) out[0] = 0;
    if (!image || !read_memory(image + 0x1a579a0, &state, 4) || !header(reinterpret_cast<void*>(state), h) ||
        h.w[2] < 8)
        return;
    lua_text(h.w[2] - 8, out, size);
}
#endif

// Reads the argument's TValue and TString directly, without calling into Lua.
bool lua_string_argument(void* state, int index, const char* expected) {
    Header h{};
    DWORD value[2]{}, length = 0;
    const DWORD size = static_cast<DWORD>(strlen(expected));
    char text[64]{};
    if (!header(state, h) || !coherent(h) || index < 1 || h.w[2] - h.w[3] < static_cast<DWORD>(index) * 8 ||
        size >= sizeof text || !read_memory(reinterpret_cast<void*>(h.w[3] + (index - 1) * 8), value, sizeof value) ||
        value[0] != 4 || !value[1] || !read_memory(reinterpret_cast<void*>(value[1] + 12), &length, 4) ||
        length != size || !read_memory(reinterpret_cast<void*>(value[1] + 16), text, size + 1))
        return false;
    return !memcmp(text, expected, size + 1);
}

#if GC_PLAYER_MENU
// The Online menu. The stock shell asks ScriptCB_IsFileExist for custom_gc_0.lvl before it builds
// its menus, and the FindFirstFileA observer calls shell_probe from inside that call.
namespace {
#include "player_menu.inc"
#include "battle_exit.inc"
std::atomic<bool> menu_api_ready{false};
std::atomic<DWORD> game_thread{0};
}
// Installs player_menu.lua as a nested call on the global Lua state, on the game thread only.
void shell_probe() {
    static thread_local bool running = false;  // the script's own file checks re-enter here
    if (running) return;
    const DWORD thread = game_thread.load(std::memory_order_acquire);
    if (!menu_api_ready.load(std::memory_order_acquire) || (thread && thread != GetCurrentThreadId())) {
        log("SHELL_MENU skipped api_ready=%d game_thread=%lu this_thread=%lu",
            menu_api_ready.load(std::memory_order_acquire), thread, GetCurrentThreadId());
        return;
    }
    DWORD state = 0;
    if (!read_memory(image + 0x1a579a0, &state, 4) || !state ||
        !lua_string_argument(reinterpret_cast<void*>(state), 1, "custom_gc_0.lvl")) {
        log("SHELL_MENU REJECT the probe is not ScriptCB_IsFileExist(\"custom_gc_0.lvl\") on the global Lua state");
        return;
    }
    running = true;
    const auto r = run_nested_lua_query(reinterpret_cast<void*>(state), native_api, player_menu_query,
                                        sizeof(player_menu_query) - 1, 1);
    running = false;
    log("SHELL_MENU %s stage=%d called=%d load=%d call=%d before=%d after=%d restored=%d prefix_equal=%d value_ok=%d",
        r.stage == 3 && r.called && !r.load_status && !r.call_status && r.restored && r.prefix_equal && r.value_ok
            ? "installed"
            : "FAILED",
        r.stage, r.called, r.load_status, r.call_status, r.before, r.after, r.restored, r.prefix_equal, r.value_ok);
}

// Shared battles. The host's game hosts each Galactic Conquest battle as a LAN match tunnelled to
// its friends, who join it from the host's galaxy; afterwards both close the network game.
static int own_battle_team();
namespace {
battle::Tracker battles;  // as the host
battle::Guest guest;      // as a friend joining the host's battle
// Whether the engine code each feature relies on matched (arm_battles).
bool battle_state_ok = false;    // the engine state getter
bool battle_sockets_ok = false;  // the game's LAN sockets are wrapped
bool battle_join_ok = false;     // AutoNetJoin's session name buffer
bool battle_play_ok = false;     // the in-battle steps
bool battle_error_ok = false;    // the network error clear
bool battle_turret_ok = false;   // the defense grid turret pool
bool battle_waves_ok = false;    // the spawn manager's wave timers
bool battle_result_ok = false;   // a client's match end
bool battle_bonus_ok = false;    // the bonus card slots
bool battle_aim_ok = false;      // the targeting controller
bool battle_sealed_now = false, battle_busy_now = false;
unsigned battle_cleanup_frames = 0, guest_join_frames = 0, guest_cleanup_frames = 0, guest_lobby_frames = 0;
bool guest_lobby_failed = false;
// Steam thread counters already handled: the host leaving (friend's PC) and each friend's battle
// reports by session slot (host's PC).
unsigned guest_host_left_seen = 0;
unsigned host_friend_seen[battle::max_peers + 1]{};
// The battle running on this PC, as host or friend.
struct Play {
    unsigned number = 0, frames = 0, spawns = 0, turret_limit = 0, bonus_frames = 0, bonus_moves = 0;
    bool host = false, active = false, bonus_logged = false, bonus_failed = false;
    std::uint64_t started = 0;  // tick of the mission's first frame
    battle::SideChoice side;
    const char* button = "";  // the side select button picked
    // Spawn, wave, result and heroes bookkeeping, and each human slot's wait for BATTLE_SPAWN_WAIT.
    unsigned ai_spawns = 0, hero_changes = 0;
    bool waves_logged = false, result_sent = false, result_applied = false, result_seen = false, count_logged = false;
    bool victory_clear = false;  // the winner has read -1 during this battle
    int heroes = -1, characters = 0;
    std::uint64_t waiting_since[4]{};
    bool wait_logged[4]{};
    // Versus bots on the battle host (even_bots): the player's team, the ally skill offsets written
    // and the ones they replaced (difficulty 1 to 3), and the catch-up last logged with its range.
    int bots_team = 0, bots_offsets[4]{}, bots_was[4]{}, catchup[2]{}, catchup_low = 0, catchup_high = 0;
    unsigned catchup_changes = 0, bots_frames = 0;
    bool bots_failed = false, bots_rewritten = false;
    // This player's battle team once known, and the battle's Exit to Windows wrapper (wrap_exit).
    int own_team = 0;
    unsigned team_frames = 0, exit_frames = 0, exit_tries = 0;
    bool exit_wrapped = false;
    // Targeting controller relinks and logging (aim_step), and the army log (army_step).
    unsigned aim_relinks = 0, aim_lines = 0, aim_frames = 0, army_lines = 0;
    int aim_state = -1;
    std::uint64_t aim_next = 0, army_next = 0;
    // The character table log (table_step): lines written, humans last counted, next look.
    unsigned table_lines = 0, table_humans = 0;
    std::uint64_t table_next = 0;
} play;
// The side is only picked within this time of the mission's first frame, so a later team change
// stays the player's.
constexpr std::uint64_t side_window_ms = 60000;
// Closes the network game after a battle, as the stock Single Player button does.
constexpr char battle_cleanup_query[] =
    "assert(type(ScriptCB_CloseNetShell) == 'function' and type(ScriptCB_SetInNetGame) == 'function')\n"
    "if type(ScriptCB_CancelLogin) == 'function' then ScriptCB_CancelLogin() end\n"
    "ScriptCB_CloseNetShell(1)\n"
    "ScriptCB_SetInNetGame(nil)\n"
    "return 6217431\n";
// Whether the friend can join now: not while any popup but the script's own note is open.
constexpr char guest_can_join_query[] =
    "local ready = rawget(_G, '_GCT_battle_join_ready')\n"
    "assert(type(ready) == 'function' and type(rawget(_G, '_GCT_battle_join')) == 'function')\n"
    "if ready() then return 6217431 end\n"
    "return 0\n";
// Starts the friend's join, closing the script's own note; returns 0 if another popup is open.
constexpr char guest_join_query[] = "return function(data)\n"
                                    "  local join = rawget(_G, '_GCT_battle_join')\n"
                                    "  assert(type(join) == 'function')\n"
                                    "  if join(data) then return 6217431 end\n"
                                    "  return 0\n"
                                    "end\n";
// Leaves the battle as the pause menu's Quit does.
constexpr char guest_quit_query[] = "assert(type(ScriptCB_QuitToShell) == 'function')\n"
                                    "ScriptCB_QuitToShell()\n"
                                    "return 6217431\n";
// The stock lobby screen's per-frame update, run once the host's session is found. UpdateLobby
// closes any open popup first, so this cannot run from the joining popup itself.
constexpr char guest_lobby_query[] = "ScriptCB_UpdateLobby(nil)\n"
                                     "ScriptCB_LaunchLobby()\n"
                                     "return 6217431\n";
// Closes the network game a failed join left open, as the session list's cancel and then the Single
// Player button do. CancelJoin may wait up to 5 s for the disconnect.
constexpr char guest_close_query[] =
    "assert(type(ScriptCB_CloseNetShell) == 'function' and type(ScriptCB_SetInNetGame) == 'function')\n"
    "if type(ScriptCB_CancelJoin) == 'function' then ScriptCB_CancelJoin() end\n"
    "if type(ScriptCB_CancelSessionList) == 'function' then ScriptCB_CancelSessionList() end\n"
    "if type(ScriptCB_CancelLogin) == 'function' then ScriptCB_CancelLogin() end\n"
    "ScriptCB_CloseNetShell(1)\n"
    "ScriptCB_SetInNetGame(nil)\n"
    "return 6217431\n";
// A chunk that ran cleanly, returned 6217431 and restored the stack.
bool battle_lua_ok(const LuaQueryResult& r) {
    return r.stage == 3 && r.called && !r.load_status && !r.call_status && r.restored && r.prefix_equal && r.value_ok;
}
// A frame where Lua may run: called from the main loop, no state change or suspend pending, and the
// current Lua state (the shell's or a battle's) idle.
bool battle_frame(const FrameSample& s, DWORD& fresh) {
    unsigned char transition = 1, suspend = 1;
    Header h{};
    return s.readable && s.caller == reinterpret_cast<DWORD>(image) + 0x217a9c &&
           read_memory(image + 0x1a563b0, &transition, 1) && !transition &&
           read_memory(image + 0x1a56fa3, &suspend, 1) && !suspend && read_memory(image + 0x1a579a0, &fresh, 4) &&
           fresh == s.context && header(reinterpret_cast<void*>(fresh), h) && idle(h);
}
// The engine's current state hash, such as battle::state_shell, or 0 when the getter differs.
DWORD battle_state() {
    DWORD hash = 0;
    return battle_state_ok && read_memory(image + 0x1a563b8, &hash, 4) ? hash : 0;
}
// The engine's pending network error: a message key such as "ifs.mp.joinerrors.full" and its level.
// The key is "none" when there is none, and "unread" or "unreadable" when it cannot be read.
struct EngineErrorText {
    char key[48]{};
    int level = 0;
    bool kept = false;  // an error is pending
};
EngineErrorText engine_error_text() {
    EngineErrorText e;
    DWORD message = 0;
    if (!read_memory(image + 0x1a6504c, &message, 4) || !read_memory(image + 0x1a65050, &e.level, 4)) {
        memcpy(e.key, "unread", sizeof "unread");
        return e;
    }
    e.kept = message != 0;
    if (!message) {
        memcpy(e.key, "none", sizeof "none");
        return e;
    }
    if (!(read_memory(reinterpret_cast<void*>(message), e.key, sizeof e.key - 1) && !strncmp(e.key, "ifs.", 4)))
        memcpy(e.key, "unreadable", sizeof "unreadable");
    for (char& c : e.key)
        if (c && (c < 0x21 || c > 0x7e)) c = '?';
    return e;
}
// Logs why the friend's join failed, as the engine recorded it, with the join object's own failure
// flag (+0x1B0) and reason (+0x1B4) if it still exists.
void log_join_error(unsigned number) {
    DWORD join = 0;
    int reason = -1;
    unsigned char failed = 0;
    const EngineErrorText e = engine_error_text();
    if (read_memory(image + 0x1aaed38, &join, 4) && join) {
        read_memory(reinterpret_cast<void*>(join + 0x1b0), &failed, 1);
        read_memory(reinterpret_cast<void*>(join + 0x1b4), &reason, 4);
    }
    log("BATTLE_JOIN_ERROR number=%u message=%s level=%d join=%s failed=%u reason=%d", number, e.key, e.level,
        join ? "open" : "none", failed, reason);
}
// Writes game memory; fails instead of faulting on a bad address.
bool write_memory(void* address, const void* data, size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, data, size, &written) && written == size;
}
// Clears a pending network error as the error popup's OK would. Left set, it would pop up again in
// the next battle, and that popup's OK leaves the battle.
void clear_engine_error(unsigned number, const char* when) {
    if (!battle_error_ok) return;
    const EngineErrorText e = engine_error_text();
    if (!e.kept) return;
    const DWORD zero = 0;
    const bool cleared = write_memory(image + 0x1a6504c, &zero, 4) && write_memory(image + 0x1a65050, &zero, 4);
    log("BATTLE_ERROR_CLEARED number=%u when=%s message=%s level=%d cleared=%d", number, when, e.key, e.level, cleared);
}
// Calls the side select's Accept handler on the SpawnDisplay, guarded against faults.
bool accept_side(DWORD display) {
    using Accept = void(__fastcall*)(DWORD);
    __try {
        reinterpret_cast<Accept>(image + 0x1cbe10)(display);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Picks the galaxy side's team on the side select screen and accepts it, as the player would. The
// button comes from each team's faction and the side MetagameSetSide stored.
void choose_side(DWORD fresh) {
    DWORD display = 0;
    if (!read_memory(image + 0x1b9ae5c, &display, 4) || !display) return;
    int state = -1, local = -1;
    unsigned char active = 0, waiting = 1;
    DWORD character = 0;
    if (!read_memory(reinterpret_cast<void*>(display + 0x18), &state, 4) ||
        !read_memory(reinterpret_cast<void*>(display + 0x1fe8), &active, 1) ||
        !read_memory(reinterpret_cast<void*>(display + 0x1ff0), &waiting, 1) ||
        !read_memory(reinterpret_cast<void*>(display + 0x1fdc), &character, 4) ||
        !read_memory(reinterpret_cast<void*>(display + 0x2000), &local, 4))
        return;
    // The display's own conditions: active, not auto-assigning, has a character, in the side select.
    if (!active || waiting || !character || local < 0 || local > 3 || state != 0) return;
    ++play.side.tries;
    DWORD teams = 0, team1 = 0, team2 = 0;
    int faction1 = 0, faction2 = 0, side = 0;
    if (read_memory(image + 0x3e9aa0, &teams, 4) && teams &&
        read_memory(reinterpret_cast<void*>(teams + 4), &team1, 4) &&
        read_memory(reinterpret_cast<void*>(teams + 8), &team2, 4) && team1 && team2) {
        read_memory(reinterpret_cast<void*>(team1 + 0x64), &faction1, 4);
        read_memory(reinterpret_cast<void*>(team2 + 0x64), &faction2, 4);
    }
    read_memory(image + 0x1a56014 + 4 * local, &side, 4);
    play.button = battle::side_button(faction1, faction2, side);
    // Select that team's button on the side select screen.
    char source[200];
    _snprintf_s(source, sizeof source, _TRUNCATE,
                "local s = rawget(_G, 'ifs_sideselect%d')\n"
                "if type(s) ~= 'table' then return 0 end\n"
                "s.CurButton = '%s'\n"
                "return 6217431\n",
                local + 1, play.button);
    const LuaQueryResult r =
        run_source(reinterpret_cast<void*>(fresh), native_api, source, static_cast<unsigned>(strlen(source)));
    if (!battle_lua_ok(r)) {
        if (!play.side.due())
            log("BATTLE_SIDE number=%u choice=failed tries=%u (no side select table)", play.number, play.side.tries);
        return;
    }
    play.side.done = true;  // one accept per battle, whatever happens next
    if (!accept_side(display)) {
        log("BATTLE_SIDE number=%u choice=%s failed=1 (Accept faulted; the player picks the side)", play.number,
            play.button);
        return;
    }
    const int spawn_screen = 2;
    const bool moved = write_memory(reinterpret_cast<void*>(display + 0x18), &spawn_screen, 4);
    log("BATTLE_SIDE number=%u choice=%s team_factions=%d,%d side=%d state_set=%d", play.number, play.button, faction1,
        faction2, side, moved);
}
// Lets anyone who asked to spawn do so now rather than wait for their team's next wave, as in
// single-player. A human still waiting 2 s after asking is logged once (BATTLE_SPAWN_WAIT).
void spawn_now_step(std::uint64_t now) {
    static unsigned char table[battle::max_characters * 0x1b0];
    DWORD characters = 0, manager = 0;
    int count = 0;
    if (!read_memory(image + 0x1a30334, &characters, 4) || !read_memory(image + 0x1a30330, &count, 4) ||
        !read_memory(image + 0x1aafb34, &manager, 4) || !characters || !manager || count < 1)
        return;
    if (count > play.characters) play.characters = count;
    if (!battle::character_count_ok(count)) {
        if (!play.count_logged)
            log("BATTLE_SPAWN_SKIP number=%u characters=%d (more than %d)", play.number, count, battle::max_characters);
        play.count_logged = true;
        return;
    }
    if (!read_memory(reinterpret_cast<void*>(characters), table, static_cast<size_t>(count) * 0x1b0)) return;
    for (int slot = 0; slot < count; ++slot) {
        const DWORD character = characters + slot * 0x1b0;
        DWORD fields[16]{};  // +0x130 net slot (-1 for AI), +0x134 team, up to +0x16F
        memcpy(fields, table + slot * 0x1b0 + 0x130, sizeof fields);
        const int own_slot = static_cast<int>(fields[0]), team = static_cast<int>(fields[1]);
        const DWORD soldier = fields[6];                    // +0x148, set while alive
        const int own_wave = static_cast<int>(fields[11]);  // +0x15C, the wave it spawns with
        const bool requested = (fields[13] & 0xff) != 0;    // +0x164, asked to spawn
        const bool human = own_slot == slot;
        if ((!human && own_slot != -1) || team < 1 || team > 2) continue;
        int team_wave = -1;
        if (!read_memory(reinterpret_cast<void*>(manager + 0xbc + 4 * team), &team_wave, 4)) continue;
        if (battle::spawn_now(team_wave, own_wave, requested) &&
            write_memory(reinterpret_cast<void*>(character + 0x15c), &team_wave, 4)) {
            if (human ? !play.spawns++ : !play.ai_spawns++)
                log("BATTLE_SPAWN_NOW number=%u slot=%d team=%d wave=%d was=%d who=%s", play.number, slot, team,
                    team_wave, own_wave, human ? "human" : "ai");
            continue;
        }
        // Time each human's wait to spawn and log the first long one, with what the spawn gates read.
        if (!human || slot >= 4) continue;
        if (!requested || soldier) {
            play.waiting_since[slot] = 0;
            continue;
        }
        if (!play.waiting_since[slot]) {
            play.waiting_since[slot] = now;
            continue;
        }
        if (play.wait_logged[slot] || now - play.waiting_since[slot] < 2000) continue;
        play.wait_logged[slot] = true;
        float delay = -1, next = -1, window = -1, window_left = -1, own_timer = -1;
        int player_state = -1;
        unsigned char hero = 0;
        read_memory(reinterpret_cast<void*>(manager + 0x5c + 4 * team), &delay, 4);
        read_memory(reinterpret_cast<void*>(manager + 0x9c + 4 * team), &next, 4);
        read_memory(reinterpret_cast<void*>(manager + 0x7c + 4 * team), &window, 4);
        read_memory(reinterpret_cast<void*>(manager + 0xdc + 4 * team), &window_left, 4);
        read_memory(reinterpret_cast<void*>(character + 0x158), &own_timer, 4);
        read_memory(reinterpret_cast<void*>(character + 0x165), &hero, 1);
        read_memory(image + 0x1acdbfc + slot * 0x208, &player_state, 4);
        log("BATTLE_SPAWN_WAIT number=%u slot=%d team=%d own_wave=%d team_wave=%d delay=%.2f next=%.2f window=%.2f window_left=%.2f own_timer=%.2f hero=%u player_state=%d",
            play.number, slot, team, own_wave, team_wave, delay, next, window, window_left, own_timer, hero,
            player_state);
    }
}
// Host: caps both armies' wave delay (+0x5C), time to next wave (+0x9C), window (+0x7C) and window
// left (+0xDC) every frame, as SetSpawnDelay may raise them again at any time.
void quicken_waves() {
    if (!battle_waves_ok) return;
    DWORD manager = 0;
    if (!read_memory(image + 0x1aafb34, &manager, 4) || !manager) return;
    float was[2][4]{};
    bool changed = false;
    for (int team = 1; team <= 2; ++team) {
        static const unsigned at[4] = {0x5c, 0x9c, 0x7c, 0xdc};
        static const float limit[4] = {battle::quick_wave_s, battle::quick_wave_s, battle::quick_window_s,
                                       battle::quick_window_s};
        for (int i = 0; i < 4; ++i) {
            float value = 0;
            if (!read_memory(reinterpret_cast<void*>(manager + at[i] + 4 * team), &value, 4)) return;
            was[team - 1][i] = value;
            const float quick = battle::quicker(value, limit[i]);
            if (quick != value && write_memory(reinterpret_cast<void*>(manager + at[i] + 4 * team), &quick, 4))
                changed = true;
        }
    }
    if (!changed || play.waves_logged) return;
    play.waves_logged = true;
    log("BATTLE_WAVES number=%u delay=%.2f,%.2f window=%.2f,%.2f quick=%.2f,%.2f (waves every %.1f s)", play.number,
        was[0][0], was[1][0], was[0][2], was[1][2], battle::quick_wave_s, battle::quick_window_s, battle::quick_wave_s);
}
// Host: sends the friends the match result once a winner is recorded in this battle: the engine's
// winning-team mask, or a mask made from the winner when that is empty.
void send_result(unsigned number) {
    if (play.result_sent) return;
    int victory = -1;
    unsigned mask = 0;
    if (!read_memory(image + 0x3e6688, &victory, 4)) return;
    if (victory < 0) {
        play.victory_clear = true;
        return;
    }
    if (!play.victory_clear || !read_memory(image + 0x1aafca0, &mask, 4)) return;
    const unsigned sent = battle::host_mask(victory, mask);
    if (!sent) return;
    play.result_sent = true;
    battle_host_result(number, sent);
    log("BATTLE_RESULT number=%u victory=%d mask=%08X from=%s", number, victory, sent, mask ? "engine" : "victory");
}
// Friend: when the host's result has arrived but this game has not ended its match after a short
// wait, writes the host's winning-team mask so the engine's client path ends it.
void apply_result(unsigned number, std::uint64_t now) {
    if (!battle_result_ok || play.result_applied) return;
    int victory = -1;
    unsigned mask = 0;
    unsigned char ended = 1;
    if (!read_memory(image + 0x3e6688, &victory, 4) || !read_memory(image + 0x1aafca0, &mask, 4) ||
        !read_memory(image + 0x1a5600a, &ended, 1))
        return;
    if (victory < 0) play.victory_clear = true;
    const bool own = (play.victory_clear && victory >= 0) || mask != 0 || ended != 0;
    const bool waiting = guest.result_number == number && guest.result_mask && !guest.result_done;
    const unsigned due = guest.result_due(now, own);
    if (own && waiting && !play.result_seen) {
        play.result_seen = true;
        log("BATTLE_RESULT_SEEN number=%u victory=%d mask=%08X", number, victory, mask);
    }
    if (!due) return;
    play.result_applied = true;
    const bool written = write_memory(image + 0x1aafca0, &due, 4);
    log("BATTLE_RESULT_APPLIED number=%u mask=%08X written=%d (not received in %llu s)", number, due, written,
        battle::Guest::result_wait_ms / 1000);
}
// Logs this PC's heroes switch whenever it changes, at most 20 times a battle.
void watch_heroes(unsigned number, std::uint64_t now) {
    unsigned char heroes = 0;
    if (!read_memory(image + 0x1a62ee3, &heroes, 1) || heroes == play.heroes || play.hero_changes >= 20) return;
    ++play.hero_changes;
    log("BATTLE_HEROES_SWITCH number=%u heroes=%u was=%d after_ms=%llu", number, heroes, play.heroes,
        now - play.started);
    play.heroes = heroes;
}
// Friend: raises the defense grid turret pool's limit to the command post count, as the host's game
// does at mission start. With the limit at 0, a turret from the host crashes the client.
void make_turret_room(unsigned number) {
    if (!battle_turret_ok || play.turret_limit) return;
    DWORD posts_at = 0;
    int posts = 0;
    unsigned limit = 0;
    if (!read_memory(image + 0x1a30894, &posts_at, 4) || !posts_at ||
        !read_memory(reinterpret_cast<void*>(posts_at), &posts, 4) || !read_memory(image + 0x1abb974, &limit, 4))
        return;
    const unsigned wanted = battle::turret_limit(limit, posts);
    if (wanted == limit) return;  // no command posts yet, or enough room already
    if (!write_memory(image + 0x1abb974, &wanted, 4)) return;
    play.turret_limit = wanted;
    log("BATTLE_TURRETS number=%u limit=%u was=%u command_posts=%d", number, wanted, limit, posts);
}
// Friend: moves the host's bonus cards into the slots this game reads for each battle team, as the
// slot rule depends on a metagame flag only the host's game sets. Both PCs log their slots.
void keep_bonus_slots(unsigned number, bool host) {
    if (!battle_bonus_ok) return;
    DWORD teams = 0, team1 = 0, team2 = 0, manager = 0;
    int faction1 = 0, faction2 = 0;
    if (!read_memory(image + 0x3e9aa0, &teams, 4) || !teams ||
        !read_memory(reinterpret_cast<void*>(teams + 4), &team1, 4) ||
        !read_memory(reinterpret_cast<void*>(teams + 8), &team2, 4) || !team1 || !team2 ||
        !read_memory(reinterpret_cast<void*>(team1 + 0x64), &faction1, 4) ||
        !read_memory(reinterpret_cast<void*>(team2 + 0x64), &faction2, 4) || faction1 <= 0 || faction2 <= 0)
        return;
    // The manager is constructed once its vtable is in place.
    if (!read_memory(image + 0x1ab0630, &manager, 4) || manager != reinterpret_cast<DWORD>(image) + 0x397098) return;
    uint32_t slots[2]{}, want[2]{};
    unsigned char metagame = 1, heroes = 0;
    if (!read_memory(image + 0x1ab0648, slots, sizeof slots) || !read_memory(image + 0x1a56033, &metagame, 1)) return;
    read_memory(image + 0x1a62ee3, &heroes, 1);
    want[0] = slots[0];
    want[1] = slots[1];
    std::string names[2];
    if (!host && battle::parse_bonus(guest.info.bonus, names) && !guest.info.bonus.empty()) {
        const uint32_t galaxy[2] = {battle::bonus_hash(names[0]), battle::bonus_hash(names[1])};
        battle::friend_bonus_slots(galaxy, faction1, faction2, metagame != 0, want);
    }
    const bool move = want[0] != slots[0] || want[1] != slots[1];
    const bool moved = move && write_memory(image + 0x1ab0648, want, sizeof want);
    if (moved) ++play.bonus_moves;
    if (move && !moved) {  // a failed write is logged once
        if (play.bonus_failed) return;
        play.bonus_failed = true;
    } else if (play.bonus_logged && !move) {
        return;
    }
    play.bonus_logged = true;
    const bool cards = !host && !guest.info.bonus.empty();
    log("BATTLE_BONUS number=%u team_factions=%d,%d metagame=%u slots=%08X,%08X was=%08X,%08X moved=%s heroes=%u card=%s (%s)",
        number, faction1, faction2, metagame, want[0], want[1], slots[0], slots[1],
        !move   ? "no"
        : moved ? "yes"
                : "failed",
        heroes,
        host    ? "-"
        : cards ? guest.info.bonus.c_str()
                : "none",
        host     ? "the host's own cards"
        : !cards ? "no cards announced"
        : move   ? "the host's cards, put where this game reads them"
                 : "the host's cards, already where this game reads them");
}
// Versus bots. In a network game the AI has no player's team, so every bot gets the enemy's skill and
// the catch-up never runs. The battle host's game answers with its own team instead.
bool player_team_hooked = false;
bool battle_ai_ok = false;  // the team lookup is hooked and its users verified
// Maps an address in the executable's preferred 0x400000 image to this process.
DWORD game_address(DWORD va) { return reinterpret_cast<DWORD>(image) - 0x400000 + va; }
// The ally skill offsets that give allies the enemy's skill, for difficulties 1 to 3.
bool even_offsets(int offsets[4], int ally[4], int enemy[4]) {
    int enemy_offsets[4]{};
    if (!read_memory(reinterpret_cast<void*>(game_address(0x0078879c)), ally, 4 * sizeof(int)) ||
        !read_memory(reinterpret_cast<void*>(game_address(0x007887a8)), enemy, 4 * sizeof(int)) ||
        !read_memory(reinterpret_cast<void*>(game_address(0x01e2fd78)), enemy_offsets, sizeof enemy_offsets))
        return false;
    for (int d = 1; d <= 3; ++d) offsets[d] = battle::even_ally_offset(enemy_offsets[d], enemy[d], ally[d]);
    return true;
}
// Writes the ally offsets for difficulties 1 to 3.
bool write_ally_offsets(const int offsets[4]) {
    return write_memory(reinterpret_cast<void*>(game_address(0x01e2fd8c) + 4), offsets + 1, 3 * sizeof(int));
}
// A team's reinforcement count, or -1 when unreadable.
int team_reinforcements(int team) {
    DWORD teams = 0, object = 0;
    int count = -1;
    if (read_memory(image + 0x3e9aa0, &teams, 4) && teams &&
        read_memory(reinterpret_cast<void*>(teams + 4 * team), &object, 4) && object)
        read_memory(reinterpret_cast<void*>(object + 0x28), &count, 4);
    return count;
}
// Versus, battle host, each mission frame: makes this team the player's, evens the ally skill
// offsets with the enemy's, and logs the catch-up as it changes.
void even_bots(unsigned number) {
    if (!battle_ai_ok || play.bots_failed) return;
    int ally[4]{}, enemy[4]{};
    // First frame: take this team as the player's and even the ally offsets.
    if (!play.bots_team) {
        const int team = battle::ai_player_team(battles.versus, true, own_battle_team());
        if (!team) return;
        if (!read_memory(reinterpret_cast<void*>(game_address(0x01e2fd8c)), play.bots_was, sizeof play.bots_was) ||
            !even_offsets(play.bots_offsets, ally, enemy) || !write_ally_offsets(play.bots_offsets)) {
            play.bots_failed = true;
            log("BATTLE_BOTS number=%u team=%d failed=1 (cannot access the skill offsets)", number, team);
            return;
        }
        play.bots_team = team;
        bots::chosen.store(team, std::memory_order_relaxed);
        int difficulty = 0, network = 0;
        read_memory(reinterpret_cast<void*>(game_address(0x01e2fda4)), &difficulty, 4);
        read_memory(reinterpret_cast<void*>(game_address(0x01ef7fb0)), &network, 4);
        log("BATTLE_BOTS number=%u team=%d difficulty=%d network_difficulty=%d allies=%d,%d,%d enemy=%d,%d,%d offsets=%d,%d,%d was=%d,%d,%d",
            number, team, difficulty, network, ally[1], ally[2], ally[3], enemy[1], enemy[2], enemy[3],
            play.bots_offsets[1], play.bots_offsets[2], play.bots_offsets[3], play.bots_was[1], play.bots_was[2],
            play.bots_was[3]);
        return;
    }
    // The mission's SetAIDifficulty may set the offsets again; even them out every 15th frame.
    if (!(++play.bots_frames % 15)) {
        int now[4]{}, want[4]{};
        if (read_memory(reinterpret_cast<void*>(game_address(0x01e2fd8c)), now, sizeof now) &&
            even_offsets(want, ally, enemy) && memcmp(now + 1, want + 1, 3 * sizeof(int)) && write_ally_offsets(want)) {
            memcpy(play.bots_offsets, want, sizeof want);
            if (!play.bots_rewritten)
                log("BATTLE_BOTS_EVEN number=%u offsets=%d,%d,%d was=%d,%d,%d", number, want[1], want[2], want[3],
                    now[1], now[2], now[3]);
            play.bots_rewritten = true;
        }
    }
    // The engine's catch-up for this team and the other, logged when it changes, at most 40 times.
    int catchup[2]{};
    if (!read_memory(reinterpret_cast<void*>(game_address(0x01e2fda8)), &catchup[0], 4) ||
        !read_memory(reinterpret_cast<void*>(game_address(0x01e2fda0)), &catchup[1], 4) ||
        (catchup[0] == play.catchup[0] && catchup[1] == play.catchup[1]))
        return;
    play.catchup[0] = catchup[0];
    play.catchup[1] = catchup[1];
    if (catchup[0] < play.catchup_low) play.catchup_low = catchup[0];
    if (catchup[0] > play.catchup_high) play.catchup_high = catchup[0];
    if (++play.catchup_changes > 40) return;
    log("BATTLE_BOTS_CATCHUP number=%u team=%+d other=%+d reinforcements=%d,%d", number, catchup[0], catchup[1],
        team_reinforcements(play.bots_team), team_reinforcements(3 - play.bots_team));
}
// Restores the game's own team rule, and the ally offsets if they are still the ones written.
void stop_even_bots() {
    if (!play.bots_team) return;
    bots::chosen.store(0, std::memory_order_relaxed);
    int now[4]{};
    if (read_memory(reinterpret_cast<void*>(game_address(0x01e2fd8c)), now, sizeof now) &&
        !memcmp(now + 1, play.bots_offsets + 1, 3 * sizeof(int)))
        write_ally_offsets(play.bots_was);
    play.bots_team = 0;
}
// Wraps the battle's ScriptCB_QuitToWindows (battle_exit.lua) so an Exit to Windows says goodbye
// first. Tried on every 15th safe frame a few times; a stack not restored ends the tries.
void wrap_exit(const FrameSample& s) {
    if (play.exit_wrapped || play.exit_tries >= battle::SideChoice::max_tries || (play.exit_frames++ % 15)) return;
    DWORD fresh = 0;
    if (!battle_frame(s, fresh)) return;
    ++play.exit_tries;
    const LuaQueryResult r =
        run_source(reinterpret_cast<void*>(fresh), native_api, battle_exit_query, sizeof(battle_exit_query) - 1);
    play.exit_wrapped = battle_lua_ok(r);
    const bool stack_ok = r.restored && r.prefix_equal;
    if (!stack_ok) play.exit_tries = battle::SideChoice::max_tries;
    if (play.exit_wrapped || play.exit_tries >= battle::SideChoice::max_tries)
        log("BATTLE_EXIT_WRAPPED number=%u wrapped=%d tries=%u stack_ok=%d (%s) error=%s", play.number,
            play.exit_wrapped, play.exit_tries, stack_ok,
            play.exit_wrapped ? "an Exit to Windows from this battle says goodbye first"
                              : "this battle's Exit closes the game without a goodbye",
            lua_error(r));
}
// Targeting on network clients. The targeting controller takes the player's character only as the
// mission starts; a client's comes later, leaving it with no enemy markers or lock-on.
struct AimView {
    int slot = -1, team = 0, pilot = -2;
    DWORD controller = 0, linked = 0, expected = 0, wanted = 0, active = 0, teams = 0, lock = 0;
    unsigned char flags = 0;
    unsigned candidates = 0;
    battle::Seat seat = battle::Seat::none;
};
// Reads the local player's controller, the character it should hold, and that character's seat.
AimView aim_view() {
    AimView v;
    if (!read_memory(image + 0x1ba3d58, &v.slot, 4) || !read_memory(image + 0x1a57400, &v.controller, 4) ||
        !v.controller)
        return v;
    DWORD table = 0;
    int count = 0, entry_slot = -2;
    if (v.slot >= 0 && read_memory(image + 0x1a30334, &table, 4) && table &&
        read_memory(image + 0x1a30330, &count, 4) && v.slot < count) {
        const DWORD entry = table + static_cast<DWORD>(v.slot) * 0x1b0;
        if (read_memory(reinterpret_cast<void*>(entry + 0x130), &entry_slot, 4) &&
            battle::character_entry_ok(v.slot, count, entry_slot))
            v.expected = entry;
    }
    read_memory(reinterpret_cast<void*>(v.controller + 4), &v.linked, 4);
    // Targeting state: requested and active mode (+0x920, +0x918), target teams (+0xF70), flags
    // (+0xF74), locked target (+0x928), and 64 HUD marker candidates of 0x1C bytes at +8.
    read_memory(reinterpret_cast<void*>(v.controller + 0x920), &v.wanted, 4);
    read_memory(reinterpret_cast<void*>(v.controller + 0x918), &v.active, 4);
    read_memory(reinterpret_cast<void*>(v.controller + 0xf70), &v.teams, 4);
    read_memory(reinterpret_cast<void*>(v.controller + 0xf74), &v.flags, 1);
    read_memory(reinterpret_cast<void*>(v.controller + 0x928), &v.lock, 4);
    static DWORD candidates[0x40 * 7];
    if (read_memory(reinterpret_cast<void*>(v.controller + 8), candidates, sizeof candidates))
        for (unsigned i = 0; i < 0x40; ++i)
            if (candidates[i * 7]) ++v.candidates;
    const DWORD character = v.linked ? v.linked : v.expected;
    DWORD held[3]{};  // +0x148 soldier, +0x14C driven vehicle, +0x150 vehicle seat
    if (character && read_memory(reinterpret_cast<void*>(character + 0x148), held, sizeof held)) {
        v.seat = battle::seat_of(held[0], held[1], held[2]);
        // The net slot of the object's controller (+0xD4); targeting arms only for a local slot.
        const DWORD object = held[2] ? held[2] : held[1] ? held[1] : held[0];
        if (object) read_memory(reinterpret_cast<void*>(object + 0xd4), &v.pilot, 4);
    }
    if (character) read_memory(reinterpret_cast<void*>(character + 0x134), &v.team, 4);
    return v;
}
// Every 15th frame: links the controller to the player's character once it exists, and logs its
// state when it changes or every 30 s, at most 24 lines a battle.
void aim_step(const FrameSample& s, std::uint64_t now) {
    if (!battle_aim_ok || (play.aim_frames++ % 15)) return;
    AimView v = aim_view();
    if (!v.controller) return;
    if (battle::aim_relink_due(v.controller, v.linked, v.expected)) {
        DWORD fresh = 0;
        if (battle_frame(s, fresh) && write_memory(reinterpret_cast<void*>(v.controller + 4), &v.expected, 4)) {
            ++play.aim_relinks;
            log("BATTLE_AIM_RELINKED number=%u slot=%d was=%s after_ms=%llu (had %s)", play.number, v.slot,
                v.linked ? "other" : "none", now - play.started, v.linked ? "another character" : "no character");
            v.linked = v.expected;
        }
    }
    // 0: no character for the player yet, 1: the controller holds it, 2: it does not.
    const int state = !v.expected ? 0 : v.linked == v.expected ? 1 : 2;
    if (play.aim_lines >= 24 || (state == play.aim_state && now < play.aim_next)) return;
    play.aim_state = state;
    play.aim_next = now + 30000;
    ++play.aim_lines;
    log("BATTLE_AIM number=%u slot=%d character=%s team=%d seat=%s pilot_slot=%d mode=%lu/%lu teams=%08lX flags=%02X candidates=%u lock=%d relinks=%u after_ms=%llu",
        play.number, v.slot,
        state == 1   ? "linked"
        : state == 2 ? "unlinked"
                     : "not_yet",
        v.team, battle::seat_name(v.seat), v.pilot, v.wanted, v.active, v.teams, v.flags, v.candidates, v.lock != 0,
        play.aim_relinks, now - play.started);
}
const char* aim_name(int state) {
    return state == 1 ? "linked" : state == 2 ? "unlinked" : state == 0 ? "not_yet" : "unread";
}
// Logs every 20 s, at most 40 lines a battle, where each army's AI is: in a vehicle, in a seat, on
// foot or waiting to spawn. An entry whose own net slot is -1 is AI.
void army_step(std::uint64_t now) {
    if (now < play.army_next || play.army_lines >= 40) return;
    play.army_next = now + 20000;
    static unsigned char table[battle::max_characters * 0x1b0];
    DWORD characters = 0;
    int count = 0;
    if (!read_memory(image + 0x1a30334, &characters, 4) || !read_memory(image + 0x1a30330, &count, 4) || !characters ||
        !battle::character_count_ok(count) ||
        !read_memory(reinterpret_cast<void*>(characters), table, static_cast<size_t>(count) * 0x1b0))
        return;
    battle::Army armies[2];
    for (int slot = 0; slot < count; ++slot) {
        DWORD f[9]{};  // +0x130 net slot, +0x134 team, +0x148 soldier, +0x14C vehicle, +0x150 seat
        memcpy(f, table + slot * 0x1b0 + 0x130, sizeof f);
        const int own = static_cast<int>(f[0]), team = static_cast<int>(f[1]);
        if (team < 1 || team > 2 || (own != -1 && own != slot)) continue;
        armies[team - 1].add(own == slot, battle::seat_of(f[6], f[7], f[8]));
    }
    ++play.army_lines;
    log("BATTLE_AI number=%u after_s=%llu team1=%u:%u/%u/%u/%u+%u team2=%u:%u/%u/%u/%u+%u (ai:vehicle/seat/foot/waiting+humans)",
        play.number, (now - play.started) / 1000, armies[0].ai, armies[0].vehicle, armies[0].seat, armies[0].soldier,
        armies[0].waiting, armies[0].humans, armies[1].ai, armies[1].vehicle, armies[1].seat, armies[1].soldier,
        armies[1].waiting, armies[1].humans);
}
// Logs each character's team and whether a player holds it, 2 s into the mission and when more
// humans appear (at most once a second, 8 lines a battle), so the PCs' logs can be compared.
void table_step(std::uint64_t now) {
    if (play.table_lines >= 8 || now - play.started < 2000 || now < play.table_next) return;
    play.table_next = now + 1000;
    static unsigned char table[battle::max_characters * 0x1b0];
    DWORD characters = 0;
    int count = 0;
    if (!read_memory(image + 0x1a30334, &characters, 4) || !read_memory(image + 0x1a30330, &count, 4) || !characters ||
        !battle::character_count_ok(count) ||
        !read_memory(reinterpret_cast<void*>(characters), table, static_cast<size_t>(count) * 0x1b0))
        return;
    battle::Layout layout;
    for (int slot = 0; slot < count; ++slot) {
        int f[2]{};  // +0x130 net slot, +0x134 team
        memcpy(f, table + slot * 0x1b0 + 0x130, sizeof f);
        layout.add(slot, f[0], f[1]);
    }
    if (play.table_lines && layout.humans <= play.table_humans) return;
    play.table_humans = layout.humans;
    ++play.table_lines;
    int players = 0;
    read_memory(image + 0x3e825c, &players, 4);
    log("BATTLE_TABLE number=%u host=%d after_ms=%llu count=%d players=%d humans=%u team1=%u team2=%u layout=%s",
        play.number, play.host, now - play.started, count, players, layout.humans, layout.team1, layout.team2,
        layout.letters.c_str());
}
// Each frame of a shared battle on this PC; the in-battle steps run during the mission only.
void step_battle_play(const FrameSample& s, DWORD hash, unsigned number, bool host) {
    if (!play.active || play.number != number || play.host != host) {
        stop_even_bots();
        play = Play{};
        play.number = number;
        play.host = host;
        play.active = true;
    }
    if (hash != battle::state_mission) return;
    if (!host) make_turret_room(number);  // must precede this frame's object update
    const std::uint64_t now = GetTickCount64();
    if (!play.started) play.started = now;
    watch_heroes(number, now);
    if (now - play.started < side_window_ms && !(play.bonus_frames++ % 15)) keep_bonus_slots(number, host);
    if (host) send_result(number);
    else apply_result(number, now);
    wrap_exit(s);
    // This player's battle team, for the goodbye message and a kept result.
    if (!play.own_team && !(play.team_frames++ % 15)) play.own_team = own_battle_team();
    if (!battle_play_ok) return;
    if (host) quicken_waves();
    if (host && battles.versus) even_bots(number);
    spawn_now_step(now);
    aim_step(s, now);
    army_step(now);
    table_step(now);
    DWORD fresh = 0;
    if (play.side.due() && now - play.started < side_window_ms && !(++play.frames % 15) && battle_frame(s, fresh))
        choose_side(fresh);
}
// The battle is over on this PC: log its summary and undo the bot changes.
void end_battle_play() {
    if (!play.active) return;
    log("BATTLE_PLAY number=%u side=%s immediate_spawns=%u turret_room=%u bonus_moves=%u ai_spawns=%u characters=%d quick_waves=%d result=%s bots=%s catchup=%d..%d catchup_changes=%u own_team=%d exit_wrapped=%d aim=%s aim_relinks=%u",
        play.number, play.side.done ? (*play.button ? play.button : "none") : "not chosen", play.spawns,
        play.turret_limit, play.bonus_moves, play.ai_spawns, play.characters, play.waves_logged,
        play.host             ? (play.result_sent ? "sent" : "none")
        : play.result_applied ? "applied"
        : play.result_seen    ? "own"
                              : "none",
        play.bots_team     ? "even"
        : play.bots_failed ? "failed"
                           : "network",
        play.catchup_low, play.catchup_high, play.catchup_changes, play.own_team, play.exit_wrapped,
        aim_name(play.aim_state), play.aim_relinks);
    stop_even_bots();
    play.active = false;
}
}
// Sets the session name AutoNetJoin joins; otherwise only the gamename switch sets it.
static bool write_join_name(const std::string& token) {
    char name[64]{};
    if (!battle_join_ok || token.size() >= sizeof name) return false;
    memcpy(name, token.data(), token.size());
    SIZE_T written = 0;
    char check[64]{};
    return WriteProcessMemory(GetCurrentProcess(), image + 0x3e8e98, name, sizeof name, &written) &&
           written == sizeof name && read_memory(image + 0x3e8e98, check, sizeof check) &&
           !memcmp(check, name, sizeof name);
}
// Verifies the engine code each battle feature relies on, with this image's addresses in place, and
// hooks the AI's player-team lookup. A feature whose code differs stays off.
static void arm_battles(HMODULE game) {
    // The state getter: mov eax,[state]; ret.
    unsigned char expected[6] = {0xa1, 0, 0, 0, 0, 0xc3}, actual[6]{};
    const DWORD state = reinterpret_cast<DWORD>(image) + 0x1a563b8;
    memcpy(expected + 1, &state, 4);
    battle_state_ok = read_memory(image + 0x13ae80, actual, sizeof actual) && !memcmp(actual, expected, sizeof actual);
    // AutoNetJoin's name getter, which returns the buffer at +0x3E8E98.
    unsigned char name_expected[10] = {0x55, 0x8b, 0xec, 0xb8, 0, 0, 0, 0, 0x5d, 0xc3}, name_actual[10]{};
    const DWORD name = reinterpret_cast<DWORD>(image) + 0x3e8e98;
    memcpy(name_expected + 4, &name, 4);
    battle_join_ok = read_memory(image + 0x1db350, name_actual, sizeof name_actual) &&
                     !memcmp(name_actual, name_expected, sizeof name_actual);
    battle_sockets_ok = install_battle_sockets(game);
    // The in-battle steps' code; at+X relocates a preferred-base address X to this image.
    const DWORD at = reinterpret_cast<DWORD>(image) - 0x400000;
    const DWORD characters = at + 0x01e30334, count = at + 0x01e30330, manager = at + 0x01eafb34,
                display = at + 0x01f9ae5c;
    unsigned char accessor[41] = {0x8b, 0x15, 0,    0,    0,    0,    0x85, 0xd2, 0x74, 0x1c, 0x85, 0xc9, 0x78, 0x18,
                                  0x3b, 0x0d, 0,    0,    0,    0,    0x7d, 0x10, 0x69, 0xc1, 0xb0, 0x01, 0,    0,
                                  0x03, 0xc2, 0x39, 0x88, 0x30, 0x01, 0,    0,    0x74, 0x02, 0x33, 0xc0, 0xc3};
    memcpy(accessor + 2, &characters, 4);
    memcpy(accessor + 16, &count, 4);
    unsigned char getter[6] = {0xa1, 0, 0, 0, 0, 0xc3};
    memcpy(getter + 1, &display, 4);
    unsigned char assign[19] = {0xa1, 0, 0, 0, 0, 0x8b, 0x84, 0x88, 0xbc, 0, 0, 0, 0x40, 0x89, 0x87, 0x5c, 0x01, 0, 0};
    memcpy(assign + 1, &manager, 4);
    unsigned char gate[34] = {0x8b, 0x35, 0,    0,    0, 0, 0x89, 0x4c, 0x24, 0x24, 0x8b, 0x84, 0x8e, 0xbc, 0, 0, 0,
                              0x3b, 0x87, 0x5c, 0x01, 0, 0, 0x7c, 0x89, 0xf3, 0x0f, 0x10, 0x84, 0x8e, 0xdc, 0, 0, 0};
    memcpy(gate + 2, &manager, 4);
    static const unsigned char accept[17] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0x89, 0x4d, 0xfc,
                                             0x8b, 0x4d, 0xfc, 0xe8, 0xdf, 0xfd, 0xff, 0xff};
    static const unsigned char faction[4] = {0x8b, 0x41, 0x64, 0xc3};
    // Each entry: an offset in the image and the bytes expected there.
    struct Code {
        unsigned rva;
        const unsigned char* bytes;
        size_t size;
    } code[] = {
        {0x029470, accessor, sizeof accessor},  // a human's character by net slot
        {0x029a20, getter, sizeof getter},      // the SpawnDisplay getter
        {0x050f99, assign, sizeof assign},      // spawn request: own wave = team wave + 1
        {0x051045, gate, sizeof gate},          // spawn waits while team wave < own wave
        {0x1cbe10, accept, sizeof accept},      // the side select's Accept
        {0x029340, faction, sizeof faction},    // a team's faction, +0x64
    };
    battle_play_ok = true;
    for (const Code& c : code) {
        unsigned char here[48]{};
        battle_play_ok = battle_play_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    // The engine's own network error clear, which clear_engine_error repeats.
    unsigned char clear[21] = {0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0xc3},
                  clear_here[21]{};
    const DWORD error_key = at + 0x01e6504c, error_level = at + 0x01e65050;
    memcpy(clear + 2, &error_key, 4);
    memcpy(clear + 12, &error_level, 4);
    battle_error_ok =
        read_memory(image + 0x1d6710, clear_here, sizeof clear_here) && !memcmp(clear_here, clear, sizeof clear);
    // The turret pool (make_turret_room): the create's limit check and allocation, the bonus manager
    // raising the limit to the command post count, and the pool's name and object size.
    const DWORD pool = at + 0x01ebb940, pool_count = pool + 0x3c, pool_limit = pool + 0x34, posts = at + 0x01e30894;
    unsigned char create[15] = {0xa1, 0, 0, 0, 0, 0x56, 0x8b, 0xf1, 0x3b, 0x05, 0, 0, 0, 0, 0x72};
    memcpy(create + 1, &pool_count, 4);
    memcpy(create + 10, &pool_limit, 4);
    unsigned char take[10] = {0x68, 0xc0, 0x06, 0x00, 0x00, 0xb9, 0, 0, 0, 0};
    memcpy(take + 6, &pool, 4);
    unsigned char raise[19] = {0x39, 0x05, 0, 0, 0,    0,    0x73, 0x0b, 0x50, 0xb9,
                               0,    0,    0, 0, 0xe8, 0x60, 0xfc, 0x28, 0x00};
    memcpy(raise + 2, &pool_limit, 4);
    memcpy(raise + 10, &pool, 4);
    unsigned char count_posts[8] = {0xa1, 0, 0, 0, 0, 0x8b, 0x00, 0xc3};
    memcpy(count_posts + 1, &posts, 4);
    const Code turret_code[] = {
        {0x097d19, create, sizeof create},            // turret create: refuse at the limit
        {0x097d3c, take, sizeof take},                // turret create: take one from the pool
        {0x04ca9d, raise, sizeof raise},              // bonus manager start: raise the limit
        {0x07ab70, count_posts, sizeof count_posts},  // the command post count
    };
    battle_turret_ok = true;
    for (const Code& c : turret_code) {
        unsigned char here[24]{};
        battle_turret_ok =
            battle_turret_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    char pool_name[32]{};
    DWORD object_size = 0;
    battle_turret_ok = battle_turret_ok && read_memory(image + 0x1abb950, pool_name, sizeof pool_name - 1) &&
                       !strcmp(pool_name, "EntityDefenseGridTurret") &&
                       read_memory(image + 0x1abb970, &object_size, 4) && object_size == 0x6c0;
    // The bonus card slots (keep_bonus_slots): the slot rule, the slot check, ActivateBonus filling a
    // slot on the manager, and the mission reset clearing both.
    const DWORD metagame_flag = at + 0x01e56033, team_list = at + 0x007e9aa0, bonus_manager = at + 0x01eb0630,
                bonus_slots = at + 0x01eb0648;
    unsigned char slot_rule[63] = {0x55, 0x8b, 0xec, 0xe8, 0x28, 0xe7, 0x0e, 0x00, 0x3d, 0x39, 0x03, 0xf6, 0x8f,
                                   0x75, 0x28, 0x80, 0x3d, 0,    0,    0,    0,    0x00, 0x74, 0x1f, 0xa1, 0,
                                   0,    0,    0,    0x8b, 0x48, 0x08, 0x8b, 0x40, 0x04, 0x8b, 0x40, 0x64, 0x3b,
                                   0x41, 0x64, 0x7e, 0x0c, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x2b, 0x45, 0x08, 0x5d,
                                   0xc2, 0x04, 0x00, 0x8b, 0x45, 0x08, 0x48, 0x5d, 0xc2, 0x04, 0x00};
    memcpy(slot_rule + 17, &metagame_flag, 4);
    memcpy(slot_rule + 25, &team_list, 4);
    static const unsigned char slot_check[38] = {0x55, 0x8b, 0xec, 0x8b, 0x55, 0x08, 0x56, 0x8b, 0xf1, 0x8d,
                                                 0x42, 0xff, 0x83, 0xf8, 0x01, 0x77, 0x15, 0x52, 0xe8, 0x59,
                                                 0xff, 0xff, 0xff, 0x8b, 0x44, 0x86, 0x18, 0x3b, 0x45, 0x0c,
                                                 0x5e, 0x0f, 0x94, 0xc0, 0x5d, 0xc2, 0x08, 0x00};
    unsigned char activate[13] = {0xb9, 0, 0, 0, 0, 0xff, 0x30, 0x56, 0xe8, 0x8a, 0x02, 0xec, 0xff};
    memcpy(activate + 1, &bonus_manager, 4);
    unsigned char reset[8] = {0x66, 0x0f, 0xd6, 0x05, 0, 0, 0, 0};
    memcpy(reset + 4, &bonus_slots, 4);
    const Code bonus_code[] = {
        {0x04c750, slot_rule, sizeof slot_rule},    // which slot holds a team's card
        {0x04c7e0, slot_check, sizeof slot_check},  // whether a team has a card, slots at +0x18
        {0x18c4f9, activate, sizeof activate},      // ActivateBonus filling a slot
        {0x04c72f, reset, sizeof reset},            // mission reset: both slots cleared
    };
    battle_bonus_ok = true;
    for (const Code& c : bonus_code) {
        unsigned char here[64]{};
        battle_bonus_ok = battle_bonus_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    // The targeting controller (aim_step): its constructor taking the slot's character and storing
    // itself per player, the local slot getter, its size, and its update reading the character.
    const DWORD local_slots = at + 0x01fa3d58, controllers = at + 0x01e57400;
    unsigned char take_character[53] = {
        0xe8, 0xcb, 0xc5, 0x03, 0x00, 0x8b, 0x15, 0,    0,    0,    0,    0x83, 0xc4, 0x04, 0x8b, 0xc8, 0x85, 0xd2,
        0x74, 0x1c, 0x85, 0xc9, 0x78, 0x18, 0x3b, 0x0d, 0,    0,    0,    0,    0x7d, 0x10, 0x69, 0xc1, 0xb0, 0x01,
        0x00, 0x00, 0x03, 0xc2, 0x39, 0x88, 0x30, 0x01, 0x00, 0x00, 0x74, 0x02, 0x33, 0xc0, 0x89, 0x47, 0x04};
    memcpy(take_character + 7, &characters, 4);
    memcpy(take_character + 26, &count, 4);
    unsigned char keep_controller[9] = {0x8b, 0x07, 0x89, 0x3c, 0x85, 0, 0, 0, 0};
    memcpy(keep_controller + 5, &controllers, 4);
    unsigned char local_slot[8] = {0x8b, 0x04, 0x8d, 0, 0, 0, 0, 0xc3};
    memcpy(local_slot + 3, &local_slots, 4);
    static const unsigned char make_controller[10] = {0x68, 0x78, 0x0f, 0x00, 0x00, 0xe8, 0x06, 0x5c, 0x14, 0x00};
    static const unsigned char read_character[13] = {0x8b, 0x47, 0x04, 0x85, 0xc0, 0x74, 0x08,
                                                     0x8b, 0x88, 0x34, 0x01, 0x00, 0x00};
    const Code aim_code[] = {
        {0x17ade0, take_character, sizeof take_character},    // constructor: the slot's character at +4
        {0x17af82, keep_controller, sizeof keep_controller},  // constructor: kept per local player
        {0x1b26e0, local_slot, sizeof local_slot},            // a local player's net slot
        {0x17d930, make_controller, sizeof make_controller},  // allocated at 0xF78 bytes
        {0x17b07d, read_character, sizeof read_character},    // update: the character, then its team
    };
    battle_aim_ok = true;
    for (const Code& c : aim_code) {
        unsigned char here[64]{};
        battle_aim_ok = battle_aim_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    // The spawn waves (quicken_waves): SetSpawnDelay's stores, and the spawn manager's update counting
    // down each team's next wave and restarting it from the delay.
    static const unsigned char set_delay[12] = {0xf3, 0x0f, 0x11, 0x4c, 0x81, 0x5c, 0xf3, 0x0f, 0x11, 0x54, 0x81, 0x7c};
    static const unsigned char wave_timers[6] = {0x8d, 0x8e, 0x9c, 0x00, 0x00, 0x00};
    static const unsigned char wave_loop[34] = {0xf3, 0x0f, 0x10, 0x09, 0xf3, 0x0f, 0x5c, 0xca, 0x0f, 0x2f, 0xe1, 0xf3,
                                                0x0f, 0x11, 0x09, 0x72, 0x34, 0x84, 0xdb, 0x75, 0x1f, 0xf3, 0x0f, 0x10,
                                                0x41, 0xc0, 0xf3, 0x0f, 0x58, 0xc1, 0xf3, 0x0f, 0x11, 0x01};
    const Code wave_code[] = {
        {0x24e29b, set_delay, sizeof set_delay},      // SetSpawnDelay: delay +0x5C, window +0x7C
        {0x24eaec, wave_timers, sizeof wave_timers},  // update: each team's next wave, +0x9C
        {0x24eb10, wave_loop, sizeof wave_loop},      // counts down, then restarts from the delay
    };
    battle_waves_ok = true;
    for (const Code& c : wave_code) {
        unsigned char here[48]{};
        battle_waves_ok = battle_waves_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    // A client's match end (apply_result): with no result yet, a winning-team mask ends the match.
    const DWORD match_over = at + 0x01e5600a, winners = at + 0x01eafca0;
    unsigned char client_end[25] = {0x80, 0x3d, 0,    0,    0,    0,    0x00, 0x75, 0x15, 0x83, 0x3d, 0,   0,
                                    0,    0,    0x00, 0x74, 0x0c, 0xc6, 0x05, 0,    0,    0,    0,    0x01};
    memcpy(client_end + 2, &match_over, 4);
    memcpy(client_end + 11, &winners, 4);
    memcpy(client_end + 20, &match_over, 4);
    unsigned char client_end_here[25]{};
    battle_result_ok = read_memory(image + 0x132913, client_end_here, sizeof client_end_here) &&
                       !memcmp(client_end_here, client_end, sizeof client_end);
    // The AI's player-team lookup for Versus bots, and its users: the skill sum, the auto-balance's
    // one-team check and its switches. The lookup's own bytes are checked only before hooking it.
    const DWORD net_select = at + 0x007e8007;
    unsigned char player_team[24] = {0xa1, 0,    0,    0,    0,    0x56, 0x57, 0x8b, 0xf9, 0x85, 0xc0, 0x75,
                                     0x35, 0x33, 0xd2, 0x33, 0xf6, 0x80, 0x3d, 0,    0,    0,    0,    0x00};
    memcpy(player_team + 1, &characters, 4);
    memcpy(player_team + 19, &net_select, 4);
    const DWORD ally_offsets = at + 0x01e2fd8c, ally_base = at + 0x0078879c, ally_catchup = at + 0x01e2fda8,
                enemy_offsets = at + 0x01e2fd78, enemy_base = at + 0x007887a8, enemy_catchup = at + 0x01e2fda0;
    unsigned char skill[48] = {0x84, 0xdb, 0x74, 0x16, 0x8b, 0x34, 0x85, 0,    0, 0,    0,    0x03,
                               0x34, 0x85, 0,    0,    0,    0,    0x03, 0x35, 0, 0,    0,    0,
                               0xeb, 0x14, 0x8b, 0x34, 0x85, 0,    0,    0,    0, 0x03, 0x34, 0x85,
                               0,    0,    0,    0,    0x03, 0x35, 0,    0,    0, 0,    0x33, 0xdb};
    memcpy(skill + 7, &ally_offsets, 4);
    memcpy(skill + 14, &ally_base, 4);
    memcpy(skill + 20, &ally_catchup, 4);
    memcpy(skill + 29, &enemy_offsets, 4);
    memcpy(skill + 36, &enemy_base, 4);
    memcpy(skill + 42, &enemy_catchup, 4);
    static const unsigned char one_team[26] = {0xb9, 0x01, 0x00, 0x00, 0x00, 0xe8, 0x59, 0x90, 0x00,
                                               0x00, 0xb9, 0x02, 0x00, 0x00, 0x00, 0x8a, 0xd8, 0xe8,
                                               0x4d, 0x90, 0x00, 0x00, 0x32, 0xc3, 0x0f, 0x84};
    const DWORD balance_option = at + 0x007e5887, balance_mission = at + 0x007e5886;
    unsigned char balance_on[30] = {0x80, 0x3d, 0,    0,    0,    0,    0x00, 0x74, 0x15, 0x80,
                                    0x3d, 0,    0,    0,    0,    0x00, 0x74, 0x0c, 0xe8, 0x89,
                                    0xff, 0xff, 0xff, 0x83, 0xf8, 0x03, 0x0f, 0x95, 0xc0, 0xc3};
    memcpy(balance_on + 2, &balance_option, 4);
    memcpy(balance_on + 11, &balance_mission, 4);
    const Code ai_code[] = {
        {0x038cd8, skill, sizeof skill},            // skill sum: offset, base and catch-up per side
        {0x0394dd, one_team, sizeof one_team},      // catch-up only with one player's team
        {0x039380, balance_on, sizeof balance_on},  // the catch-up's switches
    };
    bool ai_code_ok = true;
    for (const Code& c : ai_code) {
        unsigned char here[48]{};
        ai_code_ok = ai_code_ok && read_memory(image + c.rva, here, c.size) && !memcmp(here, c.bytes, c.size);
    }
    // Hook the lookup only if all of its users match.
    const char* bots_hook = "differs";
    if (player_team_hooked) {
        bots_hook = "kept";
    } else if (ai_code_ok) {
        unsigned char here[24]{};
        if (read_memory(image + 0x042540, here, sizeof here) && !memcmp(here, player_team, sizeof player_team)) {
            MH_STATUS status = MH_Initialize();
            if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
                void* original = nullptr;
                status = MH_CreateHook(image + 0x042540, reinterpret_cast<void*>(&bots::player_team), &original);
                if (status == MH_OK) {
                    bots::original = reinterpret_cast<bots::PlayerTeamFn>(original);
                    status = MH_EnableHook(image + 0x042540);
                    player_team_hooked = status == MH_OK;
                }
            }
            bots_hook = status == MH_OK ? "hooked" : MH_StatusToString(status);
        }
    }
    battle_ai_ok = ai_code_ok && player_team_hooked;
    log("PLAYER_BATTLES battles=%s state_getter=%s lan_sockets=%s join_name=%s in_battle=%s error_clear=%s turret_pool=%s bonus_slots=%s waves=%s result=%s versus_bots=%s aim=%s",
        battle::mode_name(battle_mode()), battle_state_ok ? "verified" : "differs",
        battle_sockets_ok ? "wrapped" : "unavailable", battle_join_ok ? "verified" : "differs",
        battle_play_ok ? "verified" : "differs", battle_error_ok ? "verified" : "differs",
        battle_turret_ok ? "verified" : "differs", battle_bonus_ok ? "verified" : "differs",
        battle_waves_ok ? "verified" : "differs", battle_result_ok ? "verified" : "differs", bots_hook,
        battle_aim_ok ? "verified" : "differs");
}
// Keeps the socket wrappers in step with the trackers: sealed from hosting or joining until the
// network game closes, and busy too while an announced battle waits to be joined.
static void update_seal() {
    const bool busy = battles.sealed() || guest.busy();
    if (busy != battle_busy_now) {
        battle_busy_now = busy;
        battle_set_busy(busy);
    }
    // The tunnel's role is set before the first sealed packet; in Versus either PC can host.
    battle_tunnel_role(battles.sealed() ? 1 : guest.sealed() ? 2 : 0);
    const bool on = battles.sealed() || guest.sealed();
    if (on == battle_sealed_now) return;
    battle_sealed_now = on;
    battle_seal(on);
    const BattleSealCounts c = battle_seal_counts();
    log("BATTLE_SEAL %s packets_to_tunnel=%u packets_without_peer=%u packets_from_tunnel=%u network_packets_discarded=%u",
        on ? "on" : "off", c.sent, c.dropped, c.delivered, c.discarded);
}
// Whether the other PC takes part in this PC's shared battles: a ready Versus opponent, or a
// paired co-op friend when this PC hosts.
static bool battle_paired() { return versus_active() ? versus_paired() : coop_host_paired(); }
// Answers the shell script's GCOBS:ask:battle_* questions: hosting, returning from a battle, giving
// up a join, kept results, the party size and which battle note to show.
bool battle_answer(const char* question) {
    if (!strcmp(question, "battle_host")) {
        const bool versus = versus_active();
        const bool paired = battle_paired();
        const bool yes = battle_state_ok && battle_sockets_ok && battle::host_battle(battle_mode(), paired);
        log("BATTLE_ASKED host=%d battles=%s paired=%d versus=%d state_getter=%d lan_sockets=%d", yes,
            battle::mode_name(battle_mode()), paired, versus, battle_state_ok, battle_sockets_ok);
        return yes;
    }
    if (!strcmp(question, "battle_return")) {
        const bool yes = guest.returning_now(battle_state());
        if (yes) log("BATTLE_RETURNING number=%u", guest.number);
        return yes;
    }
    // Versus: when the battle's host left after sending its result, this galaxy keeps that result.
    if (!strcmp(question, "battle_keep_mine") || !strcmp(question, "battle_keep_theirs")) {
        const int kept = versus_active() && guest.returning_now(battle_state()) ? guest.kept_winner() : 0;
        const bool yes = kept == (strcmp(question, "battle_keep_mine") ? 2 : 1);
        if (yes)
            log("BATTLE_RESULT_KEPT number=%u winner=%s mask=%08X own_team=%d", guest.number,
                kept == 1 ? "mine" : "theirs", guest.result_mask, guest.own_team);
        return yes;
    }
    if (!strcmp(question, "battle_join_off")) return guest.join_off();
    // The co-op game's player count, for the battle options.
    unsigned n = 0;
    if (sscanf_s(question, "battle_party_%u", &n) == 1) return n && battle_party() == n;
    if (!strncmp(question, "battle_note_", 12)) {
        // Per-friend notes: battle_note_friend<slot>_<failed|stayed|left>.
        unsigned slot = 0;
        char what[8]{};
        if (sscanf_s(question + 12, "friend%u_%7[a-z]", &slot, what, static_cast<unsigned>(sizeof what)) == 2) {
            const battle::Note told = battle_paired() ? battles.friend_note(slot) : battle::Note::none;
            if (!strcmp(what, "failed")) return told == battle::Note::friend_failed;
            if (!strcmp(what, "stayed")) return told == battle::Note::friend_stayed;
            if (!strcmp(what, "left")) return told == battle::Note::friend_left || told == battle::Note::friend_quit;
            return false;
        }
        const battle::Note asked = battle::parse_note(question + 12);
        if (asked == battle::Note::none) return false;
        if (asked == battle::Note::join_failed) return guest.note();
        return battle_paired() && battles.note() == asked;
    }
    return false;
}
// Tells the host what this game did in its battle.
static void report_part(battle::Guest::Event e) {
    const battle::Part part = guest.part(e);
    if (part == battle::Part::none) return;
    battle_guest_report(guest.number, part);
    log("BATTLE_PART number=%u part=%s", guest.number, battle::part_name(part));
}
// Handles the shell script's GCOBS:battle:* reports.
void battle_event(const char* event) {
    const bool paired = battle_paired(), versus = versus_active();
    switch (battles.report(event, GetTickCount64(), paired, versus)) {
    case battle::Tracker::Event::hosting:
        log("BATTLE_HOSTING number=%u token=%s mission=%s side=%s world=%s units=%s bonus=%s battles=%s friend=%d versus=%d",
            battles.number, battles.info.token.c_str(), battles.info.mission.c_str(), battles.info.side.c_str(),
            battles.info.world.c_str(), battles.info.units.c_str(),
            battles.info.bonus.empty() ? "none" : battles.info.bonus.c_str(), battle::mode_name(battle_mode()), paired,
            versus);
        clear_engine_error(battles.number, "hosting");
        battle_host_announce(battles.number, battle::encode_info(battles.info));
        break;
    case battle::Tracker::Event::fallback:
        log("BATTLE_FALLBACK %s; the single-player battle runs instead", event + 9);
        battle_host_over(battles.number);
        break;
    case battle::Tracker::Event::own_quit:
        log("BATTLE_OWN_QUIT number=%u host=1 versus=%d", battles.number, battles.versus);
        break;
    default: break;
    }
    const battle::Guest::Event happened = guest.report(event);
    switch (happened) {
    case battle::Guest::Event::own_quit: log("BATTLE_OWN_QUIT number=%u host=0 versus=%d", guest.number, versus); break;
    case battle::Guest::Event::gave_up:
        log("BATTLE_JOIN_GAVE_UP number=%u %s; the friend's game stays in the galaxy note=%d", guest.number, event,
            guest.note());
        log_join_error(guest.number);
        clear_engine_error(guest.number, "join");
        report_part(happened);
        break;
    case battle::Guest::Event::found:
        guest_lobby_frames = 0;
        guest_lobby_failed = false;
        log("BATTLE_JOIN_FOUND number=%u", guest.number);
        break;
    default: break;
    }
    // heroes:<card>: the host's script turned heroes on for the Leader card.
    if (!strncmp(event, "heroes:", 7)) log("BATTLE_HEROES number=%u card=%s", battles.number, event + 7);
    // noted:<name>: the shell script has shown a battle note.
    if (!strncmp(event, "noted:", 6)) {
        const battle::Note shown = battle::parse_note(event + 6);
        if (shown == battle::Note::join_failed) guest.noted();
        else if (shown != battle::Note::none) battles.noted();
        if (shown != battle::Note::none)
            log("BATTLE_NOTE_SHOWN note=%s number=%u", battle::note_name(shown),
                shown == battle::Note::join_failed ? guest.number : battles.number);
    }
    update_seal();
}
// This player's battle team, from its galaxy side and each team's faction as choose_side reads
// them; 0 when unknown.
static int own_battle_team() {
    DWORD display = 0, teams = 0, team1 = 0, team2 = 0;
    int local = 0, faction1 = 0, faction2 = 0, side = 0;
    if (read_memory(image + 0x1b9ae5c, &display, 4) && display)
        read_memory(reinterpret_cast<void*>(display + 0x2000), &local, 4);
    if (local < 0 || local > 3) local = 0;
    if (!read_memory(image + 0x3e9aa0, &teams, 4) || !teams ||
        !read_memory(reinterpret_cast<void*>(teams + 4), &team1, 4) ||
        !read_memory(reinterpret_cast<void*>(teams + 8), &team2, 4) || !team1 || !team2 ||
        !read_memory(reinterpret_cast<void*>(team1 + 0x64), &faction1, 4) ||
        !read_memory(reinterpret_cast<void*>(team2 + 0x64), &faction2, 4) ||
        !read_memory(image + 0x1a56014 + 4 * local, &side, 4))
        return 0;
    return battle::side_team(faction1, faction2, side);
}
// Versus host: the client's player quit, so their side loses. Unless the match already has a
// winner, it ends with MissionVictory for this side, as the mission scripts do.
static void forfeit_step(const FrameSample& s, DWORD hash) {
    if (battles.phase != battle::Tracker::Phase::hosted || !battles.forfeit_wanted || battles.forfeit_done ||
        hash != battle::state_mission)
        return;
    DWORD fresh = 0;
    if (!battle_frame(s, fresh) || !battles.forfeit_due()) return;
    int victory = -1;
    read_memory(image + 0x3e6688, &victory, 4);
    // While this PC's own quit is under way, the game's own rule decides the battle instead.
    unsigned char quitting = 0;
    read_memory(image + 0x1a56009, &quitting, 1);
    const int team = own_battle_team();
    if (victory >= 0 || !team || quitting) {
        log("BATTLE_FORFEIT number=%u team=%d victory=%d ended=0 (%s)", battles.number, team, victory,
            victory >= 0 ? "the match already has its winner"
            : quitting   ? "this PC's own quit is under way"
                         : "this player's battle team is unknown");
        return;
    }
    char source[160];
    _snprintf_s(source, sizeof source, _TRUNCATE,
                "if type(MissionVictory) ~= 'function' then return 0 end\nMissionVictory(%d)\nreturn 6217431\n", team);
    const LuaQueryResult r =
        run_source(reinterpret_cast<void*>(fresh), native_api, source, static_cast<unsigned>(strlen(source)));
    log("BATTLE_FORFEIT number=%u team=%d victory=%d ended=%d stack_ok=%d (the opponent quit the battle; MissionVictory for this side) error=%s",
        battles.number, team, victory, battle_lua_ok(r), r.restored && r.prefix_equal, lua_error(r));
}
// Host: shortly after this PC starts quitting, sets the all-clients-answered flag as the engine
// would, so the quit does not wait out its 6 s timeout for the friend's game.
static void hurry_step(const FrameSample& s, DWORD hash) {
    if (battles.phase != battle::Tracker::Phase::hosted || !battles.own_quit || battles.hurried ||
        hash != battle::state_mission)
        return;
    DWORD fresh = 0;
    if (!battle_frame(s, fresh) || !battles.hurry_due(GetTickCount64())) return;
    unsigned char quitting = 0, allowed = 0;
    const unsigned char yes = 1;
    read_memory(image + 0x1a56009, &quitting, 1);
    read_memory(image + 0x1ba878a, &allowed, 1);
    const bool hurried = quitting && !allowed && write_memory(image + 0x1ba878a, &yes, 1);
    log("BATTLE_QUIT_HURRIED number=%u quitting=%d answered=%d hurried=%d", battles.number, quitting, allowed, hurried);
}
// Keeps battle_closing current: what this PC's goodbye says if the game closes now. In a Versus
// mission the host concedes to the other team and a client reports its quit; nothing in co-op.
static void update_goodbye(DWORD hash) {
    static unsigned said = 0, said_mask = 0;
    static bool said_host = false;
    unsigned number = 0, mask = 0;
    bool host = false;
    if (hash == battle::state_mission && play.active) {
        if (play.host && battles.phase == battle::Tracker::Phase::hosted && battles.versus &&
            play.number == battles.number) {
            number = battles.number;
            host = true;
            mask = battle::concede_mask(play.own_team);
        } else if (!play.host && guest.phase == battle::Guest::Phase::battle && versus_active() &&
                   play.number == guest.number) {
            number = guest.number;
        }
    }
    if (number == said && host == said_host && mask == said_mask) return;
    said = number;
    said_host = host;
    said_mask = mask;
    battle_closing(number, host, mask);
}
// Each frame: the host's side of shared battles, its friends' reports, and closing the network game
// once back in the shell.
static void step_battle(const FrameSample& s) {
    const DWORD hash = battle_state();
    if (!battle_state_ok) return;
    // Each friend's report on this battle, by session slot.
    for (unsigned slot = 1; slot <= battle::max_peers; ++slot) {
        unsigned seq = 0, reported = 0;
        battle::Part part = battle::Part::none;
        if (!battle_host_report(slot, host_friend_seen[slot], seq, reported, part)) continue;
        host_friend_seen[slot] = seq;
        battles.friend_report(slot, reported, part);
        if (battles.forfeit_wanted && !battles.forfeit_done) log("BATTLE_OPPONENT_QUIT number=%u", battles.number);
    }
    if (battles.sealed()) battles.friends_told(battles.number, battle_host_told(battles.number));
    forfeit_step(s, hash);
    hurry_step(s, hash);
    switch (battles.state(hash, GetTickCount64())) {
    case battle::Tracker::Event::launched: {
        int players = 0;  // the match's player count, from the options step
        read_memory(image + 0x3e825c, &players, 4);
        log("BATTLE_LAUNCHED number=%u token=%s mission=%s hosted_battles=%u players=%d", battles.number,
            battles.info.token.c_str(), battles.info.mission.c_str(), battles.launched, players);
        break;
    }
    case battle::Tracker::Event::ended: {
        // slot:part for each friend told of this battle.
        std::string parts;
        for (unsigned slot = 1; slot <= battle::max_peers; ++slot)
            if (battles.note_slots() & (1u << slot))
                parts +=
                    (parts.empty() ? "" : ",") + std::to_string(slot) + ":" + battle::part_name(battles.parts[slot]);
        log("BATTLE_ENDED number=%u token=%s mission=%s friend=%s friends=%s", battles.number,
            battles.info.token.c_str(), battles.info.mission.c_str(),
            battles.with_friend ? battle::part_name(battles.parts[1]) : "not_paired",
            parts.empty() ? "-" : parts.c_str());
        end_battle_play();
        battle_host_over(battles.number);
        break;
    }
    case battle::Tracker::Event::abandoned:
        log("BATTLE_ABANDONED number=%u token=%s mission=%s (no launch and no report within %llu s)", battles.number,
            battles.info.token.c_str(), battles.info.mission.c_str(), battle::Tracker::launch_limit_ms / 1000);
        battle_host_over(battles.number);
        break;
    default: break;
    }
    update_seal();
    if (battles.phase == battle::Tracker::Phase::hosted) step_battle_play(s, hash, battles.number, true);
    else if (play.host) stop_even_bots();
    update_goodbye(hash);
    // Back in the shell after a hosted battle: close the network game after 30 safe shell frames.
    if (!battles.cleanup_due()) {
        battle_cleanup_frames = 0;
        return;
    }
    DWORD fresh = 0;
    if (hash != battle::state_shell || !battle_frame(s, fresh)) {
        battle_cleanup_frames = 0;
        return;
    }
    if (++battle_cleanup_frames < 30) return;
    const LuaQueryResult r =
        run_source(reinterpret_cast<void*>(fresh), native_api, battle_cleanup_query, sizeof(battle_cleanup_query) - 1);
    battles.cleaned();  // one attempt; a failing chunk is not retried
    log("BATTLE_CLEANUP lua_ok=%d stack_ok=%d note=%s (CancelLogin, CloseNetShell(1), SetInNetGame(nil), as the stock Single Player button) error=%s",
        battle_lua_ok(r), r.restored && r.prefix_equal, battle::note_name(battles.note()), lua_error(r));
    clear_engine_error(battles.number, "cleanup");
    update_seal();
}
// Versus, defender's PC: shows this side's pass of the attacker's card screen, which reports the pick
// itself. If the script fails, no card is answered so the attacker does not wait.
constexpr char versus_card_query[] = "return function(data)\n"
                                     "  local show = rawget(_G, '_GCT_versus_card')\n"
                                     "  assert(type(show) == 'function')\n"
                                     "  if show(data) then return 6217431 end\n"
                                     "  return 0\n"
                                     "end\n";
// window: an idle galaxy frame between apply cycles with no battle under way. Runs on every 15th.
static void step_versus_card(DWORD fresh, bool window) {
    static unsigned frames = 0;
    if (!window || !versus_active() || ++frames % 15) return;
    unsigned number = 0;
    std::string data;
    if (!versus_card_pending(number, data)) return;
    char head[16];
    _snprintf_s(head, sizeof head, _TRUNCATE, "%u\n", number);
    const std::string text = head + data;
    const LuaQueryResult r =
        run_lua_data_query(reinterpret_cast<void*>(fresh), native_api, versus_card_query, sizeof(versus_card_query) - 1,
                           text.data(), static_cast<unsigned>(text.size()));
    if (battle_lua_ok(r)) {
        versus_card_taken(number);
        log("VERSUS_CARD_SHOWN number=%u", number);
    } else if (r.load_status || r.call_status || !r.restored || !r.prefix_equal) {
        versus_card_taken(number);
        char answer[32];
        _snprintf_s(answer, sizeof answer, _TRUNCATE, "card:%u:0", number);
        versus_event(answer);
        log("VERSUS_CARD_REFUSED number=%u call=%d stack_ok=%d", number, r.call_status, r.restored && r.prefix_equal);
    }
}
// Host: gives the party screen (ifs_gct_party) the co-op game's rows when they change, and every 60th
// frame anyway as a rebuilt shell starts without them. The battle notes name friends from them.
constexpr char party_query[] = "return function(data)\n"
                               "  local take = rawget(_G, '_GCT_party_rows')\n"
                               "  assert(type(take) == 'function')\n"
                               "  if take(data) then return 6217431 end\n"
                               "  return 0\n"
                               "end\n";
// window: a safe frame at the menus or in the galaxy. Runs on every 15th.
static void step_party(DWORD fresh, bool window) {
    static unsigned frames = 0, given = 0, failed = 0;
    if (!window || ++frames % 15) return;
    unsigned seq = 0;
    std::string data;
    if (!party_rows(seq, data) || (seq == given && frames % 60)) return;
    const LuaQueryResult r =
        run_lua_data_query(reinterpret_cast<void*>(fresh), native_api, party_query, sizeof(party_query) - 1,
                           data.data(), static_cast<unsigned>(data.size()));
    if (battle_lua_ok(r)) {
        if (seq != given) {
            // A friend's row starts with its slot, a note about a friend who left with L.
            unsigned friends = 0, left = 0;
            for (size_t i = 1; i < data.size(); ++i) {
                if (data[i - 1] != '\n') continue;
                friends += data[i] >= '1' && data[i] <= '3' ? 1 : 0;
                left += data[i] == 'L' ? 1 : 0;
            }
            log("PARTY_ROWS seq=%u friends=%u left=%u", seq, friends, left);
        }
        given = seq;
    } else if (failed != seq) {
        failed = seq;
        log("PARTY_ROWS_FAILED seq=%u call=%d stack_ok=%d error=%s", seq, r.call_status, r.restored && r.prefix_equal,
            lua_error(r));
    }
}
// The friend's side of shared battles, from the apply step. join_window: the host's galaxy shows and
// no apply cycle is in progress.
static void step_battle_guest(const FrameSample& s, bool join_window, std::uint64_t now) {
    if (!battle_state_ok) return;
    // Read before host_left below: the Steam thread counts a session's end before the next one opens.
    const unsigned session = battle_guest_session();
    unsigned number = 0;
    std::string text;
    // The newest announcement reads as fresh until taken; one that cannot be taken is logged once.
    if (battle_guest_offered(guest.session, guest.number, number, text)) {
        battle::Info info;
        const bool decoded = battle::decode_info(text, info);
        switch (guest.offer(number, info, now)) {
        case battle::Guest::Event::offered:
            log("BATTLE_OFFERED number=%u token=%s mission=%s side=%s world=%s units=%s bonus=%s", number,
                info.token.c_str(), info.mission.c_str(), info.side.c_str(), info.world.c_str(), info.units.c_str(),
                info.bonus.empty() ? "none" : info.bonus.c_str());
            break;
        case battle::Guest::Event::ignored:
            log("BATTLE_OFFER_IGNORED number=%u decoded=%d phase=%d", number, decoded, static_cast<int>(guest.phase));
            break;
        default: break;
        }
    }
    guest.host_over(battle_guest_over(guest.session), now);
    // The host's match result.
    unsigned result_number = 0, result_mask = 0;
    if (battle_guest_result(guest.session, result_number, result_mask))
        guest.host_result(result_number, result_mask, now);
    // The battle's host has gone: an offer expires, a join gives up, a battle is left. In Versus the
    // session host joins the other PC's battles, so there the friend leaving counts.
    const unsigned host_left = versus_active() && versus_session_host()
                                   ? apply_progress().friend_left.load(std::memory_order_acquire)
                                   : apply_progress().host_left.load(std::memory_order_acquire);
    if (host_left != guest_host_left_seen) {
        guest_host_left_seen = host_left;
        guest.host_gone(now);
        if (guest.busy()) log("BATTLE_HOST_GONE number=%u phase=%d", guest.number, static_cast<int>(guest.phase));
    }
    // A new session numbers its battles from 1 again; it is taken once no battle is in progress.
    const unsigned last = guest.number;
    if (guest.renumber(session)) log("BATTLE_SESSION session=%u last_number=%u", session, last);
    // The engine lost the host (no packets for 15 s, a kick, or the host quit), so leave the battle
    // instead of waiting on its error popup. Read before the state, which may already be the shell.
    if (guest.phase == battle::Guest::Phase::battle && !guest.lost_at) {
        const EngineErrorText e = engine_error_text();
        const battle::EngineError kind = battle::engine_error(e.kept ? e.key : nullptr);
        if (kind == battle::EngineError::host_quit || kind == battle::EngineError::lost) {
            guest.engine_lost(kind == battle::EngineError::host_quit, now);
            log("BATTLE_HOST_LOST number=%u message=%s level=%d (leaving in %llu s)", guest.number, e.key, e.level,
                battle::Guest::quit_after_ms / 1000);
        }
    }
    const DWORD hash = battle_state();
    // The host quit: its packets set this game's quit request and host-quit flag. Leave now through the
    // battle's Quit, which restores the galaxy save's settings; the engine's own 3 s exit would not.
    if (guest.phase == battle::Guest::Phase::battle && hash == battle::state_mission && !guest.quitting_at) {
        unsigned char transition = 1, quitting = 0, host_quit = 0;
        if (read_memory(image + 0x1a563b0, &transition, 1) && !transition &&
            read_memory(image + 0x1a56009, &quitting, 1) && read_memory(image + 0x1ba8789, &host_quit, 1) && quitting &&
            host_quit && guest.host_quitting(now))
            log("BATTLE_HOST_QUITTING number=%u", guest.number);
    }
    const battle::Guest::Event happened = guest.state(hash, now);
    switch (happened) {
    case battle::Guest::Event::expired:
        log("BATTLE_OFFER_EXPIRED number=%u host_gone=%d note=%d", guest.number, guest.gone_at != 0, guest.note());
        break;
    case battle::Guest::Event::abandoned:
        log("BATTLE_JOIN_ABANDONED number=%u (no launch and no report within %llu s; lobby updates=%u)", guest.number,
            battle::Guest::join_limit_ms / 1000, guest_lobby_frames);
        log_join_error(guest.number);
        break;
    case battle::Guest::Event::launched: {
        // The player count copied from the host's session when joining.
        int players = 0;
        read_memory(image + 0x3e825c, &players, 4);
        log("BATTLE_JOINED number=%u token=%s mission=%s lobby_updates=%u players=%d", guest.number,
            guest.info.token.c_str(), guest.info.mission.c_str(), guest_lobby_frames, players);
        break;
    }
    case battle::Guest::Event::ended:
        log("BATTLE_ENDED number=%u left_early=%d host_quitting=%d hurried=%d", guest.number, guest.left_early(),
            guest.quitting_at != 0, guest.hurried);
        end_battle_play();
        break;
    default: break;
    }
    report_part(happened);
    DWORD fresh = 0;
    const bool guarded = battle_frame(s, fresh);
    if (guest.phase == battle::Guest::Phase::battle && hash == battle::state_mission) {
        step_battle_play(s, hash, guest.number, false);
        if (play.own_team) guest.own_team = play.own_team;
        // The match is over once the engine records a winner (-1 until then).
        int victory = -1;
        if (!guest.match_over_at && read_memory(image + 0x3e6688, &victory, 4) && victory >= 0) {
            guest.match_over(now);
            log("BATTLE_MATCH_OVER number=%u victory=%d (leaving in %llu s)", guest.number, victory,
                battle::Guest::quit_after_match_ms / 1000);
        }
    }
    if (guest.phase == battle::Guest::Phase::offered) {
        // Join from the host's galaxy between cycles, every 15th safe frame, with no other popup open.
        if (!join_window || !guarded || hash != battle::state_shell || ++guest_join_frames % 15) return update_seal();
        const LuaQueryResult can = run_source(reinterpret_cast<void*>(fresh), native_api, guest_can_join_query,
                                              sizeof(guest_can_join_query) - 1);
        if (!battle_lua_ok(can)) {
            if (can.load_status || can.call_status || !can.restored || !can.prefix_equal) {
                log("BATTLE_JOIN_REFUSED number=%u (the shell script's join is missing) call=%d stack_ok=%d",
                    guest.number, can.call_status, can.restored && can.prefix_equal);
                report_part(guest.refuse());
            }
            return update_seal();  // a popup is open; look again in 15 frames
        }
        if (!write_join_name(guest.info.token)) {
            log("BATTLE_JOIN_REFUSED number=%u (AutoNetJoin's name buffer cannot be set)", guest.number);
            report_part(guest.refuse());
            return update_seal();
        }
        // A stale network error from an earlier game would end this battle at once.
        clear_engine_error(guest.number, "joining");
        // Seal first, so the join's first network call already goes through the tunnel.
        guest.joining(now);
        update_seal();
        // The host's bonus cards are activated before joining only if keep_bonus_slots can then move
        // them to where this game reads them.
        battle::Info joined = guest.info;
        if (!battle_bonus_ok) joined.bonus.clear();
        const std::string data = battle::encode_info(joined);
        const LuaQueryResult r =
            run_lua_data_query(reinterpret_cast<void*>(fresh), native_api, guest_join_query,
                               sizeof(guest_join_query) - 1, data.data(), static_cast<unsigned>(data.size()));
        if (battle_lua_ok(r)) {
            log("BATTLE_JOINING number=%u token=%s bonus=%s (the joining popup is up%s)", guest.number,
                guest.info.token.c_str(), joined.bonus.empty() ? "none" : joined.bonus.c_str(),
                joined.bonus.size() < guest.info.bonus.size() ? "; the cards stay off: bonus_slots differs" : "");
        } else {
            log("BATTLE_JOIN_REFUSED number=%u load=%d call=%d stack_ok=%d; the friend's game stays in the galaxy",
                guest.number, r.load_status, r.call_status, r.restored && r.prefix_equal);
            report_part(guest.refuse());
        }
        return update_seal();
    }
    // Session found: the stock lobby update on every safe shell frame until the mission loads.
    if (guest.lobby_due() && guarded && hash == battle::state_shell) {
        const LuaQueryResult r =
            run_source(reinterpret_cast<void*>(fresh), native_api, guest_lobby_query, sizeof(guest_lobby_query) - 1);
        const bool ok = battle_lua_ok(r);
        if (!guest_lobby_frames++ || (!ok && !guest_lobby_failed))
            log("BATTLE_JOIN_LOBBY number=%u lua_ok=%d stack_ok=%d", guest.number, ok, r.restored && r.prefix_equal);
        if (!ok) guest_lobby_failed = true;
    }
    // A join that gave up: close the network game it opened.
    if (guest.close_due()) {
        if (hash != battle::state_shell || !guarded) return update_seal();
        const LuaQueryResult r =
            run_source(reinterpret_cast<void*>(fresh), native_api, guest_close_query, sizeof(guest_close_query) - 1);
        guest.closed();
        log("BATTLE_JOIN_CLOSED number=%u lua_ok=%d stack_ok=%d note=%d (the join's network game is closed; applying resumes) error=%s",
            guest.number, battle_lua_ok(r), r.restored && r.prefix_equal, guest.note(), lua_error(r));
        clear_engine_error(guest.number, "closed");
    }
    // Leave a battle the host has ended or left, or whose match is over; at once if the host quits.
    if (guest.phase == battle::Guest::Phase::battle && hash == battle::state_mission && guarded &&
        guest.quit_due(now)) {
        const LuaQueryResult r =
            run_source(reinterpret_cast<void*>(fresh), native_api, guest_quit_query, sizeof(guest_quit_query) - 1);
        log("BATTLE_LEAVING number=%u lua_ok=%d host_over=%d match_over=%d host_gone=%d host_lost=%d host_quitting=%d (the friend's game quits the battle, as the pause menu's Quit) error=%s",
            guest.number, battle_lua_ok(r), guest.over_at != 0, guest.match_over_at != 0, guest.gone_at != 0,
            guest.lost_at != 0, guest.quitting_at != 0, lua_error(r));
    }
    // That quit waits up to 3 s for an answer from the host that will not come. Shortly after, set the
    // answered flag as the host's reply would, so the mission ends at the end of this frame.
    if (guest.phase == battle::Guest::Phase::battle && hash == battle::state_mission && guarded &&
        guest.hurry_due(now)) {
        unsigned char quitting = 0, allowed = 0;
        const unsigned char yes = 1;
        read_memory(image + 0x1a56009, &quitting, 1);
        read_memory(image + 0x1ba878a, &allowed, 1);
        const bool hurried = quitting && !allowed && write_memory(image + 0x1ba878a, &yes, 1);
        log("BATTLE_LEAVE_HURRIED number=%u quitting=%d answered=%d hurried=%d", guest.number, quitting, allowed,
            hurried);
    }
    // Back in the galaxy: close the network game once the shell has settled.
    if (guest.phase == battle::Guest::Phase::returning) {
        if (hash != battle::state_shell || !guarded) {
            guest_cleanup_frames = 0;
            return update_seal();
        }
        if (++guest_cleanup_frames < 30) return update_seal();
        guest_cleanup_frames = 0;
        const LuaQueryResult r = run_source(reinterpret_cast<void*>(fresh), native_api, battle_cleanup_query,
                                            sizeof(battle_cleanup_query) - 1);
        guest.returned();
        log("BATTLE_RETURNED number=%u lua_ok=%d stack_ok=%d host_gone=%d (network game closed; %s) error=%s",
            guest.number, battle_lua_ok(r), r.restored && r.prefix_equal, guest.gone_at != 0,
            guest.gone_at ? "the co-op game is over: back to the menus" : "applying resumes", lua_error(r));
        clear_engine_error(guest.number, "returned");
    }
    update_seal();
}
#endif

// Verifies the engine's Lua API by its first bytes, then arms the install and the player features.
bool initialize_lua_query(HMODULE game) {
    // luaL_loadbuffer, lua_pcall, lua_gettop and lua_settop, checked by their first 16 bytes.
    struct Guard {
        DWORD rva;
        unsigned char bytes[16];
    };
    static const Guard guards[] = {
        {0x29b030, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x8b, 0x45, 0x0c, 0xff, 0x75, 0x14, 0x89, 0x45, 0xf8, 0x8b}},
        {0x29beb0, {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x14, 0x83, 0xec, 0x08, 0x56, 0x57, 0x8b, 0x7d, 0x08, 0x85, 0xc0}},
        {0x29bba0, {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x08, 0x8b, 0x41, 0x08, 0x2b, 0x41, 0x0c, 0xc1, 0xf8, 0x03, 0x5d}},
        {0x29c400, {0x55, 0x8b, 0xec, 0x8b, 0x55, 0x0c, 0x85, 0xd2, 0x78, 0x37, 0x8b, 0x45, 0x08, 0xc1, 0xe2, 0x03}}};
    image = reinterpret_cast<unsigned char*>(game);
    for (const auto& g : guards) {
        unsigned char bytes[16];
        if (!read_memory(image + g.rva, bytes, sizeof bytes) || memcmp(bytes, g.bytes, sizeof bytes)) {
            log("LUA_QUERY REJECT API guard RVA=%08lX", g.rva);
            return false;
        }
    }
    native_api = {reinterpret_cast<decltype(native_api.gettop)>(image + 0x29bba0),
                  reinterpret_cast<decltype(native_api.settop)>(image + 0x29c400),
                  reinterpret_cast<decltype(native_api.load)>(image + 0x29b030),
                  reinterpret_cast<decltype(native_api.pcall)>(image + 0x29beb0)};
#if GC_PLAYER_APPLY
    // lua_pushlstring, which passes received data to the apply scripts.
    const unsigned char push_guard[] = {0x55, 0x8b, 0xec, 0x56, 0x57, 0x8b, 0x7d, 0x08,
                                        0x8b, 0x4f, 0x10, 0x8b, 0x41, 0x24, 0x3b, 0x41};
    unsigned char push_actual[sizeof push_guard]{};
    const bool push_ok = read_memory(image + 0x29c000, push_actual, sizeof push_actual) &&
                         !memcmp(push_guard, push_actual, sizeof push_guard);
    // Applying needs these guards; capture does not, so it runs without them.
    apply_enabled = push_ok && native_save_guards() && transition_guards();
    log("PLAYER_APPLY %s", apply_enabled
                               ? "armed newest_revision_wins=1 loads=from_menus(flow 0 or 4) galaxy=in_place cap=none"
                               : "disabled: native save, transition or string-push guards differ");
    if (push_ok) native_api.pushstring = reinterpret_cast<decltype(native_api.pushstring)>(image + 0x29c000);
#endif
    deadline = GetTickCount64() + 30000;
#if GC_PLAYER
    deadline = ~0ULL;  // no deadline: ready_query gates the install
    log("PLAYER armed readiness=ifs_freeform_main+ifs_main+AddIFScreen interval_frames=120 deadline=none");
#endif
#if GC_PLAYER_MENU
    menu_api_ready.store(true, std::memory_order_release);
    log("PLAYER_MENU armed at the shell's custom_gc_0.lvl probe (Galactic Conquest > Online)");
    arm_battles(game);
#endif
#if GC_CAMPAIGN_CAPTURE
    log("CAMPAIGN_CAPTURE armed fixed_script=campaign_capture_v7 (no emission budget, cursor and AI turn sampled) max_install_attempts=1 main_menu_only=1 max_rearms=%u rearm_requires=same_lua_context",
        RearmGate::max_attempts);
#else
    log("LUA_QUERY armed fixed_expression=return_6217431 max_attempts=1 main_menu_only=1");
#endif
    return true;
}

#if GC_CAMPAIGN_CAPTURE
// Reinstalls the capture script after a shell rebuild, through the rearm gate.
static void probe_rearm(const FrameSample& s) {
    const unsigned reloads = shell_reload_count();
    if (!rearm.enabled || !image) return;
    if (reloads != rearm.generation) {
        log("REARM reload=%u observed; checking fresh guards for bounded reinstall", reloads);
    }
    if (reloads == rearm.generation && rearm.settled) return;
    // The same frame guards as the install, each kept for the waiting log.
    DWORD flow = 0, fresh = 0;
    unsigned char transition = 1, suspend = 1;
    const bool caller_ok = s.readable && s.caller == reinterpret_cast<DWORD>(image) + 0x217a9c;
    const bool flow_read = read_memory(image + 0x3e7e9c, &flow, 4);
    const bool transition_ok = read_memory(image + 0x1a563b0, &transition, 1) && !transition;
    const bool suspend_ok = read_memory(image + 0x1a56fa3, &suspend, 1) && !suspend;
    const bool context_ok = read_memory(image + 0x1a579a0, &fresh, 4) && fresh == s.context;
    Header h{};
    const bool idle_ok = context_ok && header(reinterpret_cast<void*>(fresh), h) && idle(h);
    const auto decision = rearm.step(reloads, fresh, s.thread, flow,
                                     caller_ok && flow_read && transition_ok && suspend_ok && context_ok && idle_ok);
    const auto now = GetTickCount64();
    if (decision != RearmGate::wait) {
        if (decision == RearmGate::context_mismatch) {
            log("REARM skipped reload=%u reason=context_mismatch installed=%08lX current=%08lX", reloads,
                static_cast<DWORD>(rearm.installed_context), fresh);
            return;
        }
        if (decision == RearmGate::attempt_limit) {
            log("REARM skipped reload=%u reason=attempt_limit rearms=%u", reloads, rearm.attempts);
            return;
        }
        const auto result = run_lua_query(reinterpret_cast<void*>(fresh), native_api);
        rearm.enabled = succeeded(result);  // any failure stops further attempts
        publish(result);
        log("REARM executed reload=%u attempt=%u stage=%d called=%d load=%d call=%d before=%d after=%d restored=%d prefix_equal=%d value_ok=%d",
            reloads, rearm.attempts, result.stage, result.called, result.load_status, result.call_status, result.before,
            result.after, result.restored, result.prefix_equal, result.value_ok);
        if (!rearm.enabled) log("REARM disabled reason=execution_failed");
    } else if (!probe_last_log || now - probe_last_log >= 10000) {
        probe_last_log = now;
        log("REARM waiting reload=%u flow=%lu caller=%d transition=%d suspend=%d context=%d idle=%d stable=%u", reloads,
            flow, caller_ok, transition_ok, suspend_ok, context_ok, idle_ok, rearm.stable);
    }
}
#endif

#if GC_PLAYER_APPLY
// A chunk that ran cleanly, returned 6217431 and restored the stack.
static bool lua_ok(const LuaQueryResult& r) {
    return r.stage == 3 && r.called && !r.load_status && !r.call_status && r.restored && r.prefix_equal && r.value_ok;
}
// Applies the host's galaxy revisions on the friend's game: the operations apply::Loop calls
// (apply_logic.h), on the game's own save buffer and Lua state.
struct PlayerApply {
    DWORD context = 0;                // this frame's Lua state
    ApplyOffer pending{}, applied{};  // this cycle's revision, and the last one confirmed
    bool adopted = false;             // the loop adopted this PC's own galaxy (Versus)
    SavedBuffer original{}, candidate{};
    // Runs an apply script, passing the revision's wire data as its argument.
    LuaQueryResult data_query(const char* source, unsigned size, const std::string& wire) {
        return run_lua_data_query(reinterpret_cast<void*>(context), native_api, source, size, wire.data(),
                                  static_cast<unsigned>(wire.size()));
    }
    unsigned newest() { return apply_newest_revision(); }
    // A load from the menus may replace the metagame state, but never a story campaign in progress.
    bool menu_ready() {
        DWORD campaign_length = 1;
        return read_memory(image + 0x1b9c7c4, &campaign_length, 4) && !campaign_length;
    }
    // Whether the galaxy still shows the last revision applied.
    bool in_step() { return lua_ok(data_query(player_live_query, sizeof(player_live_query) - 1, applied.wire)); }
    // Patches the galaxy in place to the newest revision, with no save, load or shell change.
    // Returns 1 when done, 0 to reload instead, -1 to stop.
    int patch(unsigned& revision) {
        if (!apply_newest(applied.revision, pending)) return 0;
        // Before any revision is applied, only an adopted galaxy can be patched.
        if (!applied.revision && !adopted) return 0;
        const bool was_in_step = !applied.revision || in_step();
        if (!was_in_step) log("APPLY_OUT_OF_STEP revision=%u", pending.host_revision);
        const LuaQueryResult change = data_query(player_patch_query, sizeof(player_patch_query) - 1, pending.wire);
        const bool stack_ok = change.restored && change.prefix_equal;
        if (!lua_ok(change)) {
            // The script validates before its first change, so a reload can follow. Only a stack left
            // unrestored stops applying.
            log("APPLY_PATCH_DECLINED revision=%u in_step=%d stack_ok=%d%s", pending.host_revision, was_in_step,
                stack_ok, stack_ok ? "; reloading it instead" : "");
            return stack_ok ? 0 : -1;
        }
        const LuaQueryResult after = data_query(player_live_query, sizeof(player_live_query) - 1, pending.wire);
        log("APPLY_PATCHED revision=%u applied=%d live_ok=%d in_step=%d normalized_sha256=%s", pending.host_revision,
            lua_ok(after), lua_ok(after), was_in_step, pending.normalized_sha256.c_str());
        if (!lua_ok(after)) return after.restored && after.prefix_equal ? 0 : -1;
        applied = pending;
        revision = pending.revision;
        return 1;
    }
    // Saves the revision through the game's own save, keeps the bytes as the candidate and restores the
    // buffer. Returns 1 when staged, 0 if the script refused it for now, -1 to stop.
    int prepare(bool menu, unsigned& revision) {
        NativeSaveIO io;
        if (!apply_newest(applied.revision, pending) || !native_save_guards() || !transition_guards() ||
            !io.read(original) || original.length > 512 || (!menu && !original.length))
            return -1;
        revision = pending.revision;
        const bool was_in_step = menu || in_step();
        LuaQueryResult prepared{};
        candidate = {};
        const SaveTransactionResult t = save_transaction(io, [&] {
            prepared = data_query(menu ? player_prepare_query : player_repeat_prepare_query,
                                  menu ? sizeof(player_prepare_query) - 1 : sizeof(player_repeat_prepare_query) - 1,
                                  pending.wire);
            SavedBuffer check{};
            // A full 512-byte buffer may hold a truncated stream.
            return lua_ok(prepared) && io.read(candidate) && candidate.length && candidate.length < 512 &&
                   io.read(check) && candidate == check;
        });
        SavedBuffer restored{};
        const bool restored_ok = t.restore_writes_ok && t.restored && io.read(restored) && restored == original;
        log("APPLY_PREPARED revision=%u menu=%d ok=%d bytes=%lu storage_restored=%d in_step=%d stack_ok=%d",
            pending.host_revision, menu, t.operation_ok, static_cast<DWORD>(candidate.length), restored_ok, was_in_step,
            prepared.restored && prepared.prefix_equal);
        if (t.operation_ok && restored_ok) return 1;
        if (restored_ok && prepared.restored && prepared.prefix_equal) {
            log("APPLY_PASSED revision=%u (not loadable now; waiting for a newer one)", pending.host_revision);
            return 0;
        }
        return -1;
    }
    // Whether the buffer still holds what prepare found there.
    bool storage_unchanged() {
        SavedBuffer current{};
        NativeSaveIO io;
        return io.read(current) && current == original;
    }
    void withdraw(unsigned) {
        log("APPLY_POSTPONED revision=%u (the game left the stable screen before commit)", pending.host_revision);
    }
    // Loads the candidate: writes it to the buffer, has the game's loader check it, then dispatches.
    // If the dispatch fails, the buffer and the transition state are put back.
    bool commit(bool menu, unsigned) {
        NativeSaveIO io;
        // The engine's state-change request (+0x1A563AC; byte 4 is the transition flag), saved to restore.
        std::array<unsigned char, 12> before{}, after{};
        if (!candidate.length || !native_save_guards() || !transition_guards() ||
            !read_memory(image + 0x1a563ac, before.data(), before.size()) || before[4])
            return false;
        LuaQueryResult verify{}, dispatch{};
        bool scheduled = false, transition_restored = false;
        const bool was_in_step = menu || in_step();
        const SaveTransactionResult t = save_transaction(
            io,
            [&] {
                SavedBuffer staged{};
                if (!io.length(0) || !io.buffer(candidate.bytes) || !io.length(candidate.length) || !io.read(staged) ||
                    !(staged == candidate))
                    return false;
                // Nothing is dispatched unless the game's loader reads back exactly the 26 expected values.
                verify = data_query(player_verify_query, sizeof(player_verify_query) - 1, pending.wire);
                if (!lua_ok(verify) || !native_save_guards() || !transition_guards() || !io.read(staged) ||
                    !(staged == candidate))
                    return false;
                dispatch = run_source(reinterpret_cast<void*>(context), native_api, player_dispatch_query,
                                      sizeof(player_dispatch_query) - 1);
                unsigned char flag = 0;
                scheduled = read_memory(image + 0x1a563b0, &flag, 1) && flag == 1;
                return lua_ok(dispatch) && scheduled;
            },
            true);
        if (t.entered && !t.committed) {
            transition_restored = io.write(0x1a563ac, before.data(), before.size()) &&
                                  read_memory(image + 0x1a563ac, after.data(), after.size()) && before == after;
        }
        log("APPLY_COMMIT revision=%u menu=%d committed=%d scheduled=%d verified=%d in_step=%d buffer_recovered=%d transition_recovered=%d",
            pending.host_revision, menu, t.committed, scheduled, lua_ok(verify), was_in_step, t.restored,
            transition_restored);
        return t.committed;
    }
    // After the load: the galaxy shows the revision and the buffer reads back the same.
    bool confirm(unsigned) {
        NativeSaveIO io;
        SavedBuffer before{}, after{};
        LuaQueryResult live{}, readback{};
        if (native_save_guards() && transition_guards() && io.read(before) && before.length && before.length <= 512) {
            live = data_query(player_live_query, sizeof(player_live_query) - 1, pending.wire);
            if (lua_ok(live)) readback = data_query(player_verify_query, sizeof(player_verify_query) - 1, pending.wire);
        }
        const bool good = lua_ok(live) && lua_ok(readback) && io.read(after) && before == after;
        if (good) applied = pending;
        log("APPLY_CONFIRMED revision=%u applied=%d live_ok=%d readback_ok=%d normalized_sha256=%s",
            pending.host_revision, good, lua_ok(live), lua_ok(readback), pending.normalized_sha256.c_str());
        return good;
    }
    // Records why applying stopped, for the Steam thread to report.
    void stopped(const char* reason) {
        apply_progress().reason.store(reason, std::memory_order_release);
        log("APPLY_STOPPED reason=%s; applying is off until this co-op game ends", reason);
    }
};
PlayerApply player_apply;
apply::Loop<PlayerApply> player_loop;
session::PlaceWatch place_watch;  // menus or galaxy, on both PCs

// Leaves a gone host's galaxy for the menus as the galaxy menu's Quit does, minus the save prompt.
constexpr char player_leave_query[] =
    "assert(type(SetState) == 'function' and type(ScriptCB_ClearMetagameState) == 'function')\n"
    "if type(ScriptCB_ClearCampaignState) == 'function' then ScriptCB_ClearCampaignState() end\n"
    "ScriptCB_ClearMetagameState()\n"
    "if type(ScriptCB_ClearMissionSetup) == 'function' then ScriptCB_ClearMissionSetup() end\n"
    "if type(ScriptCB_SetGameRules) == 'function' then ScriptCB_SetGameRules('instantaction') end\n"
    "SetState('shell')\n"
    "return 6217431\n";
unsigned host_left_seen = 0;
bool leave_pending = false;

// The friend left: the galaxy menu's confirmed Quit, which offers to save and then returns to the
// shell. Waits while a popup is open.
constexpr char host_quit_query[] =
    "if type(ScriptCB_IsPopupOpen) == 'function' and ScriptCB_IsPopupOpen() then return 0 end\n"
    "assert(type(ifs_freeform_main) == 'table' and type(ifs_freeform_main.PromptSave) == 'function')\n"
    "assert(type(ifs_freeform_menu) == 'table' and type(ScriptCB_PushScreen) == 'function')\n"
    "ifs_freeform_menu.QuitRequested = 1\n"
    "ScriptCB_PushScreen('ifs_freeform_menu')\n"
    "ifs_freeform_main:PromptSave(1)\n"
    "return 6217431\n";
// Versus: the same, from the fleet screen only. A save made over a battle, result, summary or end
// screen would reopen it on this PC only.
constexpr char versus_quit_query[] =
    "if type(ScriptCB_IsPopupOpen) == 'function' and ScriptCB_IsPopupOpen() then return 0 end\n"
    "assert(type(ScriptCB_IsScreenInStack) == 'function')\n"
    "for _, screen in ipairs({'ifs_freeform_battle', 'ifs_freeform_battle_mode', 'ifs_freeform_battle_card',\n"
    "    'ifs_freeform_result', 'ifs_freeform_summary', 'ifs_freeform_end'}) do\n"
    "  if ScriptCB_IsScreenInStack(screen) then return 0 end\n"
    "end\n"
    "assert(type(ifs_freeform_main) == 'table' and type(ifs_freeform_main.PromptSave) == 'function')\n"
    "assert(type(ifs_freeform_menu) == 'table' and type(ScriptCB_PushScreen) == 'function')\n"
    "ifs_freeform_menu.QuitRequested = 1\n"
    "ScriptCB_PushScreen('ifs_freeform_menu')\n"
    "ifs_freeform_main:PromptSave(1)\n"
    "return 6217431\n";
unsigned friend_left_seen = 0, quit_tries = 0;
bool quit_pending = false;

// The other player left: offer to save and return to the menus. Waits out a battle and its network
// cleanup, as a Quit then would lead to the multiplayer screens.
static void friend_left_step(bool guards, DWORD flow, DWORD context) {
    const unsigned events = apply_progress().friend_left.load(std::memory_order_acquire);
    if (events != friend_left_seen) {
        friend_left_seen = events;
        quit_pending = true;
        quit_tries = 0;
        log("%s; offering to save, then returning to the menus",
            versus_active() && !versus_session_host() ? "FRIEND_HOST_LEFT (Versus)" : "HOST_FRIEND_LEFT");
    }
    if (!quit_pending) return;
    // Already at the menus.
    if (place_watch.place == session::Place::menus) {
        quit_pending = false;
        return;
    }
    if (guest.busy() || battles.sealed()) return;  // a battle or its cleanup is running
    const bool versus = versus_active();
    if (!guards || flow != apply::galaxy_flow || ++quit_tries % 30) return;  // every 30th safe galaxy frame
    const LuaQueryResult r =
        versus
            ? run_source(reinterpret_cast<void*>(context), native_api, versus_quit_query, sizeof(versus_quit_query) - 1)
            : run_source(reinterpret_cast<void*>(context), native_api, host_quit_query, sizeof(host_quit_query) - 1);
    if (lua_ok(r)) {
        quit_pending = false;
        log("HOST_SAVE_PROMPT shown versus=%d", versus);
    } else if (quit_tries % 900 == 0)
        log("HOST_SAVE_PROMPT waiting (a popup or another screen) versus=%d stack_ok=%d error=%s", versus,
            r.restored && r.prefix_equal, lua_error(r));
}

// The host left: nothing more it sent is applied, and its galaxy, if shown, is left for the menus.
static void host_left_step(const apply::Frame& f) {
    const unsigned events = apply_progress().host_left.load(std::memory_order_acquire);
    if (events != host_left_seen) {
        host_left_seen = events;
        const bool was_stopped = player_loop.phase == player_loop.stopped;
        player_loop.drop_pending(apply_newest_revision());
        // Also leave when a load of that galaxy is still being confirmed.
        leave_pending = player_loop.in_galaxy || player_loop.loading();
        // In Versus the save prompt leaves the galaxy instead; the Steam thread has counted it by now.
        const bool prompt =
            versus_active() &&
            (quit_pending || apply_progress().friend_left.load(std::memory_order_acquire) != friend_left_seen);
        if (prompt) leave_pending = false;
        log("APPLY_HOST_LEFT in_galaxy=%d loading=%d; nothing more from that host is applied%s", player_loop.in_galaxy,
            player_loop.loading(),
            prompt          ? ", the save prompt leaves its galaxy"
            : leave_pending ? ", leaving its galaxy for the menus"
                            : "");
        // Applying had stopped only for the co-op game that just ended; clear the reason.
        if (was_stopped) {
            apply_progress().reason.store("", std::memory_order_release);
            log("APPLY_RESUMED");
        }
    }
    // Nothing to leave once back at the menus.
    if (leave_pending && !player_loop.in_galaxy && !player_loop.loading()) leave_pending = false;
    if (!leave_pending || !player_loop.can_leave() || !f.guards || f.flow != apply::galaxy_flow || !transition_guards())
        return;
    leave_pending = false;
    const LuaQueryResult left =
        run_source(reinterpret_cast<void*>(f.context), native_api, player_leave_query, sizeof(player_leave_query) - 1);
    unsigned char flag = 0;
    const bool scheduled = read_memory(image + 0x1a563b0, &flag, 1) && flag == 1;
    log("APPLY_LEFT_GALAXY lua_ok=%d scheduled=%d stack_ok=%d error=%s", lua_ok(left), scheduled,
        left.restored && left.prefix_equal, lua_error(left));
}

// The friend's galaxy lock (player_menu.lua): galaxy_follow while the galaxy shown is the host's, so
// only the host acts in it; galaxy_stopped once applying has stopped.
bool galaxy_answer(const char* question) {
    if (!strcmp(question, "galaxy_follow")) return apply_enabled && (player_loop.in_galaxy || player_loop.loading());
    if (!strcmp(question, "galaxy_stopped")) return apply_enabled && player_loop.phase == player_loop.stopped;
    return false;
}

// Each frame once installed, on both PCs: where the game is, the leave prompts, the party rows,
// battles joined as a guest and the apply loop.
static void step_player_apply(const FrameSample& s) {
    if (!shell_ready) return;
    DWORD flow = 0, fresh = 0;
    unsigned char transition = 1, suspend = 1;
    Header h{};
    // The usual frame guards, plus the game's flow: 4 or 0 at the menus, 3 in the galaxy.
    const bool flow_read = read_memory(image + 0x3e7e9c, &flow, 4);
    const bool guards = s.readable && s.caller == reinterpret_cast<DWORD>(image) + 0x217a9c && flow_read &&
                        read_memory(image + 0x1a563b0, &transition, 1) && !transition &&
                        read_memory(image + 0x1a56fa3, &suspend, 1) && !suspend &&
                        read_memory(image + 0x1a579a0, &fresh, 4) && fresh == s.context &&
                        header(reinterpret_cast<void*>(fresh), h) && idle(h);
    // Where this game is, for the Steam thread's co-op session.
    place_watch.step(flow_read, flow, guards);
    note_place(static_cast<int>(place_watch.place));
    friend_left_step(guards, flow, fresh);
    // The party rows, at the menus and in the galaxy, whose battle notes name the friends.
    step_party(fresh, guards && flow_read && (flow == 0 || flow == 4 || flow == apply::galaxy_flow));
    if (!apply_enabled) return;
    apply::Frame f;
    f.thread = s.thread;
    f.context = fresh;
    f.flow = flow_read ? flow : 0;
    f.generation = shell_reload_count();
    f.guards = guards;
    f.now = GetTickCount64();
    player_apply.context = fresh;
    // Versus: each PC applies the other's turns into its own galaxy. A battle away from it ends
    // in_galaxy, so the same galaxy is adopted again when it returns.
    static bool versus_galaxy = false;
    static unsigned versus_applied = 0;
    if (!versus_active()) versus_galaxy = false;
    else if (versus_session_host()) versus_galaxy = true;
    else if (player_loop.applied != versus_applied && player_loop.in_galaxy) versus_galaxy = true;
    versus_applied = player_loop.applied;
    if (versus_galaxy && guards && flow_read && flow == apply::galaxy_flow && !player_loop.in_galaxy) {
        player_loop.adopt();
        log("VERSUS_GALAXY_ADOPTED host=%d", versus_session_host());
    }
    player_apply.adopted = player_loop.adopted;
    // Battles are joined from the host's galaxy between apply cycles. A stopped loop counts too, as
    // its galaxy still shows.
    const bool between_cycles =
        player_loop.in_galaxy && (player_loop.phase == player_loop.waiting || player_loop.phase == player_loop.stopped);
    step_battle_guest(s, between_cycles, f.now);
    // Versus: the defender's card pick, also shown between cycles.
    step_versus_card(fresh, guards && flow_read && flow == apply::galaxy_flow && between_cycles && !guest.busy() &&
                                !battles.sealed());
    // Applying pauses through any battle, hosted or joined, and while an offer waits to be joined.
    const bool battle_pause =
        guest.sealed() || battles.sealed() ||
        (guest.phase == battle::Guest::Phase::offered && player_loop.phase == player_loop.waiting);
    if (!battle_pause) {
        player_loop.step(f, player_apply);
        host_left_step(f);
    }
    // Publish the loop's state for the Steam thread.
    auto& progress = apply_progress();
    progress.phase.store(player_loop.phase, std::memory_order_release);
    progress.in_galaxy.store(player_loop.in_galaxy, std::memory_order_release);
    const unsigned done = player_loop.passed > player_loop.applied ? player_loop.passed : player_loop.applied;
    progress.pending.store(apply_newest_revision() > done, std::memory_order_release);
    progress.applied.store(player_apply.applied.host_revision, std::memory_order_release);
    progress.applied_offer.store(player_apply.applied.revision, std::memory_order_release);
    progress.cycles.store(player_loop.cycles, std::memory_order_release);
}
#endif

void step_lua_query(const FrameSample& s) {
#if GC_PLAYER_MENU
    // Remember the game thread; shell_probe runs only on it.
    if (s.caller == reinterpret_cast<DWORD>(image) + 0x217a9c &&
        game_thread.load(std::memory_order_relaxed) != s.thread)
        game_thread.store(s.thread, std::memory_order_release);
#endif
    // After the install attempt, only the player features run.
    if (finished) {
#if GC_CAMPAIGN_CAPTURE
        probe_rearm(s);
#endif
#if GC_PLAYER_APPLY
        step_player_apply(s);
#endif
#if GC_PLAYER_MENU
        step_battle(s);
#endif
        return;
    }
    if (GetTickCount64() > deadline) {
        finished = true;
        LuaQueryResult result;
        result.stage = -1;
        publish(result);
        return;
    }
    // The install waits for 120 stable frames at the main menu (flow 4) on one idle Lua state.
    DWORD flow = 0, fresh = 0;
    unsigned char transition = 1, suspend = 1;
    if (!s.readable || s.caller != reinterpret_cast<DWORD>(image) + 0x217a9c ||
        !read_memory(image + 0x3e7e9c, &flow, 4) || flow != 4 || !read_memory(image + 0x1a563b0, &transition, 1) ||
        transition || !read_memory(image + 0x1a56fa3, &suspend, 1) || suspend ||
        !read_memory(image + 0x1a579a0, &fresh, 4) || fresh != s.context) {
        stable_frames = 0;
        return;
    }
    Header h{};
    if (!header(reinterpret_cast<void*>(fresh), h) || !idle(h)) {
        stable_frames = 0;
        return;
    }
    if (observed_thread != s.thread || observed_context != fresh) {
        stable_frames = 0;
        observed_thread = s.thread;
        observed_context = fresh;
    }
    if (++stable_frames < 120) return;
#if GC_PLAYER
    // Check that the screens exist first; a clean "not yet" is retried later.
    if (!shell_ready) {
        const auto probe = run_source(reinterpret_cast<void*>(fresh), native_api, ready_query, sizeof(ready_query) - 1);
        ready_probes.fetch_add(1, std::memory_order_relaxed);
        stable_frames = 0;
        const bool coherent = probe.stage == 3 && probe.called && !probe.load_status && !probe.call_status &&
                              probe.restored && probe.prefix_equal;
        // A failing chunk is not retried.
        if (!coherent) {
            finished = true;
            publish(probe);
            return;
        }
        if (!probe.value_ok) return;  // screens not registered yet
        shell_ready = true;
        ready_published.store(true, std::memory_order_release);
    }
#endif
    finished = true;  // set first: no retry or re-entry after an error
    const auto result = run_lua_query(reinterpret_cast<void*>(fresh), native_api);
#if GC_CAMPAIGN_CAPTURE
    rearm.arm(shell_reload_count(), fresh, s.thread, succeeded(result));
#endif
    publish(result);
}

void report_lua_query() {
    static ULONGLONG last_report = 0;
#if GC_PLAYER
    static bool ready_logged = false;
    static ULONGLONG last_wait = 0;
    // Log the shell becoming ready once, and the wait for it every 10 s.
    if (!ready_logged && ready_published.load(std::memory_order_acquire)) {
        ready_logged = true;
        log("PLAYER_SHELL_READY probes=%u; installing campaign capture", ready_probes.load(std::memory_order_relaxed));
    } else if (!ready_logged && (!last_wait || GetTickCount64() - last_wait >= 10000)) {
        last_wait = GetTickCount64();
        log("PLAYER_WAITING_FOR_SHELL probes=%u", ready_probes.load(std::memory_order_relaxed));
    }
#endif
    const unsigned count = published_count.load(std::memory_order_acquire);
    if (!count) return;
    const auto now = GetTickCount64();
    if (last_report && now - last_report < 10000) return;
    last_report = now;  // repeated in case the log writer dropped the line
    const auto& r = results[count - 1];
    log("LUA_QUERY_RESULT stage=%d called=%d load=%d call=%d before=%d after=%d restored=%d prefix_equal=%d value_ok=%d game_tid=%lu",
        r.stage, r.called, r.load_status, r.call_status, r.before, r.after, r.restored, r.prefix_equal, r.value_ok,
        observed_thread);
}
}
