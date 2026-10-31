// The Steam layer: lobbies and invites, the session protocol with the other PCs, and battle traffic.
#include <winsock2.h>  // before <windows.h>, which would pull in winsock 1
#include <ws2tcpip.h>
#include "steam_invite.h"
#include "steam_invite_logic.h"
#include "diagnostics.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include "capture_decode.h"
#include "capture_live.h"
#include "runtime_crypto.h"
#include "session_logic.h"
#include "versus_logic.h"
#include "apply_mailbox.h"
#include "save_candidate.h"
#include "battle_net.h"

// Binds to the game's own steam_api.dll through its flat exports: cdecl, with CSteamID passed as 8
// stack bytes and returned in EDX:EAX, which ties this layer to the x86 game.
static_assert(sizeof(void*) == 4, "Steam invite layer targets the x86 game");

// Developer builds also take request, relay and offer files from the external tools and run the
// loopback TCP relay. Release builds have neither; players use the Online menu.
#define GC_TOOL_FILES GC_DEV

namespace {
using u64 = uint64_t;
namespace s = gc::steam;
namespace ss = gc::session;

// Mirrors Steamworks' CCallbackBase: same declaration order and compiler, so the vtable matches.
class CallbackBase {
public:
    CallbackBase() {
        flags = 0;
        id = 0;
    }
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, u64 call) = 0;
    int GetICallback() { return id; }
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t flags;
    int id;
};

// Callback payloads, with the SDK's 8-byte packing.
#pragma pack(push, 8)
struct LobbyCreated {
    int result;
    u64 lobby;
};
struct LobbyEnter {
    u64 lobby;
    uint32_t permissions;
    bool locked;
    uint32_t response;
};
struct LobbyChatUpdate {
    u64 lobby, changed, making;
    uint32_t state;
};
struct LobbyChatMsg {
    u64 lobby, user;
    uint8_t type;
    uint32_t chat;
};
struct JoinRequested {
    u64 lobby, friend_id;
};
struct RichPresenceJoin {
    u64 friend_id;
    char connect[256];
};
struct P2PSessionRequest {
    u64 remote;
};
struct P2PSessionConnectFail {
    u64 remote;
    uint8_t error;
};
#pragma pack(pop)
static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyEnter) == 24 && sizeof(LobbyChatUpdate) == 32 &&
                  sizeof(LobbyChatMsg) == 24 && sizeof(JoinRequested) == 16 && sizeof(RichPresenceJoin) == 264 &&
                  sizeof(P2PSessionRequest) == 8 && sizeof(P2PSessionConnectFail) == 16,
              "Steam callback layouts (pack 8)");

// The steam_api exports this layer calls, bound by name in bind_exports.
struct Api {
    int(__cdecl* pipe)();
    int(__cdecl* user)();
    void*(__cdecl* client)();
    void*(__cdecl* user_if)(void*, int, int, const char*);
    void*(__cdecl* friends_if)(void*, int, int, const char*);
    void*(__cdecl* utils_if)(void*, int, const char*);
    void*(__cdecl* mm_if)(void*, int, int, const char*);
    void*(__cdecl* net_if)(void*, int, int, const char*);
    bool(__cdecl* p2p_send)(void*, u64, const void*, uint32_t, int, int);
    bool(__cdecl* p2p_available)(void*, uint32_t*, int);
    bool(__cdecl* p2p_read)(void*, void*, uint32_t, uint32_t*, u64*, int);
    bool(__cdecl* p2p_accept)(void*, u64);
    bool(__cdecl* p2p_close)(void*, u64);
    u64(__cdecl* steam_id)(void*);
    unsigned(__cdecl* app_id)(void*);
    bool(__cdecl* overlay)(void*);
    bool(__cdecl* call_done)(void*, u64, bool*);
    u64(__cdecl* create_lobby)(void*, int, int);
    u64(__cdecl* join_lobby)(void*, u64);
    void(__cdecl* leave_lobby)(void*, u64);
    bool(__cdecl* set_data)(void*, u64, const char*, const char*);
    const char*(__cdecl* get_data)(void*, u64, const char*);
    bool(__cdecl* send_chat)(void*, u64, const void*, int);
    int(__cdecl* chat_entry)(void*, u64, int, u64*, void*, int, int*);
    int(__cdecl* members)(void*, u64);
    bool(__cdecl* joinable)(void*, u64, bool);
    u64(__cdecl* lobby_owner)(void*, u64);
    bool(__cdecl* member_limit)(void*, u64, int);
    bool(__cdecl* set_presence)(void*, const char*, const char*);
    void(__cdecl* clear_presence)(void*);
    const char*(__cdecl* friend_name)(void*, u64);
    void(__cdecl* invite_dialog)(void*, u64);
    void(__cdecl* run)();
    void(__cdecl* reg)(CallbackBase*, int);
    void(__cdecl* reg_result)(CallbackBase*, u64);
    void(__cdecl* unreg_result)(CallbackBase*, u64);
};
Api api{};
// Interfaces created in steam_thread, at the versions the game requests.
void *user_if = nullptr, *friends_if = nullptr, *utils_if = nullptr, *mm_if = nullptr, *net_if = nullptr;

// Binds one export by name, logging it if missing.
template <class T> bool bind(HMODULE dll, T& out, const char* name) {
    out = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!out) gc::log("STEAM_INVITE missing export %s", name);
    return out != nullptr;
}

// Binds every export this layer uses; false if any is missing.
bool bind_exports(HMODULE dll) {
    bool ok = true;
    ok &= bind(dll, api.pipe, "SteamAPI_GetHSteamPipe");
    ok &= bind(dll, api.user, "SteamAPI_GetHSteamUser");
    ok &= bind(dll, api.client, "SteamClient");
    ok &= bind(dll, api.user_if, "SteamAPI_ISteamClient_GetISteamUser");
    ok &= bind(dll, api.friends_if, "SteamAPI_ISteamClient_GetISteamFriends");
    ok &= bind(dll, api.utils_if, "SteamAPI_ISteamClient_GetISteamUtils");
    ok &= bind(dll, api.mm_if, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    ok &= bind(dll, api.net_if, "SteamAPI_ISteamClient_GetISteamNetworking");
    ok &= bind(dll, api.p2p_send, "SteamAPI_ISteamNetworking_SendP2PPacket");
    ok &= bind(dll, api.p2p_available, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    ok &= bind(dll, api.p2p_read, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    ok &= bind(dll, api.p2p_accept, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    ok &= bind(dll, api.p2p_close, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    ok &= bind(dll, api.steam_id, "SteamAPI_ISteamUser_GetSteamID");
    ok &= bind(dll, api.app_id, "SteamAPI_ISteamUtils_GetAppID");
    ok &= bind(dll, api.overlay, "SteamAPI_ISteamUtils_IsOverlayEnabled");
    ok &= bind(dll, api.call_done, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= bind(dll, api.create_lobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    ok &= bind(dll, api.join_lobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    ok &= bind(dll, api.leave_lobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    ok &= bind(dll, api.set_data, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= bind(dll, api.get_data, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= bind(dll, api.send_chat, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
    ok &= bind(dll, api.chat_entry, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
    ok &= bind(dll, api.members, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    ok &= bind(dll, api.joinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    // GetLobbyOwner returns its CSteamID in EDX:EAX, like GetSteamID.
    ok &= bind(dll, api.lobby_owner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    ok &= bind(dll, api.member_limit, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit");
    ok &= bind(dll, api.set_presence, "SteamAPI_ISteamFriends_SetRichPresence");
    ok &= bind(dll, api.clear_presence, "SteamAPI_ISteamFriends_ClearRichPresence");
    ok &= bind(dll, api.friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= bind(dll, api.invite_dialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    ok &= bind(dll, api.run, "SteamAPI_RunCallbacks");
    ok &= bind(dll, api.reg, "SteamAPI_RegisterCallback");
    ok &= bind(dll, api.reg_result, "SteamAPI_RegisterCallResult");
    ok &= bind(dll, api.unreg_result, "SteamAPI_UnregisterCallResult");
    return ok;
}

// The game pumps callbacks only in its sign-in wait. Every RunCallbacks caller holds pump_lock, and
// while the game awaits its own call result only it pumps, so its handler runs on its thread.
CRITICAL_SECTION pump_lock;
u64 game_call = 0;  // the game's call result in flight
ULONGLONG game_call_since = 0;
bool game_call_timeout_logged = false;
void(__cdecl* game_run)() = nullptr;
void(__cdecl* game_reg_result)(CallbackBase*, u64) = nullptr;
void(__cdecl* game_unreg_result)(CallbackBase*, u64) = nullptr;
constexpr ULONGLONG game_call_limit = 300000;  // ms, after which this layer pumps anyway

// A callback copied out of RunCallbacks, handled later on the Steam thread.
struct Event {
    int id;
    bool io;
    u64 call;
    int size;
    unsigned char data[272];
};
std::vector<Event> events;  // guarded by pump_lock

// Runs inside RunCallbacks, so pump_lock is held. Keeps at most 64 events between pumps.
void queue_event(int id, const void* param, int size, bool io, u64 call) {
    Event e{};
    e.id = id;
    e.io = io;
    e.call = call;
    e.size = size < static_cast<int>(sizeof e.data) ? size : static_cast<int>(sizeof e.data);
    if (param && e.size > 0) memcpy(e.data, param, e.size);
    if (events.size() < 64) events.push_back(e);
}

// Queues each callback it receives for the Steam thread.
class Listener final : public CallbackBase {
public:
    Listener(int callback, int bytes) : size(bytes) { id = callback; }
    void Run(void* param) override { queue_event(id, param, size, false, 0); }
    void Run(void* param, bool io_failure, u64 call) override { queue_event(id, param, size, io_failure, call); }
    int GetCallbackSizeBytes() override { return size; }

private:
    int size;
};
// on_created and on_entered receive call results; the others are registered as callbacks.
Listener on_join_requested(s::k_lobby_join_requested, sizeof(JoinRequested));
Listener on_presence_join(s::k_rich_presence_join, sizeof(RichPresenceJoin));
Listener on_chat_update(s::k_lobby_chat_update, sizeof(LobbyChatUpdate));
Listener on_chat_msg(s::k_lobby_chat_msg, sizeof(LobbyChatMsg));
Listener on_created(s::k_lobby_created, sizeof(LobbyCreated));
Listener on_entered(s::k_lobby_enter, sizeof(LobbyEnter));
Listener on_p2p_request(s::k_p2p_session_request, sizeof(P2PSessionRequest));
Listener on_p2p_fail(s::k_p2p_connect_fail, sizeof(P2PSessionConnectFail));

// Wrappers for the game's steam_api imports: each takes pump_lock and tracks the game's call result.
void __cdecl hooked_run() {
    EnterCriticalSection(&pump_lock);
    bool failed = false;
    // A result already complete is dispatched by this run, which ends the game's wait.
    const bool done = game_call && api.call_done(utils_if, game_call, &failed);
    game_run();
    if (done) game_call = 0;
    LeaveCriticalSection(&pump_lock);
}
void __cdecl hooked_reg_result(CallbackBase* callback, u64 call) {
    EnterCriticalSection(&pump_lock);
    game_call = call;
    game_call_since = GetTickCount64();
    game_call_timeout_logged = false;
    game_reg_result(callback, call);
    LeaveCriticalSection(&pump_lock);
}
void __cdecl hooked_unreg_result(CallbackBase* callback, u64 call) {
    EnterCriticalSection(&pump_lock);
    if (call == game_call) game_call = 0;
    game_unreg_result(callback, call);
    LeaveCriticalSection(&pump_lock);
}

// Points the game's RunCallbacks, RegisterCallResult and UnregisterCallResult imports at the wrappers.
bool hook_game_imports(HMODULE game) {
    auto base = reinterpret_cast<unsigned char*>(game);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    void** run = nullptr;
    void** reg = nullptr;
    void** unreg = nullptr;
    for (DWORD off = 0; dir.VirtualAddress && off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.Size;
         off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress + off);
        if (!desc->Name) break;
        if (desc->Name >= size || _stricmp(reinterpret_cast<char*>(base + desc->Name), "steam_api.dll")) continue;
        if (!desc->OriginalFirstThunk || desc->OriginalFirstThunk >= size || desc->FirstThunk >= size) continue;
        auto names = reinterpret_cast<DWORD*>(base + desc->OriginalFirstThunk);
        for (DWORD i = 0; names[i]; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[i]) || names[i] + 2 >= size) continue;
            const char* name = reinterpret_cast<char*>(base + names[i] + 2);
            auto slot = reinterpret_cast<void**>(base + desc->FirstThunk + 4 * i);
            if (!strcmp(name, "SteamAPI_RunCallbacks")) run = slot;
            else if (!strcmp(name, "SteamAPI_RegisterCallResult")) reg = slot;
            else if (!strcmp(name, "SteamAPI_UnregisterCallResult")) unreg = slot;
        }
    }
    if (!run || !reg || !unreg) {
        gc::log("STEAM_INVITE REJECT steam_api import slots not found");
        return false;
    }
    // Publish the originals before the game can enter a wrapper.
    game_run = reinterpret_cast<decltype(game_run)>(*run);
    game_reg_result = reinterpret_cast<decltype(game_reg_result)>(*reg);
    game_unreg_result = reinterpret_cast<decltype(game_unreg_result)>(*unreg);
    void** slots[3] = {reg, unreg, run};  // run last, once the call-result wrappers are in
    void* hooks[3] = {reinterpret_cast<void*>(&hooked_reg_result), reinterpret_cast<void*>(&hooked_unreg_result),
                      reinterpret_cast<void*>(&hooked_run)};
    for (int i = 0; i < 3; ++i) {
        DWORD protection, unused;
        if (!VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &protection)) {
            gc::log("STEAM_INVITE IAT protection change failed: %lu", GetLastError());
            return false;
        }
        InterlockedExchangePointer(slots[i], hooks[i]);
        VirtualProtect(slots[i], sizeof(void*), protection, &unused);
    }
    gc::log("STEAM_INVITE IAT RunCallbacks/RegisterCallResult/UnregisterCallResult serialised");
    return true;
}

// Runs Steam callbacks unless the game is waiting on its own call result (for up to
// game_call_limit), and returns the events they queued.
std::vector<Event> pump() {
    std::vector<Event> taken;
    EnterCriticalSection(&pump_lock);
    const ULONGLONG now = GetTickCount64();
    const bool game_waiting = game_call && now - game_call_since < game_call_limit;
    if (game_call && !game_waiting && !game_call_timeout_logged) {
        gc::log("STEAM_INVITE game call result outstanding for %llu ms; resuming pump", now - game_call_since);
        game_call_timeout_logged = true;
    }
    if (!game_waiting) api.run();
    taken.swap(events);
    LeaveCriticalSection(&pump_lock);
    return taken;
}

// This process's files in the status folder: <stem>-<pid><ext>.
std::wstring dir;
DWORD pid = 0;
std::wstring path_for(const wchar_t* stem, const wchar_t* ext) {
    return dir + L"\\" + stem + L"-" + std::to_wstring(pid) + ext;
}

#if GC_TOOL_FILES
// Reads a small file whole, trimming trailing newlines and spaces. False if missing or over limit.
bool read_small(const std::wstring& path, std::string& out, DWORD limit) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= limit;
    if (ok) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    while (ok && !out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return ok;
}
#endif

// Writes a temporary file and moves it over `path`, so readers never see a partial file.
bool write_replace(const std::wstring& path, const std::string& text) {
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok =
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(f);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// State owned by the Steam thread.
s::Status st;
std::string last_written;  // the status body last written
#if GC_TOOL_FILES
std::string offer_file_text;  // the offer file as last read
#endif
u64 pending_call = 0;     // CreateLobby or JoinLobby in flight
bool lobby_lost = false;  // Steam has dropped this PC from its lobby
// Host: lobby_open is the joinable state last sent to Steam. Once the co-op game has started, the galaxy
// announces friends who leave or join.
bool lobby_started = false, lobby_open = true;
unsigned note_seq = 0;  // the last party note's number

// A PC this one pairs with: on the host each friend, keeping its entry while it stays; on a friend's
// PC its host, in peers[0]. Only paired peers' frames, tunnel packets and P2P sessions count.
struct Peer {
    u64 id = 0;
    std::string name, build, offer;
    bool build_match = false, offer_valid = false;
    ULONGLONG left_at = 0;     // when it left the lobby
    ULONGLONG name_retry = 0;  // the next persona name lookup
    ss::Sender out;            // what this PC sends it
    ULONGLONG hello_at = 0;    // host: the last H sent to it
    ULONGLONG heard_at = 0;    // its last valid frame
    // As its battle host: the battle to announce (F), the first and newest announced, and the last
    // result (V) and end (E) sent.
    unsigned battle = 0, battle_first = 0, battle_sent = 0, result_sent = 0, over_sent = 0;
    // Host: in_start if the Start hold includes it, shown once its game shows the galaxy. Shown without
    // in_start means it is joining a game in progress.
    bool in_start = false, shown = false;
};
std::array<Peer, s::max_friends> peers;

// Paired once its HELLO has matched this PC's.
bool paired(const Peer& p) { return p.id && p.build_match; }
// The friend's number in party rows and logs: 1 to 3 on the host, 0 on a friend's PC.
unsigned index_of(const Peer& p) { return st.role == "host" ? static_cast<unsigned>(&p - peers.data()) + 1 : 0; }
// A peer's session slot, 1 to 3 in any role: its battle address and tunnel tag (a friend's host is 1).
unsigned slot_of(const Peer& p) { return static_cast<unsigned>(&p - peers.data()) + 1; }
// Looks a peer up by SteamID; find_paired returns it only if it is paired.
Peer* find_peer(u64 user) {
    for (Peer& p : peers)
        if (user && p.id == user) return &p;
    return nullptr;
}
Peer* find_paired(u64 user) {
    Peer* p = find_peer(user);
    return p && paired(*p) ? p : nullptr;
}
// The first paired peer: a friend's host, or the host's friend in the lowest slot. The developer
// relay and Versus only ever use this one.
Peer* first_paired() {
    for (Peer& p : peers)
        if (paired(p)) return &p;
    return nullptr;
}
unsigned paired_count() {
    unsigned n = 0;
    for (const Peer& p : peers) n += paired(p) ? 1 : 0;
    return n;
}
// " friend=N" for the host's per-friend log lines; empty on a friend's PC.
std::string tag(const Peer& p) { return st.role == "host" ? " friend=" + std::to_string(index_of(p)) : std::string(); }

void relay_peer_gone(u64 who);
void forget(Peer& p);
void session_reset();
bool prompt_left(bool session_host);
void host_gone(const char* why);
void friend_left(Peer& p, const char* why);
void end_coop(const char* why);
// Forgets the lobby, peers, sessions and notes, and moves to `state`.
void reset_session(const char* state) {
    relay_peer_gone(0);
    session_reset();
    st.state = state;
    st.lobby = 0;
    st.members = 0;
    for (Peer& p : peers) p = Peer{};
    st.notes.clear();
    st.offer_sent = false;
    st.mode = "coop";
    pending_call = 0;
    lobby_lost = false;
    lobby_started = false;
    lobby_open = true;
}

// Records an error for the status file and logs it.
void fail(const char* error) {
    st.error = error;
    gc::log("STEAM_INVITE_ERROR %s", error);
}

std::string hello_text = s::hello_body(false);  // set at startup (start_steam_invite)

// Sends a HELLO or OFFER to the lobby chat, terminator included.
bool send(s::Kind kind, const std::string& body) {
    const std::string text = s::encode_message(kind, body);
    return st.lobby && api.send_chat(mm_if, st.lobby, text.c_str(), static_cast<int>(text.size()) + 1);
}

// Joins a lobby from an invite, a rich-presence join or the command line.
void begin_join(u64 lobby, const char* source) {
    gc::log("STEAM_JOIN_REQUEST source=%s lobby=%llu state=%s", source, lobby, st.state.c_str());
    if (st.state != "idle" && st.state != "left" && st.state != "error") {
        fail("join_ignored_session_active");
        return;
    }
    reset_session("joining");
    st.role = "guest";
    st.error.clear();
    st.session_end.clear();  // a new host
    st.lobby = lobby;
    // Register the call result under pump_lock, so no RunCallbacks can complete it unclaimed.
    EnterCriticalSection(&pump_lock);
    pending_call = api.join_lobby(mm_if, lobby);
    if (pending_call) api.reg_result(&on_entered, pending_call);
    LeaveCriticalSection(&pump_lock);
    if (!pending_call) {
        reset_session("error");
        fail("join_call_rejected");
    }
}

// Leaves the lobby, clears rich presence and resets to `state`.
void leave(const char* state) {
    if (st.lobby) api.leave_lobby(mm_if, st.lobby);
    api.clear_presence(friends_if);
    gc::log("STEAM_LOBBY_LEFT lobby=%llu state=%s", st.lobby, state);
    reset_session(state);
}

// Host: the peer for a lobby member, given the first free entry if new. Null when full or not a candidate.
Peer* note_member(u64 user) {
    if (Peer* p = find_peer(user)) return p;
    if (st.role != "host" || !s::valid_user_id(user) || user == st.self) return nullptr;
    for (Peer& p : peers)
        if (!p.id) {
            p = Peer{};
            p.id = user;
            gc::log("STEAM_PEER id=%llu%s", user, tag(p).c_str());
            return &p;
        }
    gc::log("STEAM_INVITE ignoring member %llu: the friend slots are taken", user);
    return nullptr;
}

// Friend's PC: its host is the lobby's owner, recorded once in peers[0].
Peer* guest_host() {
    if (peers[0].id) return &peers[0];
    const u64 owner = st.lobby ? api.lobby_owner(mm_if, st.lobby) : 0;
    if (!s::valid_user_id(owner) || owner == st.self) return nullptr;
    peers[0] = Peer{};
    peers[0].id = owner;
    gc::log("STEAM_PEER id=%llu (lobby owner)", owner);
    return &peers[0];
}

// Host: locks the lobby when full and reopens it below the limit, game started or not. Steam only
// hears of changes.
void update_joinable() {
    if (st.role != "host" || !st.lobby || lobby_lost) return;
    const int limit = s::lobby_limit(st.mode == "versus");
    const bool open = s::lobby_joinable(st.members, limit);
    if (open == lobby_open) return;
    lobby_open = open;
    api.joinable(mm_if, st.lobby, open);
    gc::log("STEAM_LOBBY_JOINABLE %d members=%d limit=%d started=%d", open, st.members, limit, lobby_started);
}

#if GC_TOOL_FILES
// Developer relay of the bridge's TCP stream over P2P. The host bridge connects to a loopback listener
// here; on O the target side connects to its bridge. Only the first paired peer is relayed to.
struct Relay {
    bool host = false, target = false;
    unsigned target_port = 0;
    SOCKET listener = INVALID_SOCKET, local = INVALID_SOCKET;
    uint32_t epoch = 0;  // tags the open stream's frames; 0 for none
    bool reading = false, remote_closed = false;
    std::string to_local;  // peer bytes waiting for the local socket
    ULONGLONG connecting_since = 0;
    u64 pinged = 0;  // the peer whose P2P session is open
    unsigned dropped = 0;
} relay;
constexpr size_t relay_backlog = 256 * 1024;  // stop reading P2P above this

// Sends a relay frame to the first paired peer.
bool p2p(s::Frame type, const char* data = nullptr, size_t n = 0) {
    const Peer* peer = first_paired();
    if (!peer) return false;
    const std::string f = s::encode_frame(type, type == s::Frame::ping ? 0 : relay.epoch, data, n);
    return api.p2p_send(net_if, peer->id, f.data(), static_cast<uint32_t>(f.size()), 2 /* k_EP2PSendReliable */,
                        s::relay_channel);
}

// Closes the local stream, if any, and resets the relay state.
void close_local(const char* state) {
    if (relay.local != INVALID_SOCKET) {
        shutdown(relay.local, SD_BOTH);
        closesocket(relay.local);
        relay.local = INVALID_SOCKET;
        gc::log("STEAM_RELAY_STREAM_END epoch=%08X state=%s up=%llu down=%llu", relay.epoch, state, st.relay_up,
                st.relay_down);
    }
    relay.epoch = 0;
    relay.reading = relay.remote_closed = false;
    relay.to_local.clear();
    if (!strcmp(state, "failed")) st.relay = "failed";
    else st.relay = relay.host ? "listening" : relay.target ? "ready" : "off";
}

// The relay's peer has left or its P2P session has failed (`who`, or 0 for any): its stream ends.
void relay_peer_gone(u64 who) {
    if (who && who != relay.pinged) return;
    if (relay.local != INVALID_SOCKET) close_local("failed");
    relay.pinged = 0;
}

bool set_nonblocking(SOCKET sock) {
    u_long on = 1;
    return ioctlsocket(sock, FIONBIO, &on) == 0;
}

// Applies a relay request: listen for the host bridge, or note the target bridge's port.
void configure_relay(const std::string& text) {
    bool host = false;
    unsigned port = 0;
    if (!s::parse_relay_request(text, host, port)) {
        fail("relay_request_rejected");
        return;
    }
    if (host) {
        if (relay.listener == INVALID_SOCKET) {
            SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            int len = sizeof a;
            if (l == INVALID_SOCKET || ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(l, 1) ||
                getsockname(l, reinterpret_cast<sockaddr*>(&a), &len) || !set_nonblocking(l)) {
                if (l != INVALID_SOCKET) closesocket(l);
                fail("relay_listen_failed");
                return;
            }
            relay.listener = l;
            st.relay_port = ntohs(a.sin_port);
        }
        relay.host = true;
        relay.target = false;
        st.relay = relay.local == INVALID_SOCKET ? "listening" : st.relay;
        gc::log("STEAM_RELAY host listening on 127.0.0.1:%u", st.relay_port);
    } else {
        relay.target = true;
        relay.host = false;
        relay.target_port = port;
        if (relay.local == INVALID_SOCKET) st.relay = "ready";
        gc::log("STEAM_RELAY target bridge at 127.0.0.1:%u", port);
    }
}

// Handles one relay frame from the peer.
void relay_frame(const char* p, size_t n) {
    s::Frame type;
    uint32_t epoch = 0;
    size_t payload = 0;
    if (!s::decode_frame(p, n, type, epoch, payload)) {
        ++relay.dropped;
        return;
    }
    switch (type) {
    case s::Frame::ping: return;
    case s::Frame::open: {
        if (!relay.target || !relay.target_port) {
            relay.epoch = epoch;
            p2p(s::Frame::close);
            relay.epoch = 0;
            return;
        }
        if (relay.local != INVALID_SOCKET) close_local("closed");  // a new open replaces a stale stream
        SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<u_short>(relay.target_port));
        relay.epoch = epoch;
        if (c == INVALID_SOCKET || ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) || !set_nonblocking(c)) {
            if (c != INVALID_SOCKET) closesocket(c);
            p2p(s::Frame::close);
            gc::log("STEAM_RELAY_ERROR target bridge refused 127.0.0.1:%u (%d)", relay.target_port, WSAGetLastError());
            close_local("failed");
            return;
        }
        relay.local = c;
        relay.reading = true;
        ++st.relay_streams;
        st.relay = "open";
        p2p(s::Frame::accept);
        gc::log("STEAM_RELAY_STREAM_OPEN epoch=%08X side=target", epoch);
        return;
    }
    case s::Frame::accept:
        if (relay.host && epoch == relay.epoch && st.relay == "connecting") {
            relay.reading = true;
            st.relay = "open";
            gc::log("STEAM_RELAY_STREAM_OPEN epoch=%08X side=host", epoch);
        }
        return;
    case s::Frame::data:
        if (epoch != relay.epoch || relay.local == INVALID_SOCKET) {
            ++relay.dropped;
            return;
        }
        relay.to_local.append(p + s::relay_header, payload);
        st.relay_down += payload;
        return;
    case s::Frame::close:
        if (epoch == relay.epoch && relay.local != INVALID_SOCKET) relay.remote_closed = true;
        else if (epoch == relay.epoch && st.relay == "connecting") close_local("failed");
        return;
    }
}

// Moves bytes between the local bridge socket and the peer, and opens and closes streams.
void relay_tick() {
    if (!net_if || (!relay.host && !relay.target)) return;
    const Peer* peer = first_paired();
    const bool peer_ok = peer != nullptr;
    if (peer_ok && relay.pinged != peer->id) {  // a first send also accepts its session
        if (p2p(s::Frame::ping)) relay.pinged = peer->id;
    }
    // Peer to the local buffer, paused while the local socket is backed up.
    uint32_t size = 0;
    std::vector<char> buffer;
    for (int i = 0;
         i < 64 && relay.to_local.size() < relay_backlog && api.p2p_available(net_if, &size, s::relay_channel); ++i) {
        buffer.resize(size ? size : 1);
        uint32_t got = 0;
        u64 remote = 0;
        if (!api.p2p_read(net_if, buffer.data(), static_cast<uint32_t>(buffer.size()), &got, &remote, s::relay_channel))
            break;
        if (!peer_ok || remote != peer->id) {
            ++relay.dropped;
            continue;
        }
        relay_frame(buffer.data(), got);
    }
    // Host: accept the bridge's connection and ask the target side to open.
    if (relay.host && relay.listener != INVALID_SOCKET) {
        SOCKET c = accept(relay.listener, nullptr, nullptr);
        if (c != INVALID_SOCKET) {
            if (!peer_ok || relay.local != INVALID_SOCKET || !set_nonblocking(c)) {
                closesocket(c);  // one stream at a time, only with a paired peer
            } else {
                relay.local = c;
                // Any nonzero epoch that differs between streams.
                do {
                    relay.epoch = (GetTickCount() * 2654435761u) ^
                                  (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&c)) << 7) ^ st.relay_streams;
                } while (!relay.epoch);
                relay.connecting_since = GetTickCount64();
                ++st.relay_streams;
                st.relay = "connecting";
                if (!p2p(s::Frame::open)) close_local("failed");
            }
        }
        if (st.relay == "connecting" && GetTickCount64() - relay.connecting_since > 30000) {
            gc::log("STEAM_RELAY_ERROR target side did not accept within 30 s");
            close_local("failed");
        }
    }
    if (relay.local == INVALID_SOCKET) return;
    // Local socket to the peer.
    char chunk[s::relay_max_payload];
    for (int i = 0; relay.reading && i < 8; ++i) {
        const int n = recv(relay.local, chunk, sizeof chunk, 0);
        if (n > 0) {
            if (!p2p(s::Frame::data, chunk, static_cast<size_t>(n))) {
                close_local("failed");
                return;
            }
            st.relay_up += static_cast<unsigned>(n);
        } else if (n == 0) {
            p2p(s::Frame::close);
            close_local("closed");
            return;
        } else {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                p2p(s::Frame::close);
                close_local("failed");
                return;
            }
            break;
        }
    }
    // Buffered peer bytes to the local socket.
    while (!relay.to_local.empty()) {
        const int n = ::send(relay.local, relay.to_local.data(), static_cast<int>(relay.to_local.size()), 0);
        if (n > 0) relay.to_local.erase(0, static_cast<size_t>(n));
        else {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                p2p(s::Frame::close);
                close_local("failed");
                return;
            }
            break;
        }
    }
    if (relay.remote_closed && relay.to_local.empty()) close_local("closed");
}
#else
void relay_peer_gone(u64) {}
#endif

// Snapshot session (session_logic.h). The host sends each validated capture to every friend under
// that friend's session id; a friend validates it again and hands it to its apply loop.
struct Session {
    ss::Receiver in;          // the other PC's revisions: on a friend's PC, or either PC in Versus
    gc::CaptureOffer latest;  // this PC's newest validated snapshot
    uint32_t floor = 0;       // revisions up to it belong to an earlier galaxy
    std::string received;     // the newest accepted JSON
    unsigned dropped = 0;
    ss::Hold hold;  // host: the ready check
    ss::CoopGame coop;
    ss::ShowsReport shows;  // friend: what L has told the host
    // Versus: what the other PC holds, and the turn of each recently sent revision, credited when it is
    // acknowledged.
    gc::versus::Exchange exchange;
    struct SentTurn {
        uint32_t revision = 0;
        int turn = 0;
    } sent_turns[8]{};
    unsigned sent_turn_next = 0;
} session;

// Versus state shared with the game thread, which posts this PC's picks and game end (versus_event)
// and asks for the other PC's (versus_answer). This thread sends P and G.
SRWLOCK versus_lock = SRWLOCK_INIT;
gc::versus::Sides versus_sides;                       // guarded by versus_lock
unsigned versus_over_turn = 0, versus_over_sent = 0;  // guarded: this PC's G and the last sent
uint8_t versus_over_winner = 0;                       // guarded
std::atomic<int> versus_winner{0};                    // the other PC's G: the side that won
std::atomic<bool> versus_flag{false};                 // this game is Versus, until it is back at the menus
std::atomic<bool> versus_host_flag{false};            // this PC is the session host
std::atomic<bool> versus_pair_flag{false};            // a Versus session is ready, in either role
bool versus_latch = false;                            // this thread's copy of versus_flag
// The defender's bonus card, guarded by versus_lock: card_ask is this PC's request as attacker (C)
// and its answer (D), card_peer the other PC's request to this one as defender.
gc::versus::CardAsk card_ask;
gc::versus::CardPeer card_peer;

bool versus_game() { return st.mode == "versus"; }
// The turn of a recently sent revision; 0 if unknown.
int sent_turn(uint32_t revision) {
    for (const auto& t : session.sent_turns)
        if (t.revision == revision) return t.turn;
    return 0;
}

bool session_send(const Peer& p, const ss::Frame& f);

// Battle news shared with the game thread. A battle's host announces it, its result and its end
// (F, V, E); its client reports its part (J). Battle numbers restart with each session.
SRWLOCK battle_lock = SRWLOCK_INIT;
struct BattleNews {
    unsigned announced = 0, over = 0;        // host
    std::string text;                        // host: F's payload
    unsigned session = 0;                    // client: sessions opened
    unsigned offered = 0, offered_over = 0;  // client: in the newest session
    std::string offered_text;
    unsigned part_seq = 0, part_sent = 0, part_number = 0;  // client: the newest report
    uint8_t part = 0;
    // Host: each friend's newest J by session slot, and the slots told of the newest battle (F).
    struct Report {
        unsigned seq = 0, number = 0;
        uint8_t part = 0;
    } reports[s::max_friends + 1];
    unsigned told_number = 0, told_slots = 0;
    unsigned result = 0;  // host: V
    uint32_t result_mask = 0;
    unsigned offered_result = 0;  // client: in the newest session
    uint32_t offered_mask = 0;
    // What this PC's goodbye says about the Versus battle it is in (battle_closing); 0 for none.
    unsigned closing = 0;
    bool closing_host = false;
    uint32_t closing_mask = 0;
} battle_news;
std::atomic<bool> battle_busy_flag{false};  // a shared battle runs on this PC
std::atomic<unsigned> party_friends{0};     // the co-op host's paired friends

// Battle tunnel traffic for the BATTLE_TUNNEL log line: frames and bytes each way, the largest
// frames, and frames per session slot.
struct TunnelCounts {
    unsigned out = 0, in = 0, refused = 0, unsent = 0, largest_out = 0, largest_in = 0;
    unsigned long long bytes_out = 0, bytes_in = 0;
    unsigned out_by[s::max_friends + 1]{}, in_by[s::max_friends + 1]{};
} tunnel;

// Battle host: tells each paired friend of the newest battle (F), then its result (V) and end (E).
// A friend that pairs after a battle's announcement is not told of it.
void battle_host_news() {
    unsigned number = 0, over = 0, result = 0;
    uint32_t mask = 0;
    std::string text;
    AcquireSRWLockExclusive(&battle_lock);
    number = battle_news.announced;
    text = battle_news.text;
    result = battle_news.result;
    mask = battle_news.result_mask;
    over = battle_news.over;
    const unsigned told = battle_news.told_number;
    ReleaseSRWLockExclusive(&battle_lock);
    if (number > told) {
        unsigned slots = 0;
        for (Peer& p : peers)
            if (paired(p)) {
                p.battle = number;
                slots |= 1u << slot_of(p);
            }
        if (slots) {
            AcquireSRWLockExclusive(&battle_lock);
            battle_news.told_number = number;
            battle_news.told_slots = slots;
            ReleaseSRWLockExclusive(&battle_lock);
            gc::log("SESSION_BATTLE_FRIENDS number=%u slots=%u%s%s%s", number, slots, slots & 2 ? " 1" : "",
                    slots & 4 ? " 2" : "", slots & 8 ? " 3" : "");
        }
    }
    for (Peer& p : peers) {
        if (!paired(p) || !p.out.ready || !p.battle) continue;
        if (p.battle > p.battle_sent) {
            ss::Frame f;
            f.type = ss::Type::battle;
            f.session = p.out.session;
            f.revision = p.battle;
            f.json = text;
            const bool ok = session_send(p, f);
            gc::log("SESSION_BATTLE_SENT number=%u ok=%d bytes=%u%s", p.battle, ok, static_cast<unsigned>(text.size()),
                    tag(p).c_str());
            if (ok) {
                p.battle_sent = p.battle;
                if (!p.battle_first) p.battle_first = p.battle;
            }
        }
        // V and E only for battles this peer has heard of (F).
        const auto told_of = [&p](unsigned n) {
            return n && p.battle_first && n >= p.battle_first && n <= p.battle_sent;
        };
        if (result > p.result_sent && told_of(result)) {
            ss::Frame f;
            f.type = ss::Type::battle_result;
            f.session = p.out.session;
            f.revision = result;
            f.result = mask;
            const bool ok = session_send(p, f);
            gc::log("SESSION_BATTLE_RESULT_SENT number=%u mask=%08X ok=%d%s", result, mask, ok, tag(p).c_str());
            if (ok) p.result_sent = result;
        }
        if (over > p.over_sent && told_of(over)) {
            ss::Frame f;
            f.type = ss::Type::battle_over;
            f.session = p.out.session;
            f.revision = over;
            const bool ok = session_send(p, f);
            gc::log("SESSION_BATTLE_OVER_SENT number=%u ok=%d%s", over, ok, tag(p).c_str());
            if (ok) p.over_sent = over;
        }
    }
}

// Battle client: sends its newest part report (J) once the session is open.
void battle_guest_news() {
    const Peer* peer = first_paired();
    if (!peer || !session.in.session) return;
    unsigned seq = 0, number = 0;
    uint8_t part = 0;
    AcquireSRWLockExclusive(&battle_lock);
    if (battle_news.part_seq != battle_news.part_sent) {
        seq = battle_news.part_seq;
        number = battle_news.part_number;
        part = battle_news.part;
    }
    ReleaseSRWLockExclusive(&battle_lock);
    if (!seq) return;
    ss::Frame f;
    f.type = ss::Type::battle_part;
    f.session = session.in.session;
    f.revision = number;
    f.part = part;
    const bool ok = session_send(*peer, f);
    static unsigned failed = 0;  // a failed send retries every tick; logged once
    if (ok || failed != seq)
        gc::log("SESSION_BATTLE_PART_SENT number=%u part=%s ok=%d", number,
                gc::battle::part_name(gc::battle::part_from(part)), ok);
    if (!ok) {
        failed = seq;
        return;
    }
    AcquireSRWLockExclusive(&battle_lock);
    battle_news.part_sent = seq;
    ReleaseSRWLockExclusive(&battle_lock);
}

// Drops the announcements (F, E) of a session that has ended, so none is joined later. The result
// (V) stays until the next session: a closing battle host sends V just before B.
void battle_guest_forget() {
    AcquireSRWLockExclusive(&battle_lock);
    battle_news.offered = battle_news.offered_over = 0;
    battle_news.offered_text.clear();
    ReleaseSRWLockExclusive(&battle_lock);
}

// Exit to Windows inside a Versus battle forfeits it: as its host this PC sends V with the other
// team's win, as its client J quit. Sent before B on the same reliable channel, so it arrives first.
void battle_goodbye() {
    if (!versus_game()) return;
    AcquireSRWLockExclusive(&battle_lock);
    const unsigned number = battle_news.closing;
    const bool host = battle_news.closing_host;
    const uint32_t mask = battle_news.closing_mask;
    ReleaseSRWLockExclusive(&battle_lock);
    if (!number || (host && !mask)) return;
    if (host) gc::battle_host_result(number, mask);
    else gc::battle_guest_report(number, gc::battle::Part::quit);
    if (host) battle_host_news();
    else battle_guest_news();
    gc::log("BATTLE_GOODBYE number=%u %s", number,
            host ? "as its host: the other team's win (V)" : "as its client: its player quit (J)");
}

// Battle tunnel: the game's sealed LAN packets to and from the paired peers on P2P channel 9, each
// peer addressed by its session slot.
void tunnel_tick() {
    if (!net_if) return;
    // Paired peers by session slot: the host's friends, or a friend's host.
    unsigned slots = 0;
    if (st.role == "host" || st.role == "guest")
        for (const Peer& p : peers)
            if (paired(p)) slots |= 1u << slot_of(p);
    gc::battle_tunnel_peer(slots ? (st.role == "host" ? 1 : 2) : 0);
    gc::battle_tunnel_peers(slots);
    party_friends.store(st.role == "host" ? paired_count() : 0, std::memory_order_release);
    // The first frame each way is logged at once, in case the run ends before the next report.
    static bool told_in = false, told_out = false;
    static std::vector<char> buffer;
    uint32_t size = 0;
    for (int i = 0; i < 512 * s::max_friends && api.p2p_available(net_if, &size, gc::battle::tunnel_channel); ++i) {
        buffer.resize(size ? size : 1);
        uint32_t got = 0;
        u64 remote = 0;
        if (!api.p2p_read(net_if, buffer.data(), static_cast<uint32_t>(buffer.size()), &got, &remote,
                          gc::battle::tunnel_channel))
            break;
        const Peer* from = slots ? find_paired(remote) : nullptr;
        if (from && gc::battle_tunnel_put(buffer.data(), got, slot_of(*from))) {
            ++tunnel.in;
            ++tunnel.in_by[slot_of(*from)];
            tunnel.bytes_in += got;
            if (got > tunnel.largest_in) tunnel.largest_in = got;
            if (!told_in) {
                told_in = true;
                gc::log("BATTLE_TUNNEL_FIRST in bytes=%u slot=%u", got, slot_of(*from));
            }
        } else ++tunnel.refused;
    }
    std::string frame;
    unsigned to = 0;
    for (int i = 0; i < 512 * s::max_friends && gc::battle_tunnel_take(frame, &to); ++i) {
        // One unreliable packet holds up to 1200 bytes; larger frames go reliably. Each goes to the slot it
        // is addressed to, or to every peer.
        const int type = frame.size() <= 1200 ? 0 /* k_EP2PSendUnreliable */ : 2 /* k_EP2PSendReliable */;
        bool any = false;
        for (const Peer& p : peers) {
            const unsigned slot = slot_of(p);
            if (!(slots & (1u << slot)) || (to != gc::battle::all_peers && to != slot)) continue;
            if (!api.p2p_send(net_if, p.id, frame.data(), static_cast<uint32_t>(frame.size()), type,
                              gc::battle::tunnel_channel))
                continue;
            any = true;
            ++tunnel.out;
            ++tunnel.out_by[slot];
            tunnel.bytes_out += frame.size();
            if (frame.size() > tunnel.largest_out) tunnel.largest_out = static_cast<unsigned>(frame.size());
            if (!told_out) {
                told_out = true;
                gc::log("BATTLE_TUNNEL_FIRST out bytes=%u slot=%u", static_cast<unsigned>(frame.size()), slot);
            }
        }
        if (!any) ++tunnel.unsent;
    }
    static ULONGLONG reported = 0;
    static bool was_sealed = false;
    const bool now_sealed = gc::battle_sealed();
    if (!now_sealed) told_in = told_out = false;
    const ULONGLONG now = GetTickCount64();
    if ((now_sealed && now - reported >= 10000) || now_sealed != was_sealed) {
        reported = now;
        // The peer's battle role; in Versus the session host may be the client.
        const int role = gc::battle_tunnel_peer_role();
        // Frames out/in per slot; stray counts packets sent to an address no peer has.
        std::string by_slot;
        for (unsigned k = 1; k <= s::max_friends; ++k)
            if (tunnel.out_by[k] || tunnel.in_by[k])
                by_slot += (by_slot.empty() ? "" : ",") + std::to_string(k) + ":" + std::to_string(tunnel.out_by[k]) +
                           "/" + std::to_string(tunnel.in_by[k]);
        gc::log(
            "BATTLE_TUNNEL sealed=%d peer=%s frames_out=%u frames_in=%u refused=%u unsent=%u bytes_out=%llu bytes_in=%llu largest_out=%u largest_in=%u slots=%s stray=%u",
            now_sealed,
            role == 1   ? (versus_game() ? "client" : "friend")
            : role == 2 ? "host"
                        : "none",
            tunnel.out, tunnel.in, tunnel.refused, tunnel.unsent, tunnel.bytes_out, tunnel.bytes_in, tunnel.largest_out,
            tunnel.largest_in, by_slot.empty() ? "-" : by_slot.c_str(), gc::battle_seal_counts().stray);
    }
    was_sealed = now_sealed;
}

// The ready check across threads: the game thread posts popup events and Start (hold_event, menu_event)
// and reads hold_published and hold_loading; the Hold itself lives on this thread.
enum HoldEvent : unsigned {
    hold_shown = 1,
    hold_cancelled = 2,
    hold_popup_released = 4,
    hold_popup_timeout = 8,
    hold_started = 16
};
std::atomic<bool> hold_published{false};
std::atomic<unsigned> hold_events{0};
std::atomic<unsigned> hold_loading{0};
bool holding() {
    return session.hold.state == ss::Hold::State::waiting || session.hold.state == ss::Hold::State::shown;
}

// Start on the co-op party screen: hold the galaxy for every friend whose session is ready. A friend
// still joining is not waited for; it joins the game in progress.
void hold_start() {
    uint32_t ids[s::max_friends]{};
    size_t n = 0;
    for (Peer& p : peers)
        if (paired(p) && p.out.ready && n < s::max_friends) {
            ids[n++] = p.out.session;
            p.in_start = true;
        }
    const bool held = st.role == "host" && !versus_game() && session.hold.start(ids, n);
    gc::log("MENU_START friends=%u held=%d", static_cast<unsigned>(n), held);
    if (held) gc::log("HOLD_WAITING id=%08X friends=%u", session.hold.session, session.hold.friends);
}

// Applies the game thread's hold events and publishes whether the galaxy is held.
void hold_tick(ULONGLONG now) {
    const unsigned arrived = hold_events.exchange(0, std::memory_order_acq_rel);
    if (arrived & hold_started) hold_start();
    if (arrived & hold_shown) {
        session.hold.shown(now);
        if (session.hold.state == ss::Hold::State::shown && session.hold.friends > 1)
            gc::log("HOLD_SHOWN id=%08X the host's galaxy waits for %u friends' games", session.hold.session,
                    session.hold.loading());
        else if (session.hold.state == ss::Hold::State::shown)
            gc::log("HOLD_SHOWN id=%08X the host's galaxy waits for the friend's game", session.hold.session);
    }
    if ((arrived & hold_cancelled) && session.hold.release("the host cancelled the wait"))
        gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
    if ((arrived & hold_popup_timeout) && session.hold.release("the popup timed out"))
        gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
    const bool was = session.hold.state == ss::Hold::State::shown;
    const bool wanted = st.role == "host" && session.hold.wanted(now);
    if (was && session.hold.state == ss::Hold::State::done)
        gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
    if (arrived & hold_popup_released) gc::log("HOLD_POPUP_CLOSED id=%08X", session.hold.session);
    hold_loading.store(wanted ? session.hold.loading() : 0, std::memory_order_release);
    hold_published.store(wanted, std::memory_order_release);
}

// Session ids only need to differ (Steam authenticates the peers): clock, process and a counter
// hashed with SHA-256. Windows CNG is avoided because its first use can load a DLL.
uint32_t random_id() {
    static std::atomic<uint32_t> count{0};
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const struct {
        LONGLONG counter;
        ULONGLONG ticks;
        DWORD process, thread;
        uint32_t count;
        const void* stack;
    } seed{counter.QuadPart,     GetTickCount64(),       GetCurrentProcessId(),
           GetCurrentThreadId(), count.fetch_add(1) + 1, &counter};
    const std::array<unsigned char, 32> digest = gc::sha256(&seed, sizeof seed);
    uint32_t id = 0;
    memcpy(&id, digest.data(), sizeof id);
    return id ? id : 1;
}

// Lowercase hex of a digest.
std::string hex(const ss::Digest& d) { return gc::request_hex(d.data(), static_cast<unsigned>(d.size())); }

// Parses 64 hex digits into a digest; false unless the text is exactly its lowercase hex.
bool digest_of(const std::string& text, ss::Digest& d) {
    if (text.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned byte = 0;
        if (sscanf_s(text.c_str() + 2 * i, "%2x", &byte) != 1) return false;
        d[i] = static_cast<uint8_t>(byte);
    }
    return hex(d) == text;
}

// Sends a session frame reliably to a paired peer.
bool session_send(const Peer& p, const ss::Frame& f) {
    const std::string bytes = ss::encode(f);
    return paired(p) && api.p2p_send(net_if, p.id, bytes.data(), static_cast<uint32_t>(bytes.size()),
                                     2 /* k_EP2PSendReliable */, ss::channel);
}

// The session id used with this peer: its own on the host, the host's on a friend's PC.
uint32_t session_id(const Peer& p) { return st.role == "host" ? p.out.session : session.in.session; }
bool session_active() {
    if (session.in.session) return true;
    for (const Peer& p : peers)
        if (p.out.session) return true;
    return false;
}

// Host: ends one friend's session while the others go on; the ready check stops waiting for it.
void end_friend_session(Peer& p) {
    const uint32_t id = p.out.session;
    if (id) gc::log("SESSION_CLOSED id=%08X%s", id, tag(p).c_str());
    const unsigned loading = session.hold.loading();
    if (id && session.hold.on_left(id))
        gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
    else if (session.hold.loading() < loading && holding())
        gc::log("HOLD_FRIEND id=%08X%s left; %u friend(s) still loading", session.hold.session, tag(p).c_str(),
                session.hold.loading());
    p.out = ss::Sender{};
    p.hello_at = p.heard_at = 0;
}

// Ends every session, and clears the Versus and battle state tied to them.
void session_reset() {
    if (st.role == "host")
        for (Peer& p : peers) end_friend_session(p);
    else {
        if (session.in.session) gc::log("SESSION_CLOSED id=%08X", session.in.session);
        for (Peer& p : peers) {
            p.out = ss::Sender{};
            p.hello_at = p.heard_at = 0;
        }
    }
    session.in = ss::Receiver{};
    if (session.hold.release("the friend left"))
        gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
    hold_published.store(false, std::memory_order_release);
    session.shows = ss::ShowsReport{};
    session.exchange.reset();
    for (auto& t : session.sent_turns) t = Session::SentTurn{};
    AcquireSRWLockExclusive(&versus_lock);
    versus_sides.reset();
    versus_over_turn = versus_over_sent = 0;
    versus_over_winner = 0;
    card_ask = gc::versus::CardAsk{};
    card_peer.reset();
    ReleaseSRWLockExclusive(&versus_lock);
    versus_winner.store(0, std::memory_order_release);
    st.session = "off";
    // An earlier session's battle report never reaches the next host, and nothing it has announced is joined.
    AcquireSRWLockExclusive(&battle_lock);
    battle_news.part_sent = battle_news.part_seq;
    ReleaseSRWLockExclusive(&battle_lock);
    battle_guest_forget();
}

// Friend's PC: tells the game thread the host has gone. Battle news goes first, so a game thread
// that sees host_left never takes that session's announcements.
void count_host_left() {
    battle_guest_forget();
    gc::apply_progress().host_left.fetch_add(1, std::memory_order_acq_rel);
}

// The other PC's snapshot (S): validated, offered to the apply loop if new, and acknowledged (K).
void peer_snapshot(const Peer& from, const ss::Frame& f) {
    const auto verdict = session.in.classify(f.session, f.revision, f.digest);
    if (verdict == ss::Receiver::Verdict::other_session) {
        ++session.dropped;
        return;
    }
    ss::Frame reply;
    reply.type = ss::Type::ack;
    reply.session = f.session;
    reply.revision = f.revision;
    reply.digest = session.in.digest;
    switch (verdict) {
    case ss::Receiver::Verdict::accept: {
        gc::capture::Data data;
        std::string reason = hex(f.digest) == gc::capture::sha256_hex(f.json)
                                 ? std::string()
                                 : std::string("digest does not match the snapshot");
        if (reason.empty()) reason = gc::capture::parse_data(f.json, static_cast<int>(f.meta.producer), data);
        reply.digest = f.digest;
        if (reason.empty()) {
            session.in.commit(f.revision, f.digest);
            session.received = f.json;
            reply.ack = ss::Ack::accepted;
            int active = 0, turn = 0, side = 0;
            gc::turn_facts(data, active, turn, side);
            if (versus_game()) session.exchange.received(turn);
            gc::log(
                "SESSION_RECEIVED revision=%u generation=%u sequence=%u bytes=%u turn=%d active=%d normalized_sha256=%s",
                f.revision, f.meta.generation, f.meta.sequence, static_cast<unsigned>(f.json.size()), turn, active,
                hex(f.digest).c_str());
            gc::ApplyOffer offer;
            const std::string skip = gc::candidate::guest_wire(data, static_cast<int>(f.meta.producer), offer.wire);
            if (skip.empty()) {
                // Offers are numbered by this PC, so a restarted host's revisions are never taken as stale.
                static unsigned offers = 0;
                offer.revision = ++offers;
                offer.host_revision = f.revision;
                offer.normalized_sha256 = hex(f.digest);
                gc::log("APPLY_OFFERED revision=%u offer=%u wire_bytes=%u", f.revision, offer.revision,
                        static_cast<unsigned>(offer.wire.size()));
                gc::apply_offer(std::move(offer));
            } else {
                gc::log("APPLY_SKIPPED revision=%u reason=%.200s", f.revision, skip.c_str());
            }
        } else {
            reply.ack = ss::Ack::rejected;
            gc::log("SESSION_REFUSED revision=%u reason=%.200s", f.revision, reason.c_str());
        }
        break;
    }
    case ss::Receiver::Verdict::duplicate: reply.ack = ss::Ack::duplicate; break;
    case ss::Receiver::Verdict::stale:
        reply.ack = ss::Ack::stale;
        gc::log("SESSION_STALE revision=%u held=%u", f.revision, session.in.revision);
        break;
    default:
        reply.ack = ss::Ack::rejected;
        gc::log("SESSION_REFUSED revision=%u reason=a different snapshot already holds this revision", f.revision);
        break;
    }
    session_send(from, reply);
}

// The other PC acknowledges one of this PC's revisions (K).
void peer_ack(Peer& from, const ss::Frame& f) {
    if (!from.out.on_ack(f.revision, f.ack)) {
        ++session.dropped;
        return;
    }
    static const char* names[] = {"", "accepted", "duplicate", "stale", "rejected"};
    const int turn = sent_turn(f.revision);
    gc::log("SESSION_ACK revision=%u status=%s rtt_ms=%llu turn=%d%s", f.revision, names[static_cast<int>(f.ack)],
            f.revision == from.out.sent ? GetTickCount64() - from.out.sent_at : 0ull, turn, tag(from).c_str());
    if (!versus_game()) return;
    if (f.ack == ss::Ack::accepted || f.ack == ss::Ack::duplicate) session.exchange.acknowledged(turn);
    else if (f.ack == ss::Ack::rejected) gc::log("VERSUS_HANDOFF_REFUSED revision=%u turn=%d", f.revision, turn);
}

// The other PC's Versus frames: side pick (P), game over (G), card request (C) or answer (D).
void versus_frame(const ss::Frame& f) {
    const bool guest = st.role == "guest";
    if (f.type == ss::Type::card_ask) {
        AcquireSRWLockExclusive(&versus_lock);
        const bool fresh = card_peer.on_ask(f.revision, f.json, f.cards);
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_CARD_ASKED number=%u mission=%s cards=%u,%u,%u fresh=%d", f.revision, f.json.c_str(),
                f.cards[0], f.cards[1], f.cards[2], fresh);
        return;
    }
    if (f.type == ss::Type::card_pick) {
        AcquireSRWLockExclusive(&versus_lock);
        const bool taken = card_ask.on_answer(f.revision, f.slot);
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_CARD_ANSWERED number=%u slot=%u taken=%d", f.revision, f.slot, taken);
        return;
    }
    if (f.type == ss::Type::pick) {
        AcquireSRWLockExclusive(&versus_lock);
        versus_sides.on_peer(f.team, f.ready, guest ? f.era : 0, guest && f.started);
        const gc::versus::Sides now = versus_sides;
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_PICK_RECEIVED number=%u side=%u ready=%d era=%u started=%d (this PC: side=%u ready=%d)",
                f.revision, f.team, f.ready, now.era, now.started, now.mine.team, now.mine.ready);
        return;
    }
    versus_winner.store(f.winner, std::memory_order_release);
    gc::log("VERSUS_OVER_RECEIVED turn=%u winner=%u", f.revision, f.winner);
}

// Sends this PC's Versus news when it changes: pick (P), game over (G), card request (C) and answer (D).
void versus_tick(const Peer& to, uint32_t id) {
    if (!versus_game() || !id) return;
    ss::Frame p, g;
    bool pick = false, over = false;
    AcquireSRWLockExclusive(&versus_lock);
    if (versus_sides.due()) {
        pick = true;
        p.type = ss::Type::pick;
        p.session = id;
        p.revision = versus_sides.sequence;
        p.team = versus_sides.mine.team;
        p.ready = versus_sides.mine.ready;
        p.era = st.role == "host" ? versus_sides.era : 0;
        p.started = st.role == "host" && versus_sides.started;
    }
    if (versus_over_turn && versus_over_turn != versus_over_sent) {
        over = true;
        g.type = ss::Type::game_over;
        g.session = id;
        g.revision = versus_over_turn;
        g.winner = versus_over_winner;
    }
    // This PC's card request as attacker, and its answer to the other PC's as defender.
    ss::Frame c, d;
    const bool ask = card_ask.due(), answer = card_peer.answer_due();
    if (ask) {
        c.type = ss::Type::card_ask;
        c.session = id;
        c.revision = card_ask.number;
        c.json = card_ask.mission;
        for (unsigned i = 0; i < gc::versus::card_slots; ++i) c.cards[i] = card_ask.cards[i];
    }
    if (answer) {
        d.type = ss::Type::card_pick;
        d.session = id;
        d.revision = card_peer.answered;
        d.slot = card_peer.slot;
    }
    ReleaseSRWLockExclusive(&versus_lock);
    if (ask && session_send(to, c)) {
        AcquireSRWLockExclusive(&versus_lock);
        card_ask.sent = c.revision;
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_CARD_ASK_SENT number=%u mission=%s cards=%u,%u,%u", c.revision, c.json.c_str(), c.cards[0],
                c.cards[1], c.cards[2]);
    }
    if (answer && session_send(to, d)) {
        AcquireSRWLockExclusive(&versus_lock);
        card_peer.answer_sent = d.revision;
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_CARD_PICK_SENT number=%u slot=%u", d.revision, d.slot);
    }
    if (pick && session_send(to, p)) {
        AcquireSRWLockExclusive(&versus_lock);
        versus_sides.sent = p.revision;
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_PICK_SENT number=%u side=%u ready=%d era=%u started=%d", p.revision, p.team, p.ready, p.era,
                p.started);
    }
    if (over && session_send(to, g)) {
        AcquireSRWLockExclusive(&versus_lock);
        versus_over_sent = g.revision;
        ReleaseSRWLockExclusive(&versus_lock);
        gc::log("VERSUS_OVER_SENT turn=%u winner=%u", g.revision, g.winner);
    }
}

// Battle news from the other PC: F, E and V from a battle's host, J from its client (kept by slot).
// An F also closes any open card request to this PC: the attacker has moved on.
bool battle_news_frame(const ss::Frame& f, unsigned slot = 1) {
    switch (f.type) {
    case ss::Type::battle: {
        gc::log("SESSION_BATTLE number=%u bytes=%u", f.revision, static_cast<unsigned>(f.json.size()));
        AcquireSRWLockExclusive(&battle_lock);
        if (f.revision > battle_news.offered) {
            battle_news.offered = f.revision;
            battle_news.offered_text = f.json;
        }
        ReleaseSRWLockExclusive(&battle_lock);
        AcquireSRWLockExclusive(&versus_lock);
        const bool closed = card_peer.open();
        card_peer.close();
        const uint32_t request = card_peer.number;
        ReleaseSRWLockExclusive(&versus_lock);
        if (closed) gc::log("VERSUS_CARD_CLOSED number=%u", request);
        return true;
    }
    case ss::Type::battle_over:
        gc::log("SESSION_BATTLE_OVER number=%u", f.revision);
        AcquireSRWLockExclusive(&battle_lock);
        if (f.revision > battle_news.offered_over) battle_news.offered_over = f.revision;
        ReleaseSRWLockExclusive(&battle_lock);
        return true;
    case ss::Type::battle_result:
        gc::log("SESSION_BATTLE_RESULT number=%u mask=%08X", f.revision, f.result);
        AcquireSRWLockExclusive(&battle_lock);
        if (f.revision > battle_news.offered_result) {
            battle_news.offered_result = f.revision;
            battle_news.offered_mask = f.result;
        }
        ReleaseSRWLockExclusive(&battle_lock);
        return true;
    case ss::Type::battle_part:
        gc::log("SESSION_BATTLE_PART number=%u part=%s slot=%u", f.revision,
                gc::battle::part_name(gc::battle::part_from(f.part)), slot);
        if (!slot || slot > s::max_friends) return false;
        AcquireSRWLockExclusive(&battle_lock);
        battle_news.reports[slot].number = f.revision;
        battle_news.reports[slot].part = f.part;
        ++battle_news.reports[slot].seq;
        ReleaseSRWLockExclusive(&battle_lock);
        return true;
    default: return false;
    }
}
// Frames that come from a battle's host.
bool battle_kind(ss::Type t) {
    return t == ss::Type::battle || t == ss::Type::battle_over || t == ss::Type::battle_result;
}
// Frames only Versus games use.
bool versus_kind(ss::Type t) {
    return t == ss::Type::pick || t == ss::Type::game_over || t == ss::Type::card_ask || t == ss::Type::card_pick;
}

// A frame from a paired peer: on the host a friend, under that friend's own session id; on a friend's
// PC its host.
void session_frame(Peer& from, const char* p, size_t n) {
    ss::Frame f;
    if (!ss::decode(p, n, f)) {
        ++session.dropped;
        return;
    }
    if (st.role == "host") {
        if (f.session != from.out.session) {
            ++session.dropped;
            return;
        }
        from.heard_at = GetTickCount64();
        if (f.type == ss::Type::ready) {
            const bool first = !from.out.ready;
            if (first) gc::log("SESSION_READY id=%08X peer_holds=%u%s", f.session, f.revision, tag(from).c_str());
            from.out.on_ready(f.revision);
            // Versus holds from pairing; co-op waits for Start on the party screen (hold_start).
            if (first && versus_game() && session.hold.ready(f.session)) gc::log("HOLD_WAITING id=%08X", f.session);
        } else if (f.type == ss::Type::loaded) {
            gc::log("SESSION_FRIEND_SHOWS revision=%u state=%s%s", f.revision,
                    f.shows == ss::Shows::applied ? "applied" : "stopped", tag(from).c_str());
            // A friend's game first shows the galaxy: one outside the Start hold is joining late.
            if (f.shows == ss::Shows::applied && !from.shown) {
                from.shown = true;
                if (!from.in_start && lobby_started && !versus_game()) {
                    st.notes.push_back(
                        s::PartyNote{++note_seq, index_of(from), s::party_name(from.name, index_of(from)), true});
                    gc::log("SESSION_FRIEND_JOINED_NOTE seq=%u%s", note_seq, tag(from).c_str());
                }
            }
            const unsigned loading = session.hold.loading();
            if (session.hold.on_loaded(f.session, f.shows))
                gc::log("HOLD_RELEASED id=%08X reason=%s", session.hold.session, session.hold.reason);
            else if (session.hold.loading() < loading && holding())
                gc::log("HOLD_FRIEND id=%08X%s %s; %u friend(s) still loading", session.hold.session, tag(from).c_str(),
                        f.shows == ss::Shows::applied ? "shows the galaxy" : "stopped applying",
                        session.hold.loading());
        } else if (f.type == ss::Type::bye) {
            // The friend has left the galaxy or its waiting popup; the game goes on while others remain.
            gc::log("SESSION_ENDED id=%08X by the friend; %s%s", f.session,
                    paired_count() > 1 ? "the host plays on with its other friends" : "leaving the co-op game",
                    tag(from).c_str());
            friend_left(from, "the friend left");
        } else if (f.type == ss::Type::battle_part) {
            // A friend's J, kept by its slot.
            battle_news_frame(f, slot_of(from));
        } else if (versus_game() && battle_kind(f.type)) {
            // Versus: the friend's PC hosts the battles it attacks in.
            battle_news_frame(f);
        } else if (f.type == ss::Type::ack) {
            peer_ack(from, f);
        } else if (versus_game() && f.type == ss::Type::snapshot) {
            peer_snapshot(from, f);
        } else if (versus_game() && versus_kind(f.type)) {
            versus_frame(f);
        } else ++session.dropped;
    } else if (st.role == "guest") {
        if (f.type == ss::Type::hello) {
            if (session.in.open(f.session)) {
                gc::log("SESSION_OPEN id=%08X", f.session);
                st.session_end.clear();
                // Only what this game applies from now on tells the host it shows the galaxy.
                session.shows.open(f.session, gc::apply_newest_revision() + 1);
                // The host's battles count from 1 again in this session.
                AcquireSRWLockExclusive(&battle_lock);
                ++battle_news.session;
                battle_news.offered = battle_news.offered_over = 0;
                battle_news.offered_text.clear();
                battle_news.offered_result = 0;
                battle_news.offered_mask = 0;
                ReleaseSRWLockExclusive(&battle_lock);
            }
            from.heard_at = GetTickCount64();
            st.session = "ready";
            // Versus: this PC sends too, under the host's session id.
            if (versus_game() && from.out.session != f.session) {
                from.out.open(f.session);
                from.out.on_ready(0);
                session.exchange.reset();
                gc::log("VERSUS_SESSION id=%08X", f.session);
            }
            ss::Frame ready;
            ready.type = ss::Type::ready;
            ready.session = f.session;
            ready.revision = session.in.revision;
            session_send(from, ready);
        } else if (f.type == ss::Type::snapshot) {
            if (f.session == session.in.session) from.heard_at = GetTickCount64();
            peer_snapshot(from, f);
        } else if (f.type == ss::Type::bye && f.session == session.in.session) {
            // The host has left its galaxy: this game leaves too, after a save prompt in Versus.
            gc::log("SESSION_ENDED id=%08X by the host", f.session);
            st.session_end = "the host left its galaxy";
            prompt_left(false);
            count_host_left();
            leave("left");
        } else if (battle_kind(f.type) && f.session == session.in.session) {
            // The host's battle news; the game thread acts on it.
            from.heard_at = GetTickCount64();
            battle_news_frame(f);
        } else if (versus_game() && f.type == ss::Type::battle_part && f.session == session.in.session) {
            // Versus: the host's part in this PC's battle (J).
            from.heard_at = GetTickCount64();
            battle_news_frame(f);
        } else if (versus_game() && f.type == ss::Type::ack && f.session == session.in.session) {
            from.heard_at = GetTickCount64();
            peer_ack(from, f);
        } else if (versus_game() && versus_kind(f.type) && f.session == session.in.session) {
            from.heard_at = GetTickCount64();
            versus_frame(f);
        } else ++session.dropped;
    }
}

// Friend's PC: tells the host what its game shows (L) whenever that changes, for the ready check.
void guest_report() {
    const Peer* host = first_paired();
    if (!host || !session.in.session || session.shows.session != session.in.session) return;
    const auto& progress = gc::apply_progress();
    const unsigned applied_offer = progress.applied_offer.load(std::memory_order_acquire);
    const unsigned applied = progress.applied.load(std::memory_order_acquire);
    const bool stopped = progress.phase.load(std::memory_order_acquire) == 4;  // apply::Loop's stopped phase
    uint32_t revision = 0;
    if (!session.shows.due(applied_offer, applied, stopped, revision)) return;
    ss::Frame shows;
    shows.type = ss::Type::loaded;
    shows.session = session.in.session;
    shows.revision = revision;
    shows.shows = stopped ? ss::Shows::stopped : ss::Shows::applied;
    if (!session_send(*host, shows)) return;
    session.shows.on_sent(revision, stopped);
    gc::log("SESSION_SHOWS revision=%u state=%s", revision, stopped ? "stopped" : "applied");
}

// Takes this PC's newest validated snapshot. At the menus everything captured so far becomes the
// floor: an earlier galaxy, never sent.
void session_take_capture() {
    gc::CaptureOffer next;
    if (gc::capture_latest(session.latest.revision, next)) session.latest = std::move(next);
    if (static_cast<ss::Place>(gc::game_place()) == ss::Place::menus && session.floor != session.latest.revision) {
        session.floor = session.latest.revision;
        gc::log("SESSION_FLOOR revision=%u", session.floor);
    }
    // Every peer is offered the same newest revision.
    for (Peer& p : peers) {
        p.out.latest = session.latest.revision;
        p.out.floor = session.floor;
    }
}

// Sends the newest revision to this peer when due. In Versus only what may_send allows: this PC's turn
// in play, a turn just handed over, or the host's first state for the friend's first load.
void session_send_revision(Peer& to, ULONGLONG now, uint32_t id) {
    if (!to.out.due(now)) return;
    const bool versus = versus_game();
    if (versus) {
        const gc::versus::Turn t{session.latest.player_team, session.latest.turn_number, session.latest.local_team};
        if (!gc::versus::may_send(t, session.exchange, st.role == "host")) return;
    }
    ss::Frame s;
    s.type = ss::Type::snapshot;
    s.session = id;
    s.revision = session.latest.revision;
    s.meta.producer = session.latest.producer;
    s.meta.generation = session.latest.generation;
    s.meta.sequence = session.latest.sequence;
    s.meta.source_table = session.latest.source_table;
    s.meta.update_ticket = session.latest.update_ticket;
    s.meta.unchanged_samples = session.latest.unchanged_samples;
    s.json = session.latest.json;
    if (!digest_of(session.latest.normalized_sha256, s.digest) || s.json.empty() || s.json.size() > ss::max_json) {
        gc::log("SESSION_ERROR revision=%u cannot be sent (bytes=%u)", s.revision,
                static_cast<unsigned>(s.json.size()));
        to.out.refused = s.revision;
        return;
    }
    const bool resend = to.out.sent == s.revision;
    if (session_send(to, s)) {
        to.out.on_sent(now);
        if (versus && !resend) {
            session.sent_turns[session.sent_turn_next++ % 8] =
                Session::SentTurn{s.revision, session.latest.turn_number};
            gc::log("VERSUS_TURN_SENT revision=%u turn=%d active=%d own=%d live=%d", s.revision,
                    session.latest.turn_number, session.latest.player_team, session.latest.local_team,
                    session.latest.player_team == session.latest.local_team);
        }
        gc::log("SESSION_SENT revision=%u bytes=%u resend=%d%s", s.revision, static_cast<unsigned>(s.json.size()),
                resend, tag(to).c_str());
    }
}

// Host: opens a newly paired friend's session under a fresh id.
void session_open(Peer& p) {
    p.out.open(random_id());
    p.hello_at = 0;
    gc::log("SESSION_OPENING id=%08X mode=%s%s", p.out.session, st.mode.c_str(), tag(p).c_str());
    // Versus: the friend's turns come back under this id, and its battles restart their numbering.
    if (versus_game()) {
        session.in.open(p.out.session);
        session.exchange.reset();
        AcquireSRWLockExclusive(&battle_lock);
        ++battle_news.session;
        battle_news.offered = battle_news.offered_over = 0;
        battle_news.offered_text.clear();
        battle_news.offered_result = 0;
        battle_news.offered_mask = 0;
        ReleaseSRWLockExclusive(&battle_lock);
    }
}

// Host: opens and keeps alive each friend's session, then sends battle news, Versus news and revisions.
void session_host_tick(ULONGLONG now) {
    session_take_capture();
    for (Peer& p : peers) {
        if (!paired(p)) continue;
        if (!p.out.session) session_open(p);
        // H opens the session until it is ready, then serves as the keepalive.
        if (now - p.hello_at >= (p.out.ready ? ss::keepalive_interval : ss::hello_interval)) {
            ss::Frame hello;
            hello.type = ss::Type::hello;
            hello.session = p.out.session;
            session_send(p, hello);
            p.hello_at = now;
        }
    }
    battle_host_news();
    Peer* first = first_paired();
    if (first && first->out.ready) versus_tick(*first, first->out.session);
    for (Peer& p : peers)
        if (paired(p)) session_send_revision(p, now, p.out.session);
}

// Friend's PC in Versus: its picks, the battles it hosts as attacker, and its own turns go to the host.
void session_guest_tick(ULONGLONG now) {
    if (!versus_game()) return;
    session_take_capture();
    Peer* host = first_paired();
    if (!host || !session.in.session) return;
    versus_tick(*host, session.in.session);
    battle_host_news();
    if (host->out.session == session.in.session) session_send_revision(*host, now, session.in.session);
}

// Reads session frames, runs this PC's side of every session and drops silent peers, then publishes
// the session state to the status file and the game thread.
void session_tick() {
    if (!net_if) return;
    if (!paired_count() || (st.role != "host" && st.role != "guest")) {
        if (session_active()) session_reset();
    } else {
        uint32_t size = 0;
        std::vector<char> buffer;
        for (int i = 0; i < 16 * s::max_friends && api.p2p_available(net_if, &size, ss::channel); ++i) {
            buffer.resize(size ? size : 1);
            uint32_t got = 0;
            u64 remote = 0;
            if (!api.p2p_read(net_if, buffer.data(), static_cast<uint32_t>(buffer.size()), &got, &remote, ss::channel))
                break;
            Peer* from = find_paired(remote);
            if (!from) {
                ++session.dropped;
                continue;
            }
            session_frame(*from, buffer.data(), got);
        }
        if (st.role == "host") session_host_tick(GetTickCount64());
        if (st.role == "guest") session_guest_tick(GetTickCount64());
        if (st.role == "guest") guest_report();
        // In Versus the session host can be a battle's client too.
        if (st.role == "guest" || versus_game()) battle_guest_news();
        // A paired peer that goes quiet is gone, whatever the lobby says; sooner if it also left the lobby.
        for (Peer& p : peers) {
            if (!paired(p) || !p.heard_at) continue;
            const ULONGLONG silent = GetTickCount64() - p.heard_at;
            if (silent <= ss::silence_limit && !(p.left_at && silent > ss::departed_limit)) continue;
            gc::log("SESSION_TIMEOUT id=%08X silent_ms=%llu left_lobby=%d; forgetting the peer%s", session_id(p),
                    silent, p.left_at != 0, tag(p).c_str());
            if (st.role == "guest")
                host_gone(p.left_at ? "the host left the lobby and went silent" : "the host went silent");
            else friend_left(p, p.left_at ? "the friend left the lobby and went silent" : "the friend went silent");
        }
    }
    // The host's session is ready once any friend's is.
    if (st.role == "host") {
        const char* state = "off";
        for (const Peer& p : peers)
            if (paired(p) && p.out.session) {
                state = p.out.ready ? "ready" : "opening";
                if (p.out.ready) break;
            }
        st.session = state;
    }
    hold_tick(GetTickCount64());
    const Peer* first = first_paired();
    st.session_id = st.role == "host" ? (first ? first->out.session : 0) : session.in.session;
    st.session_latest = session.latest.revision;
    st.session_sent = first ? first->out.sent : 0;
    st.session_acked = first ? first->out.acked : 0;
    st.session_received = session.in.revision;
    st.session_digest = st.role == "host"     ? session.latest.normalized_sha256
                        : session.in.revision ? hex(session.in.digest)
                                              : std::string();
    static const char* phases[] = {"waiting", "settling", "committing", "confirming", "stopped"};  // apply::Loop phases
    const auto& progress = gc::apply_progress();
    const int phase = progress.phase.load(std::memory_order_acquire);
    st.apply = phase >= 0 && phase <= 4 ? phases[phase] : "off";
    st.apply_offered = gc::apply_newest_host_revision();
    st.apply_applied = progress.applied.load(std::memory_order_acquire);
    st.apply_reason = progress.reason.load(std::memory_order_acquire);
    st.apply_in_galaxy = progress.in_galaxy.load(std::memory_order_acquire);
    st.apply_pending = progress.pending.load(std::memory_order_acquire);
    // Leaving the co-op galaxy ends the game for every PC; a shared battle still counts as in it.
    const auto place = static_cast<ss::Place>(gc::game_place());
    const bool ready = first && st.session == "ready";
    // Versus answers for the game thread. A Versus galaxy stays Versus (no AI for the other side) until
    // this PC is back at the menus.
    if (first && versus_game()) versus_latch = true;
    else if (!first && place == ss::Place::menus) versus_latch = false;
    versus_flag.store(versus_latch, std::memory_order_release);
    versus_host_flag.store(st.role == "host", std::memory_order_release);
    versus_pair_flag.store(ready && versus_game(), std::memory_order_release);
    bool showing = st.role == "host" ? place == ss::Place::galaxy : st.apply_in_galaxy;
    showing = showing || battle_busy_flag.load(std::memory_order_acquire);
    if (session.coop.step(ready, showing, place)) {
        end_coop(st.role == "host" ? "the host left its galaxy" : "the friend left the host's galaxy");
        return;
    }
    // The galaxy has shown with a friend paired: the co-op game has started (the lobby stays open).
    if (st.role == "host" && session.coop.shown && !lobby_started) {
        lobby_started = true;
        gc::log("STEAM_LOBBY_STARTED members=%d friends=%u", st.members, paired_count());
        update_joinable();
    }
}

// The other PC has left: counts a save prompt (friend_left) when save_prompt_due says so, ahead of any
// host_left. Call before leaving the lobby, which resets the mode.
bool prompt_left(bool session_host) {
    const bool in_galaxy = static_cast<ss::Place>(gc::game_place()) == ss::Place::galaxy;
    AcquireSRWLockShared(&versus_lock);
    const bool over = versus_game() && (versus_over_turn || versus_winner.load(std::memory_order_acquire));
    ReleaseSRWLockShared(&versus_lock);
    const bool due = ss::save_prompt_due(versus_game(), session_host, in_galaxy, over);
    if (due) gc::apply_progress().friend_left.fetch_add(1, std::memory_order_acq_rel);
    if (versus_game() || session_host)
        gc::log("SESSION_SAVE_PROMPT due=%d in_galaxy=%d game_over=%d mode=%s", due, in_galaxy, over, st.mode.c_str());
    return due;
}

// Friend's PC: the host has quit, crashed or lost its connection. Its galaxy is dropped for the menus,
// after a save prompt in Versus.
void host_gone(const char* why) {
    gc::log("SESSION_HOST_LEFT id=%08X reason=%s", session.in.session, why);
    st.session_end = why;
    prompt_left(false);
    count_host_left();
    if (Peer* host = first_paired()) forget(*host);
    else leave("left");
}

// Host: a friend has left, quit or been lost. Only its session ends while others remain; the last one's
// leaving ends the game and the lobby, after a save prompt if due.
void friend_left(Peer& p, const char* why) {
    const unsigned others = paired_count() - (paired(p) ? 1 : 0);
    gc::log("SESSION_FRIEND_LEFT id=%08X reason=%s%s others=%u", p.out.session, why, tag(p).c_str(), others);
    if (others) {
        // Once the game has started the galaxy says who has left (the name goes in the party rows).
        if (lobby_started && paired(p)) {
            st.notes.push_back(s::PartyNote{++note_seq, index_of(p), s::party_name(p.name, index_of(p))});
            gc::log("SESSION_FRIEND_LEFT_NOTE seq=%u%s", note_seq, tag(p).c_str());
        }
        forget(p);
        return;
    }
    st.session_end = why;
    if (prompt_left(true)) gc::log("SESSION_FRIEND_LEFT the host's game offers to save");
    leave("left");
}

// This PC leaves the game (its galaxy, or the friend's waiting popup): every paired peer gets B and the
// lobby is left, so the next game needs a new invite.
void end_coop(const char* why) {
    for (Peer& p : peers) {
        if (!paired(p)) continue;
        const uint32_t id = session_id(p);
        ss::Frame bye;
        bye.type = ss::Type::bye;
        bye.session = id;
        const bool told = id && session_send(p, bye);
        gc::log("SESSION_ENDED id=%08X reason=%s told=%d; %s%s", id, why, told,
                st.role == "host" ? "each friend's game leaves the co-op game too" : "this game leaves the co-op game",
                tag(p).c_str());
    }
    st.session_end = why;
    if (st.role == "guest") count_host_left();
    leave("left");
}

// A peer is gone: ends everything tied to it. A friend's PC leaves the lobby; the host keeps its other
// friends and leaves only once it has none and Steam has dropped its lobby.
void forget(Peer& p) {
    if (!p.id) return;
    relay_peer_gone(p.id);
    if (st.role == "host") end_friend_session(p);
    p = Peer{};
    st.offer_sent = false;  // the developer offer goes again to the next peer
    if (st.role != "host") {
        leave("left");
        return;
    }
    if (!paired_count()) session_reset();
    if (lobby_lost && !paired_count()) leave("left");
    else update_joinable();
}

// Handles one queued Steam callback.
void handle(const Event& e) {
    switch (e.id) {
    case s::k_lobby_created: {
        LobbyCreated r;
        memcpy(&r, e.data, sizeof r);
        if (st.state != "creating" || e.call != pending_call) return;
        pending_call = 0;
        if (e.io || r.result != 1 || !s::valid_lobby_id(r.lobby)) {
            reset_session("error");
            fail("create_lobby_failed");
            return;
        }
        st.lobby = r.lobby;
        st.state = "hosting";
        st.members = 1;
        // Mark the lobby as GCT's, advertise it through rich presence, and open the invite overlay.
        api.set_data(mm_if, st.lobby, "gct", s::protocol);
        api.set_data(mm_if, st.lobby, "gct_build", s::build_id);
        api.set_data(mm_if, st.lobby, gc::versus::mode_key, st.mode.c_str());
        const std::string connect = "+connect_lobby " + std::to_string(st.lobby);
        api.set_presence(friends_if, "connect", connect.c_str());
        api.set_presence(friends_if, "status",
                         st.mode == "versus" ? "Galactic Conquest versus" : "Galactic Conquest co-op");
        gc::log("STEAM_LOBBY_CREATED lobby=%llu mode=%s; opening invite overlay", st.lobby, st.mode.c_str());
        api.invite_dialog(friends_if, st.lobby);
        return;
    }
    case s::k_lobby_enter: {
        LobbyEnter r;
        memcpy(&r, e.data, sizeof r);
        if (st.state != "joining" || e.call != pending_call) return;
        pending_call = 0;
        if (e.io || r.response != 1) {
            reset_session("error");
            fail("join_lobby_failed");
            return;
        }
        const char* marker = api.get_data(mm_if, st.lobby, "gct");
        const char* build = api.get_data(mm_if, st.lobby, "gct_build");
        if (!marker || strcmp(marker, s::protocol)) {
            leave("error");
            fail("not_a_gct_lobby");
            return;
        }
        if (!build || strcmp(build, s::build_id)) {
            leave("error");
            fail("lobby_build_mismatch");
            return;
        }
        // The lobby's owner is this PC's host; it pairs with nobody else in the lobby.
        const Peer* host = guest_host();
        if (!host) {
            leave("error");
            fail("lobby_owner_unknown");
            return;
        }
        st.state = "joined";
        st.members = api.members(mm_if, st.lobby);
        st.mode = gc::versus::mode_name(gc::versus::parse_mode(api.get_data(mm_if, st.lobby, gc::versus::mode_key)));
        gc::log("STEAM_LOBBY_ENTERED lobby=%llu members=%d mode=%s owner=%llu", st.lobby, st.members, st.mode.c_str(),
                host->id);
        send(s::Kind::hello, hello_text);
        return;
    }
    case s::k_lobby_join_requested: {
        JoinRequested r;
        memcpy(&r, e.data, sizeof r);
        if (s::valid_lobby_id(r.lobby)) begin_join(r.lobby, "invite");
        return;
    }
    case s::k_rich_presence_join: {
        RichPresenceJoin r;
        memcpy(&r, e.data, sizeof r);
        r.connect[sizeof r.connect - 1] = '\0';
        u64 lobby = 0;
        if (s::parse_connect_lobby(r.connect, lobby)) begin_join(lobby, "presence");
        else gc::log("STEAM_INVITE ignored rich-presence join without a lobby");
        return;
    }
    case s::k_lobby_chat_update: {
        LobbyChatUpdate r;
        memcpy(&r, e.data, sizeof r);
        if (!st.lobby || r.lobby != st.lobby) return;
        st.members = api.members(mm_if, st.lobby);
        if (r.state & 0x1) {         // k_EChatMemberStateChangeEntered
            note_member(r.changed);  // host only: the member gets an entry
            send(s::Kind::hello, hello_text);
        } else if (Peer* p = find_peer(r.changed)) {  // left, disconnected, kicked or banned
            gc::log("STEAM_PEER_LEFT id=%llu state=%u%s", p->id, r.state, tag(*p).c_str());
            // A paired peer stays: the session runs on P2P, and the keepalive ends it if the peer is gone.
            if (p->build_match) {
                p->left_at = GetTickCount64();
                gc::log("SESSION keeping the P2P session with the paired peer");
                update_joinable();
                return;
            }
            forget(*p);
            return;
        } else if (r.changed == st.self) {
            // Steam has ended this PC's own membership.
            gc::log("STEAM_LOBBY_LOST lobby=%llu state=%u members=%d", st.lobby, r.state, st.members);
            lobby_lost = true;
            // Unpaired peers go; paired sessions run on over P2P.
            for (Peer& q : peers)
                if (q.id && !q.build_match) forget(q);
            if (st.lobby && !paired_count()) leave("left");
            return;
        }
        update_joinable();
        return;
    }
    case s::k_lobby_chat_msg: {
        LobbyChatMsg r;
        memcpy(&r, e.data, sizeof r);
        if (!st.lobby || r.lobby != st.lobby) return;
        char buffer[s::max_message + 16];
        u64 user = 0;
        int type = 0;
        const int n = api.chat_entry(mm_if, st.lobby, static_cast<int>(r.chat), &user, buffer, sizeof buffer, &type);
        if (n <= 0 || type != 1 || user == st.self) return;  // 1: k_EChatEntryTypeChatMsg
        std::string body;
        const s::Kind kind = s::decode_message(buffer, static_cast<size_t>(n), body);
        if (kind == s::Kind::none) {
            gc::log("STEAM_INVITE ignored foreign chat message bytes=%d", n);
            return;
        }
        // The host takes every member's messages; a friend's PC only its host's.
        const bool host = st.role == "host";
        Peer* const owner = host ? nullptr : guest_host();
        if (!s::takes_hello(host, user, st.self, owner ? owner->id : 0)) {
            gc::log("STEAM_INVITE ignoring %s from %llu: not this PC's host",
                    kind == s::Kind::hello ? "HELLO" : "OFFER", user);
            return;
        }
        Peer* p = host ? note_member(user) : owner;
        if (!p) return;
        if (kind == s::Kind::hello) {
            const bool match = body == hello_text, newcomer = !p->build_match;
            p->build = body;
            p->build_match = match;
            gc::log("STEAM_PEER_HELLO build=%s match=%d%s", body.c_str(), match, tag(*p).c_str());
            if (host && !newcomer && !match) end_friend_session(*p);
            // Answer a member not paired yet, so one whose entry races the host's HELLO still gets one.
            if (host && newcomer) send(s::Kind::hello, hello_text);
        } else {
            p->offer = body;
            p->offer_valid = true;
            gc::log("STEAM_OFFER_RECEIVED bytes=%u%s", static_cast<unsigned>(body.size()), tag(*p).c_str());
        }
        return;
    }
    case s::k_p2p_session_request: {
        P2PSessionRequest r;
        memcpy(&r, e.data, sizeof r);
        if (find_paired(r.remote)) api.p2p_accept(net_if, r.remote);
        else gc::log("STEAM_RELAY refused P2P session from %llu", r.remote);
        return;
    }
    case s::k_p2p_connect_fail: {
        P2PSessionConnectFail r;
        memcpy(&r, e.data, sizeof r);
        Peer* p = find_peer(r.remote);
        if (!p) return;
        gc::log("STEAM_RELAY_ERROR P2P session failed error=%u%s", r.error, tag(*p).c_str());
        // The relay belongs to the first paired peer.
        const bool relay_peer = p == first_paired() || !first_paired();
        if (p->build_match) {
            if (st.role == "guest") host_gone("the P2P session failed");
            else friend_left(*p, "the P2P session failed");
            if (relay_peer) st.relay = "failed";
            return;
        }
        relay_peer_gone(p->id);
        if (relay_peer) st.relay = "failed";
        return;
    }
    }
}

std::string host_mode = "coop";  // the mode of the host's last invite
// Opens the invite overlay, creating the lobby first if there is none.
void request_invite() {
    if (st.state == "hosting") {
        // An unpaired lobby takes this invite's mode and member limit.
        if (!paired_count() && st.mode != host_mode) {
            st.mode = host_mode;
            api.set_data(mm_if, st.lobby, gc::versus::mode_key, st.mode.c_str());
            api.set_presence(friends_if, "status",
                             st.mode == "versus" ? "Galactic Conquest versus" : "Galactic Conquest co-op");
            const int limit = s::lobby_limit(st.mode == "versus");
            const bool limited = api.member_limit(mm_if, st.lobby, limit);
            gc::log("STEAM_LOBBY_MODE mode=%s limit=%d ok=%d", st.mode.c_str(), limit, limited);
            update_joinable();
        }
        // A co-op host may invite more friends until the lobby is full; a paired Versus game needs no invite.
        if (!paired_count() || (st.mode == "coop" && st.members < s::lobby_limit(false)))
            api.invite_dialog(friends_if, st.lobby);
    } else if (st.state == "idle" || st.state == "left" || st.state == "error") {
        reset_session("creating");
        st.role = "host";
        st.mode = host_mode;
        st.error.clear();
        EnterCriticalSection(&pump_lock);
        pending_call = api.create_lobby(mm_if, 1 /* k_ELobbyTypeFriendsOnly */, s::lobby_limit(st.mode == "versus"));
        if (pending_call) api.reg_result(&on_created, pending_call);
        LeaveCriticalSection(&pump_lock);
        if (!pending_call) {
            reset_session("error");
            fail("create_call_rejected");
        }
    } else if (st.state != "creating") fail("invite_ignored_session_active");
}

// Copies the peers into st for the status file and the menus, then fills the single peer fields.
void describe_peers() {
    st.peers.clear();
    for (const Peer& p : peers) {
        if (!p.id) continue;
        s::PeerStatus d;
        d.id = p.id;
        d.index = index_of(p);
        d.name = p.name;
        d.build = p.build;
        d.offer = p.offer;
        d.build_match = p.build_match;
        d.offer_valid = p.offer_valid;
        d.left_lobby = p.left_at != 0;
        if (st.role == "host") {
            d.session = !p.out.session ? "off" : p.out.ready ? "ready" : "opening";
            d.session_id = p.out.session;
        } else {
            d.session = st.session;
            d.session_id = session.in.session;
        }
        d.sent = p.out.sent;
        d.acked = p.out.acked;
        st.peers.push_back(std::move(d));
    }
    s::summarize(st);
}

// The host's party screen rows while it has a co-op lobby, guarded by party_lock. The game thread
// reads them through party_rows; party_seq changes whenever they do.
SRWLOCK party_lock = SRWLOCK_INIT;
bool party_on = false;
std::string party_text;
unsigned party_seq = 0;
// Republishes the rows when they change.
void publish_party() {
    const bool on = st.role == "host" && st.state == "hosting" && st.mode == "coop";
    const std::string rows = on ? s::party_rows(st) : std::string();
    AcquireSRWLockExclusive(&party_lock);
    if (on != party_on || rows != party_text) {
        party_on = on;
        party_text = rows;
        ++party_seq;
    }
    ReleaseSRWLockExclusive(&party_lock);
}

// The Online menu (player_menu.lua): the game thread posts requests as bits, this thread acts on them
// and publishes the answers its popups ask for. invite_more reopens the overlay for the lobby.
enum MenuRequest : unsigned {
    menu_invite = 1,
    menu_cancel = 2,
    menu_leave = 4,
    menu_closing = 8,
    menu_invite_more = 16
};
std::atomic<unsigned> menu_requests{0};
// Exit to Windows: set once the other PCs have been told (menu_event waits for it).
std::atomic<bool> menu_closed{false};
std::atomic<unsigned> menu_published{0};  // MenuAnswer bits
// Set with an invite request and cleared once newer answers are published, so the popup never reads
// an earlier attempt's failure.
std::atomic<bool> menu_pending{false};
std::atomic<bool> menu_versus{false};    // the invite is for a Versus game
std::atomic<unsigned> lobby_members{0};  // 0 without a lobby
std::atomic<unsigned> notes_shown{0};    // the newest party note the galaxy has shown
bool menu_waiting = false;               // an invite popup waits for its answer
unsigned menu_last = 0;                  // the answers last computed

// Acts on the menu's requests, drops party notes already shown, and publishes the menu answers.
void menu_tick() {
    const unsigned requests = menu_requests.exchange(0, std::memory_order_acq_rel);
    if (requests & menu_invite) {
        host_mode = menu_versus.load(std::memory_order_acquire) ? "versus" : "coop";
        gc::log("MENU_INVITE state=%s role=%s paired=%u mode=%s", st.state.c_str(), st.role.c_str(), paired_count(),
                host_mode.c_str());
        menu_waiting = true;
        // A friend paired in the other mode means starting over with a new lobby.
        if (st.role == "host" && paired_count() && st.mode != host_mode)
            end_coop("the host started a game of the other mode");
        if (st.role != "guest" || (st.state != "joining" && st.state != "joined")) request_invite();
        // Another Versus game with the paired friend: its first galaxy waits for the friend's game again.
        const Peer* first = st.role == "host" && versus_game() ? first_paired() : nullptr;
        if (first && first->out.ready && session.hold.again(first->out.session))
            gc::log("HOLD_WAITING id=%08X again", first->out.session);
    }
    if (requests & menu_invite_more) {
        gc::log("MENU_INVITE_MORE state=%s role=%s members=%d paired=%u", st.state.c_str(), st.role.c_str(), st.members,
                paired_count());
        if (st.role == "host" && st.state == "hosting") request_invite();
    }
    if (requests & menu_cancel) {
        menu_waiting = false;
        // A friend not paired yet would join a game nobody starts.
        if (st.role == "host" && st.lobby && !paired_count()) leave("left");
        gc::log("MENU_CANCELLED state=%s", st.state.c_str());
    }
    // The friend presses Back on its waiting popup or the Versus side screen.
    if ((requests & menu_leave) && st.role == "guest" && st.lobby) {
        gc::log("MENU_GUEST_LEFT state=%s", st.state.c_str());
        if (paired_count()) end_coop("the friend left on its waiting popup");
        else leave("left");
    }
    // The host presses Back on the Versus side screen or the party screen: the game ends for everyone.
    if ((requests & menu_leave) && st.role == "host" && st.lobby) {
        gc::log("MENU_HOST_LEFT state=%s", st.state.c_str());
        if (paired_count())
            end_coop(versus_game() ? "the host left the side screen" : "the host left the party screen");
        else leave("left");
    }
    // Exit to Windows: tell the other PCs now; a closing game cannot later, and they would otherwise
    // only notice the silence.
    if (requests & menu_closing) {
        gc::log("MENU_CLOSING state=%s role=%s paired=%u", st.state.c_str(), st.role.c_str(), paired_count());
        if (paired_count()) {
            // Closing inside a Versus battle forfeits it; that goes first.
            battle_goodbye();
            end_coop(st.role == "host" ? "the host closed its game" : "the friend closed its game");
        } else if (st.lobby) leave("left");
        menu_closed.store(true, std::memory_order_release);
    }
    // Drop the party notes the galaxy has shown.
    const unsigned shown = notes_shown.load(std::memory_order_acquire);
    for (size_t i = 0; i < st.notes.size();) {
        if (st.notes[i].seq > shown) {
            ++i;
            continue;
        }
        gc::log("SESSION_NOTE_SHOWN seq=%u friend=%u %s", st.notes[i].seq, st.notes[i].slot,
                st.notes[i].joined ? "joined" : "left");
        st.notes.erase(st.notes.begin() + static_cast<std::ptrdiff_t>(i));
    }
    describe_peers();
    publish_party();
    lobby_members.store(st.lobby ? static_cast<unsigned>(st.members) : 0, std::memory_order_release);
    // A lobby Steam has dropped takes no invites.
    const unsigned answers = s::menu_answers(st) & ~(lobby_lost ? unsigned(s::menu_party_invite) : 0u);
    if (menu_waiting && answers != menu_last) {
        gc::log("MENU_WAITING paired=%d failed=%d build=%d guest=%d no_overlay=%d state=%s peer_build=%s",
                !!(answers & s::menu_paired), !!(answers & s::menu_failed), !!(answers & s::menu_build),
                !!(answers & s::menu_guest), !!(answers & s::menu_no_overlay), st.state.c_str(), st.peer_build.c_str());
        if (answers & (s::menu_paired | s::menu_failed)) menu_waiting = false;
    }
    menu_last = answers;
    menu_published.store(answers, std::memory_order_release);
    if (requests & menu_invite) menu_pending.store(false, std::memory_order_release);
}

#if GC_TOOL_FILES
// Developer tools: request-<pid>.txt (invite or leave), relay-<pid>.txt and offer-<pid>.txt.
void poll_files() {
    std::string request;
    const std::wstring request_path = path_for(L"request", L".txt");
    if (read_small(request_path, request, 64)) {
        DeleteFileW(request_path.c_str());
        gc::log("STEAM_INVITE request=%s", request.c_str());
        if (request == "invite") {
            request_invite();
        } else if (request == "leave") {
            if (st.lobby) leave("left");
        } else fail("unknown_request");
    }
    std::string relay_request;
    const std::wstring relay_path = path_for(L"relay", L".txt");
    if (read_small(relay_path, relay_request, 64)) {
        DeleteFileW(relay_path.c_str());
        configure_relay(relay_request);
    }
    std::string offer;
    if (read_small(path_for(L"offer", L".txt"), offer, s::max_offer + 8) && offer != offer_file_text) {
        offer_file_text = offer;
        if (s::safe_offer(offer.data(), offer.size())) {
            st.local_offer = offer;
            st.offer_sent = false;
            gc::log("STEAM_OFFER_LOCAL bytes=%u", static_cast<unsigned>(offer.size()));
        } else fail("local_offer_rejected");
    }
}
#endif

// Fetches missing persona names, sends a pending developer offer, and rewrites the status file when
// its contents change.
void publish_status() {
    const ULONGLONG now = GetTickCount64();
    for (Peer& p : peers)
        if (p.id && p.name.empty() && now >= p.name_retry) {
            const char* name = api.friend_name(friends_if, p.id);
            if (name && *name) p.name = name;
            p.name_retry = now + 1000;
        }
    if (paired_count() && !st.local_offer.empty() && !st.offer_sent) {
        st.offer_sent = send(s::Kind::offer, st.local_offer);
        gc::log("STEAM_OFFER_SENT ok=%d bytes=%u", st.offer_sent, static_cast<unsigned>(st.local_offer.size()));
    }
    describe_peers();
    s::Status probe = st;
    probe.revision = 0;
    std::string body = s::format_status(probe, pid);
    if (body == last_written) return;
    last_written = body;
    ++st.revision;
    probe.revision = st.revision;
    if (!write_replace(path_for(L"status", L".json"), s::format_status(probe, pid)))
        gc::log("STEAM_INVITE status write failed: %lu", GetLastError());
}

HMODULE game_module = nullptr;  // the module whose imports are hooked

#if GC_PLAYER
// Removes files left in the status folder by game processes that have exited.
void remove_stale_files() {
    WIN32_FIND_DATAW d{};
    HANDLE h = FindFirstFileW((dir + L"\\*-*.*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return;
    unsigned removed = 0;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* dash = wcsrchr(d.cFileName, L'-');
        wchar_t* stop = nullptr;
        const unsigned long owner = dash ? wcstoul(dash + 1, &stop, 10) : 0;
        if (!owner || owner == pid || !stop || *stop != L'.') continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
        if (process) {
            CloseHandle(process);
            continue;
        }
        if (GetLastError() == ERROR_INVALID_PARAMETER && DeleteFileW((dir + L"\\" + d.cFileName).c_str())) ++removed;
    } while (FindNextFileW(h, &d));
    FindClose(h);
    if (removed) gc::log("STEAM_INVITE removed %u file(s) left by exited game processes", removed);
}
#endif

// Steam reports the overlay only once it has hooked the game's renderer, a few seconds after start-up,
// so the flag is read again on every tick.
void read_overlay() {
    const bool on = api.overlay(utils_if);
    if (on == st.overlay) return;
    st.overlay = on;
    gc::log("STEAM_OVERLAY enabled=%d", on);
}

// The Steam thread: binds to steam_api, hooks the game's imports, then pumps callbacks and runs the
// session until the process exits.
DWORD WINAPI steam_thread(void*) {
    HMODULE dll = nullptr;
    for (int i = 0; i < 600 && !(dll = GetModuleHandleW(L"steam_api.dll")); ++i) Sleep(100);
    if (!dll || !bind_exports(dll)) {
        fail("steam_api_unavailable");
        st.state = "error";
        publish_status();
        return 1;
    }
    int pipe = 0, user = 0;
    for (int i = 0; i < 600 && !(pipe = api.pipe()); ++i) Sleep(100);
    user = api.user();
    void* client = api.client();
    if (!pipe || !user || !client) {
        fail("steam_not_initialised");
        st.state = "error";
        publish_status();
        return 1;
    }
    // The same interface versions the game itself requests.
    user_if = api.user_if(client, user, pipe, "SteamUser019");
    friends_if = api.friends_if(client, user, pipe, "SteamFriends015");
    utils_if = api.utils_if(client, pipe, "SteamUtils008");
    mm_if = api.mm_if(client, user, pipe, "SteamMatchMaking009");
    net_if = api.net_if(client, user, pipe, "SteamNetworking005");
    if (!user_if || !friends_if || !utils_if || !mm_if || !net_if) {
        fail("steam_interfaces_unavailable");
        st.state = "error";
        publish_status();
        return 1;
    }
    st.self = api.steam_id(user_if);
    st.app_id = api.app_id(utils_if);
    st.overlay = api.overlay(utils_if);
    // 6060 is the game's Steam app id.
    if (!s::valid_user_id(st.self) || st.app_id != 6060) {
        fail("unexpected_steam_identity");
        st.state = "error";
        publish_status();
        return 1;
    }
    if (!hook_game_imports(game_module)) {
        fail("iat_hook_failed");
        st.state = "error";
        publish_status();
        return 1;
    }
    EnterCriticalSection(&pump_lock);
    api.reg(&on_join_requested, s::k_lobby_join_requested);
    api.reg(&on_presence_join, s::k_rich_presence_join);
    api.reg(&on_chat_update, s::k_lobby_chat_update);
    api.reg(&on_chat_msg, s::k_lobby_chat_msg);
    api.reg(&on_p2p_request, s::k_p2p_session_request);
    api.reg(&on_p2p_fail, s::k_p2p_connect_fail);
    LeaveCriticalSection(&pump_lock);
#if GC_TOOL_FILES
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fail("winsock_unavailable");
        st.state = "error";
        publish_status();
        return 1;
    }
#endif
    st.ready = true;
    st.state = "idle";
    gc::log("STEAM_INVITE_READY app=%u overlay=%d self=%llu", st.app_id, st.overlay, st.self);
    // Steam starts the game with "+connect_lobby <id>" when an invite is accepted while it is closed.
    u64 lobby = 0;
    if (s::parse_connect_lobby(GetCommandLineA(), lobby)) begin_join(lobby, "cmdline");
    Sleep(1000);  // let calls begun before the import swap finish
    // Session and tunnel every 10 ms; callbacks, files, menu and status every 100 ms.
    for (unsigned tick = 0;; ++tick) {
        if (tick % 10 == 0) {
            read_overlay();
            for (const Event& e : pump()) handle(e);
#if GC_TOOL_FILES
            poll_files();
#endif
            menu_tick();
            publish_status();
        }
#if GC_TOOL_FILES
        relay_tick();
#endif
        session_tick();
        // While a battle is sealed, tick every millisecond.
        bool fast = false;
        tunnel_tick();
        fast = gc::battle_sealed();
#if GC_TOOL_FILES
        fast = fast || relay.local != INVALID_SOCKET;
#endif
        Sleep(fast ? 1 : 10);
    }
}
}

namespace gc {
// Game thread: the Online menu's GCOBS:menu:* events.
void menu_event(const char* event) {
    unsigned bit = 0;
    if (!strcmp(event, "invite_more")) bit = menu_invite_more;
    else if (!strncmp(event, "invite", 6)) {
        bit = menu_invite;
        menu_pending.store(true, std::memory_order_release);
        // Versus invites ("invite:vs_new", "invite:vs_load") start with "vs".
        menu_versus.store(!strncmp(event, "invite:vs", 9), std::memory_order_release);
    } else if (!strcmp(event, "cancel")) bit = menu_cancel;
    else if (!strcmp(event, "leave")) bit = menu_leave;
    else if (!strcmp(event, "closing")) bit = menu_closing;
    // Start on the party screen; the Steam thread takes it long before the galaxy's first frame.
    else if (!strcmp(event, "start")) hold_events.fetch_or(hold_started, std::memory_order_acq_rel);
    // The galaxy has shown the party note with this number.
    else if (!strncmp(event, "noted:", 6)) {
        const unsigned seq = static_cast<unsigned>(strtoul(event + 6, nullptr, 10));
        unsigned was = notes_shown.load(std::memory_order_acquire);
        while (seq > was && !notes_shown.compare_exchange_weak(was, seq, std::memory_order_acq_rel)) {}
    }
    if (bit) menu_requests.fetch_or(bit, std::memory_order_acq_rel);
    // The game closes as soon as this returns: wait up to 0.5 s for the other PCs to be told.
    if (bit == menu_closing)
        for (int i = 0; i < 50 && !menu_closed.load(std::memory_order_acquire); ++i) Sleep(10);
}
// Game thread: answers the Online menu's GCOBS:ask:* questions.
bool menu_answer(const char* question) {
    // lobby_members_N and hold_loading_N: the lobby's size, and the friends' games the hold awaits.
    unsigned n = 0;
    if (sscanf_s(question, "lobby_members_%u", &n) == 1) return n && lobby_members.load(std::memory_order_acquire) == n;
    if (sscanf_s(question, "hold_loading_%u", &n) == 1) return n && hold_loading.load(std::memory_order_acquire) == n;
    unsigned bit = s::menu_answer_bit(question);
    if (menu_pending.load(std::memory_order_acquire))
        bit &= ~(s::menu_paired | s::menu_failed | s::menu_build | s::menu_guest);
    return bit && (menu_published.load(std::memory_order_acquire) & bit);
}
// Whether this PC is a paired host, as the Online menu's answers say.
bool coop_host_paired() { return (menu_published.load(std::memory_order_acquire) & s::menu_paired) != 0; }
// The host's party screen rows and their number, while it has a co-op lobby.
bool party_rows(unsigned& seq, std::string& text) {
    AcquireSRWLockShared(&party_lock);
    const bool on = party_on;
    if (on) {
        seq = party_seq;
        text = party_text;
    }
    ReleaseSRWLockShared(&party_lock);
    return on;
}
// Game thread, battle host: a battle (F) and its end (E) to announce.
void battle_host_announce(unsigned number, const std::string& text) {
    AcquireSRWLockExclusive(&battle_lock);
    if (number > battle_news.announced) {
        battle_news.announced = number;
        battle_news.text = text;
    }
    ReleaseSRWLockExclusive(&battle_lock);
}
void battle_host_over(unsigned number) {
    AcquireSRWLockExclusive(&battle_lock);
    if (number > battle_news.over) battle_news.over = number;
    ReleaseSRWLockExclusive(&battle_lock);
}
// Game thread, client: the session count, and what the battle host has announced in that session.
unsigned battle_guest_session() {
    AcquireSRWLockExclusive(&battle_lock);
    const unsigned session = battle_news.session;
    ReleaseSRWLockExclusive(&battle_lock);
    return session;
}
bool battle_guest_offered(unsigned session, unsigned after, unsigned& number, std::string& text) {
    AcquireSRWLockExclusive(&battle_lock);
    const bool fresh = session == battle_news.session && battle_news.offered > after;
    if (fresh) {
        number = battle_news.offered;
        text = battle_news.offered_text;
    }
    ReleaseSRWLockExclusive(&battle_lock);
    return fresh;
}
unsigned battle_guest_over(unsigned session) {
    AcquireSRWLockExclusive(&battle_lock);
    const unsigned over = session == battle_news.session ? battle_news.offered_over : 0;
    ReleaseSRWLockExclusive(&battle_lock);
    return over;
}
// A shared battle runs on this PC, from the join to the return.
void battle_set_busy(bool busy) { battle_busy_flag.store(busy, std::memory_order_release); }
// Game thread, client: this game's part in battle `number` (J).
void battle_guest_report(unsigned number, battle::Part part) {
    if (!number || part == battle::Part::none) return;
    AcquireSRWLockExclusive(&battle_lock);
    battle_news.part_number = number;
    battle_news.part = static_cast<uint8_t>(part);
    ++battle_news.part_seq;
    ReleaseSRWLockExclusive(&battle_lock);
}
// Game thread, battle host: the winning-team mask its game has recorded (V).
void battle_host_result(unsigned number, unsigned mask) {
    if (!number || !mask) return;
    AcquireSRWLockExclusive(&battle_lock);
    if (number > battle_news.result) {
        battle_news.result = number;
        battle_news.result_mask = mask;
    }
    ReleaseSRWLockExclusive(&battle_lock);
}
// Game thread, client: the newest result (V) the battle host has sent in `session`.
bool battle_guest_result(unsigned session, unsigned& number, unsigned& mask) {
    AcquireSRWLockExclusive(&battle_lock);
    const bool fresh = session == battle_news.session && battle_news.offered_result;
    if (fresh) {
        number = battle_news.offered_result;
        mask = battle_news.offered_mask;
    }
    ReleaseSRWLockExclusive(&battle_lock);
    return fresh;
}
// Game thread: what battle_goodbye says if this game closes now.
void battle_closing(unsigned number, bool host, unsigned mask) {
    AcquireSRWLockExclusive(&battle_lock);
    battle_news.closing = number;
    battle_news.closing_host = number && host;
    battle_news.closing_mask = number ? mask : 0;
    ReleaseSRWLockExclusive(&battle_lock);
}
// Game thread, battle host: the newest J from the friend in `slot`, unless its count is still `after`.
bool battle_host_report(unsigned slot, unsigned after, unsigned& seq, unsigned& number, battle::Part& part) {
    if (!slot || slot > s::max_friends) return false;
    AcquireSRWLockExclusive(&battle_lock);
    const auto& report = battle_news.reports[slot];
    const bool fresh = report.seq != after;
    if (fresh) {
        seq = report.seq;
        number = report.number;
        part = battle::part_from(report.part);
    }
    ReleaseSRWLockExclusive(&battle_lock);
    return fresh;
}
// The slots told of battle `number` (F), one bit each.
unsigned battle_host_told(unsigned number) {
    AcquireSRWLockExclusive(&battle_lock);
    const unsigned slots = number && battle_news.told_number == number ? battle_news.told_slots : 0;
    ReleaseSRWLockExclusive(&battle_lock);
    return slots;
}
// This PC and its paired friends.
unsigned battle_party() { return 1 + party_friends.load(std::memory_order_acquire); }
// Game thread: the shell script's GCOBS:versus:* reports.
void versus_event(const char* event) {
    // load:<side>:<from>: the side a loaded Versus galaxy gives this player, for the log.
    unsigned side = 0;
    char from[16]{};
    if (sscanf_s(event, "load:%u:%15[a-z]", &side, from, static_cast<unsigned>(sizeof from)) == 2) {
        log("VERSUS_LOAD side=%u from=%s", side, from);
        return;
    }
    AcquireSRWLockExclusive(&versus_lock);
    unsigned value = 0, turn = 0;
    if (!strcmp(event, "sides")) ++versus_sides.sequence;  // the side screen is open: send the pick again
    else if (!strcmp(event, "reset")) versus_sides.reset();
    else if (sscanf_s(event, "era:%u", &value) == 1) {
        if ((value == 1 || value == 2) && value != versus_sides.era) {
            versus_sides.era = static_cast<uint8_t>(value);
            ++versus_sides.sequence;
        }
    } else if (sscanf_s(event, "pick:%u", &value) == 1) versus_sides.pick(static_cast<uint8_t>(value));
    else if (sscanf_s(event, "ready:%u", &value) == 1) versus_sides.set_ready(value == 1);
    else if (!strcmp(event, "started")) {
        if (versus_sides.both_ready() && !versus_sides.started) {
            versus_sides.started = true;
            ++versus_sides.sequence;
        }
    } else if (sscanf_s(event, "over:%u:%u", &value, &turn) == 2 && (value == 1 || value == 2) && turn) {
        versus_over_winner = static_cast<uint8_t>(value);
        versus_over_turn = turn;
    } else if (!strncmp(event, "card_ask:", 9)) {
        // card_ask:<mission>:<c1>,<c2>,<c3>: the attacker's card screen has reached the defender's pass.
        char mission[32]{};
        unsigned c[3]{};
        if (sscanf_s(event + 9, "%31[A-Za-z0-9_]:%u,%u,%u", mission, static_cast<unsigned>(sizeof mission), &c[0],
                     &c[1], &c[2]) == 4 &&
            c[0] < 0x10000 && c[1] < 0x10000 && c[2] < 0x10000) {
            const uint16_t cards[3] = {static_cast<uint16_t>(c[0]), static_cast<uint16_t>(c[1]),
                                       static_cast<uint16_t>(c[2])};
            card_ask.ask(mission, cards);
        }
    } else if (sscanf_s(event, "card:%u:%u", &value, &turn) == 2) {
        // card:<request>:<slot>: the defender's pick, 0 for none.
        card_peer.answer(value, static_cast<uint8_t>(turn > gc::versus::card_slots ? 0xFF : turn));
    }
    const gc::versus::Sides now = versus_sides;
    ReleaseSRWLockExclusive(&versus_lock);
    log("VERSUS_EVENT %s side=%u ready=%d other_side=%u other_ready=%d era=%u started=%d", event, now.mine.team,
        now.mine.ready, now.theirs.team, now.theirs.ready, now.era, now.started);
}
// Game thread: the shell script's GCOBS:ask:versus_* questions.
bool versus_answer(const char* question) {
    if (!strcmp(question, "versus")) return versus_flag.load(std::memory_order_acquire);
    if (!strcmp(question, "versus_host")) return versus_host_flag.load(std::memory_order_acquire);
    if (!strcmp(question, "versus_paired")) return versus_pair_flag.load(std::memory_order_acquire);
    if (!strcmp(question, "versus_over_1")) return versus_winner.load(std::memory_order_acquire) == 1;
    if (!strcmp(question, "versus_over_2")) return versus_winner.load(std::memory_order_acquire) == 2;
    AcquireSRWLockShared(&versus_lock);
    const gc::versus::Sides now = versus_sides;
    ReleaseSRWLockShared(&versus_lock);
    if (!strcmp(question, "versus_heard")) return now.heard;
    if (!strcmp(question, "versus_peer_1")) return now.theirs.team == 1;
    if (!strcmp(question, "versus_peer_2")) return now.theirs.team == 2;
    if (!strcmp(question, "versus_peer_ready")) return now.theirs.ready;
    if (!strcmp(question, "versus_mine_1")) return now.mine.team == 1;
    if (!strcmp(question, "versus_mine_2")) return now.mine.team == 2;
    if (!strcmp(question, "versus_mine_ready")) return now.mine.ready;
    if (!strcmp(question, "versus_go")) return now.both_ready();
    if (!strcmp(question, "versus_clash")) return now.clash();
    if (!strcmp(question, "versus_started")) return now.started;
    if (!strcmp(question, "versus_era_1")) return now.era == 1;
    if (!strcmp(question, "versus_era_2")) return now.era == 2;
    // The attacker's card screen waits for the answer; the defender's closes once the request is no
    // longer open (answered, or the attacker's battle has begun).
    if (!strncmp(question, "versus_card_", 12)) {
        AcquireSRWLockShared(&versus_lock);
        const bool answered = card_ask.answered_now(), open = card_peer.open();
        const uint8_t slot = card_ask.slot;
        ReleaseSRWLockShared(&versus_lock);
        if (!strcmp(question, "versus_card_answered")) return answered;
        if (!strcmp(question, "versus_card_open")) return open;
        unsigned k = 0;
        if (sscanf_s(question, "versus_card_slot_%u", &k) == 1)
            return answered && k <= gc::versus::card_slots && slot == k;
    }
    return false;
}
// Game thread: whether this game is Versus, this PC is the session host, and a Versus session is ready.
bool versus_active() { return versus_flag.load(std::memory_order_acquire); }
bool versus_session_host() { return versus_host_flag.load(std::memory_order_acquire); }
bool versus_paired() { return versus_pair_flag.load(std::memory_order_acquire); }
// Defender's game thread: the other PC's open card request not shown yet, as its number and
// "<mission>\n<slot 1>,<slot 2>,<slot 3>".
bool versus_card_pending(unsigned& number, std::string& data) {
    AcquireSRWLockShared(&versus_lock);
    const bool pending = card_peer.pending();
    if (pending) {
        number = card_peer.number;
        char cards[32];
        _snprintf_s(cards, sizeof cards, _TRUNCATE, "%u,%u,%u", card_peer.cards[0], card_peer.cards[1],
                    card_peer.cards[2]);
        data = card_peer.mission + "\n" + cards;
    }
    ReleaseSRWLockShared(&versus_lock);
    return pending;
}
// The defender's game has shown request `number`.
void versus_card_taken(unsigned number) {
    AcquireSRWLockExclusive(&versus_lock);
    card_peer.take(number);
    ReleaseSRWLockExclusive(&versus_lock);
}
// Game thread: whether the host's galaxy is held now, and the hold popup's events.
bool hold_wanted() { return hold_published.load(std::memory_order_acquire); }
void hold_event(const char* event) {
    unsigned bit = 0;
    if (!strcmp(event, "shown")) bit = hold_shown;
    else if (!strcmp(event, "cancel")) bit = hold_cancelled;
    else if (!strcmp(event, "released")) bit = hold_popup_released;
    else if (!strcmp(event, "timeout")) bit = hold_popup_timeout;
    if (bit) hold_events.fetch_or(bit, std::memory_order_acq_rel);
}

// Prepares the status folder and starts the Steam thread; false if already running in this process.
bool start_steam_invite(HMODULE self, HMODULE game) {
    pid = GetCurrentProcessId();
    wchar_t name[64];
    swprintf_s(name, L"Local\\GCT_SteamInvite_%lu", pid);
    SetLastError(ERROR_SUCCESS);
    HANDLE single = CreateMutexW(nullptr, FALSE, name);  // held until the process exits
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single) CloseHandle(single);
        log("STEAM_INVITE REJECT another instance is active in this process");
        return false;
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    // Whether this game has the Remaster, for HELLO.
    const std::wstring remaster = std::wstring(path) + L"\\addon\\Remaster\\remaster_hook.lvl";
    hello_text = s::hello_body(GetFileAttributesW(remaster.c_str()) != INVALID_FILE_ATTRIBUTES);
    log("STEAM_INVITE hello=%s", hello_text.c_str());
#if GC_PLAYER
    // Player builds keep their files in gct\steam beside the DLL.
    dir = std::wstring(path) + L"\\gct";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\steam";
#else
    dir = std::wstring(path) + L"\\steam";
#endif
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    // Process ids are reused: files under this one belong to an exited game, and an old request must not
    // run in this one.
    for (const wchar_t* stem : {L"status", L"request", L"relay", L"offer"})
        for (const wchar_t* ext : {L".json", L".txt", L".json.tmp"}) DeleteFileW(path_for(stem, ext).c_str());
#if GC_PLAYER
    remove_stale_files();
#endif
    InitializeCriticalSection(&pump_lock);
    game_module = game;
    HANDLE thread = CreateThread(nullptr, 0, steam_thread, nullptr, 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);
    return true;
}
}
