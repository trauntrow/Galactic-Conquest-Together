// Test double for steam_api.dll: the flat API functions the invite layer binds, plus a scripted peer.
// Built as steam_api.dll beside the harness, which drives it through the FakeSteam_* exports.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <deque>
#include <thread>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using u64 = uint64_t;
#define API extern "C" __declspec(dllexport)

// Same layout as the Steamworks CCallbackBase, so the layer's callback objects run here.
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

namespace {
// The lobby, this process's Steam user and its scripted peer.
constexpr u64 lobby_id = 109775241021786432ull, self_id = 76561197960287930ull, peer_id = 76561197960287931ull;
// Steam callback payloads, packed as steam_api packs them.
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
#pragma pack(pop)
// A queued callback (call 0) or call result, and a lobby chat entry.
struct Pending {
    int id;
    u64 call;
    std::vector<unsigned char> data;
};
struct Message {
    u64 user;
    std::string text;
};

// The fake's state, guarded by `lock`.
std::recursive_mutex lock;
std::vector<Pending> queue;
std::map<int, CallbackBase*> broadcast;
std::map<u64, CallbackBase*> results;
std::vector<Message> chat;
std::vector<std::string> sent;
std::string presence_connect;
bool joinable = true, game_call_done = false, created = false;
int members = 1, member_limit = 0;
DWORD main_thread = 0;
volatile LONG worker_runs = 0;  // RunCallbacks calls made off the main thread
char scenario[32] = "host";     // the harness scenario, from GCT_FAKE_SCENARIO
char dummy[4][16];              // interface pointers handed to the layer

// Queues a callback (call 0) or a call result for the next RunCallbacks.
template <class T> void push(int id, u64 call, const T& value) {
    Pending p{id, call, std::vector<unsigned char>(sizeof value)};
    memcpy(p.data.data(), &value, sizeof value);
    queue.push_back(p);
}
// Adds a lobby chat entry and queues its LobbyChatMsg callback.
void say(u64 user, const char* text) {
    chat.push_back({user, text});
    push(507, 0, LobbyChatMsg{lobby_id, user, 1, static_cast<uint32_t>(chat.size() - 1)});
}
void load_scenario() {
    if (!GetEnvironmentVariableA("GCT_FAKE_SCENARIO", scenario, sizeof scenario)) strcpy_s(scenario, "host");
}
bool send_all(SOCKET sock, const char* p, int n) {
    while (n > 0) {
        const int k = ::send(sock, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}
bool recv_all(SOCKET sock, char* p, int n) {
    while (n > 0) {
        const int k = recv(sock, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}
// A P2P packet waiting to be read.
struct Packet {
    int channel;
    std::string data;
    u64 remote;
};
std::deque<Packet> inbox;
bool deaf = false;  // set by FakeSteam_Deaf: the peer's game is gone and nothing more arrives

// Two-process P2P: both fakes share a loopback TCP link (port from GCT_FAKE_P2P_PORT) carrying
// [u32 length][u8 channel][payload] records. Host scenarios listen; each side sees the other as peer_id.
SOCKET link = INVALID_SOCKET;
std::vector<std::string> outbox;
void start_link() {
    char port_text[16];
    if (!GetEnvironmentVariableA("GCT_FAKE_P2P_PORT", port_text, sizeof port_text)) return;
    const bool listener = !strcmp(scenario, "relay_host") || !strcmp(scenario, "session_host") ||
                          !strcmp(scenario, "hostgone_host") || !strcmp(scenario, "hostends_host") ||
                          !strcmp(scenario, "friendends_host") || !strcmp(scenario, "hostcloses_host") ||
                          !strcmp(scenario, "friendcloses_host") || !strcmp(scenario, "friendgone_host") ||
                          !strcmp(scenario, "battle_host") || !strcmp(scenario, "versus_host") ||
                          !strcmp(scenario, "versusbattle_host") || !strcmp(scenario, "versusload_host") ||
                          !strcmp(scenario, "versushostcloses_host") || !strcmp(scenario, "versusfriendcloses_host");
    const u_short port = static_cast<u_short>(atoi(port_text));
    std::thread([listener, port] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        SOCKET sock = INVALID_SOCKET;
        if (listener) {
            SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(l, 1)) return;
            sock = accept(l, nullptr, nullptr);
            closesocket(l);
        } else {
            for (int i = 0; i < 200 && sock == INVALID_SOCKET; ++i) {
                SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (!::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a)) sock = c;
                else {
                    closesocket(c);
                    Sleep(50);
                }
            }
        }
        if (sock == INVALID_SOCKET) return;
        {
            std::lock_guard<std::recursive_mutex> g(lock);
            link = sock;
            for (const std::string& f : outbox) send_all(link, f.data(), static_cast<int>(f.size()));
            outbox.clear();
        }
        for (;;) {
            uint32_t n = 0;
            char channel = 0;
            if (!recv_all(sock, reinterpret_cast<char*>(&n), 4) || !recv_all(sock, &channel, 1) || n > (1u << 20))
                return;
            std::string data(n, '\0');
            if (n && !recv_all(sock, &data[0], static_cast<int>(n))) return;
            std::lock_guard<std::recursive_mutex> g(lock);
            if (!deaf) inbox.push_back({channel, std::move(data), peer_id});
        }
    }).detach();
}

// Hub mode: three or four processes share one lobby, each its own Steam user (self_id + GCT_FAKE_INDEX).
// Index 0 also runs the hub on GCT_FAKE_HUB_PORT, which keeps the lobby and routes chat and packets.
constexpr int hub_max = 4;
bool hub = false;
int me = 0;
// Steam IDs of hub members: self_id plus the member's index.
u64 hub_id(int i) { return self_id + static_cast<u64>(i); }
int hub_index(u64 id) { return id >= self_id && id < self_id + hub_max ? static_cast<int>(id - self_id) : -1; }
// Writes to the hub take hub_write alone, so a large packet never holds `lock` while the socket is busy.
std::mutex hub_write;
SOCKET hub_link = INVALID_SOCKET;     // this process's connection to the hub
std::vector<std::string> hub_outbox;  // records queued before it connects
bool hub_member[hub_max] = {};        // the lobby's members, as this process last heard
// Hub record: [u32 length][u8 type][u8 from][u8 to][u8 arg][payload]. To the hub: H hello, N create,
// J join, L leave, O joinable, M limit, C chat, P packet. From it: A entered, X refused, U member, C, P.
std::string record(char type, int from, int to, int arg, const std::string& payload = std::string()) {
    std::string r(8 + payload.size(), '\0');
    const uint32_t n = static_cast<uint32_t>(payload.size());
    memcpy(&r[0], &n, 4);
    r[4] = type;
    r[5] = static_cast<char>(from);
    r[6] = static_cast<char>(to);
    r[7] = static_cast<char>(arg);
    if (n) memcpy(&r[8], payload.data(), n);
    return r;
}
// Sends a record to the hub, or queues it until this process has connected.
void hub_send(const std::string& r) {
    std::lock_guard<std::mutex> g(hub_write);
    if (hub_link == INVALID_SOCKET) hub_outbox.push_back(r);
    else send_all(hub_link, r.data(), static_cast<int>(r.size()));
}
int hub_members() {
    int n = 0;
    for (bool m : hub_member) n += m ? 1 : 0;
    return n;
}
// Applies a record from the hub in this process.
void hub_deliver(char type, int from, int arg, std::string payload) {
    std::lock_guard<std::recursive_mutex> g(lock);
    switch (type) {
    case 'A':
        for (bool& m : hub_member) m = false;
        for (char c : payload)
            if (c >= 0 && c < hub_max) hub_member[static_cast<int>(c)] = true;
        members = hub_members();
        if (arg == 1) push(513, 0x1001, LobbyCreated{1, lobby_id});
        else push(504, 0x2001, LobbyEnter{lobby_id, 0, false, 1});
        break;
    case 'X': push(504, 0x2001, LobbyEnter{lobby_id, 0, false, 4}); break;  // response 4: the lobby is full
    case 'U':
        if (from < 0 || from >= hub_max) break;
        hub_member[from] = arg == 1;
        members = hub_members();
        push(506, 0, LobbyChatUpdate{lobby_id, hub_id(from), hub_id(from), static_cast<uint32_t>(arg)});
        break;
    case 'C': say(hub_id(from), payload.c_str()); break;
    case 'P':
        if (!deaf) inbox.push_back({arg, std::move(payload), hub_id(from)});
        break;
    default: break;
    }
}
// Reads one record; false once the connection ends.
bool read_record(SOCKET s, char& type, int& from, int& to, int& arg, std::string& payload) {
    char head[8];
    if (!recv_all(s, head, 8)) return false;
    uint32_t n = 0;
    memcpy(&n, head, 4);
    if (n > (1u << 20)) return false;
    type = head[4];
    from = head[5];
    to = head[6];
    arg = static_cast<unsigned char>(head[7]);
    payload.assign(n, '\0');
    return !n || recv_all(s, &payload[0], static_cast<int>(n));
}

// The hub's lobby, in the host's process. Its own lock: the processes' fakes never wait on each other.
struct Hub {
    std::mutex lock;
    SOCKET socks[hub_max] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
    bool made = false, joinable = false, in[hub_max] = {};
    int owner = -1, limit = 0;
    unsigned friend_packets = 0;             // P2P packets between two friends (expected 0)
    unsigned tunnel[hub_max][hub_max] = {};  // battle tunnel packets (channel 9), by sender and recipient
} the_hub;
void hub_to(int i, const std::string& r) {  // callers hold the hub's lock
    if (i >= 0 && i < hub_max && the_hub.socks[i] != INVALID_SOCKET)
        send_all(the_hub.socks[i], r.data(), static_cast<int>(r.size()));
}
std::string hub_list() {
    std::string list;
    for (int i = 0; i < hub_max; ++i)
        if (the_hub.in[i]) list += static_cast<char>(i);
    return list;
}
void hub_left(int i) {  // callers hold the hub's lock
    if (!the_hub.in[i]) return;
    the_hub.in[i] = false;
    for (int j = 0; j < hub_max; ++j)
        if (the_hub.in[j]) hub_to(j, record('U', i, j, 2));
}
// Handles a record from member `from` as Steam's lobby and P2P would.
void hub_route(int from, char type, int to, int arg, const std::string& payload) {
    std::lock_guard<std::mutex> g(the_hub.lock);
    int count = 0;
    for (bool m : the_hub.in) count += m ? 1 : 0;
    switch (type) {
    case 'N':
        the_hub.made = true;
        the_hub.owner = from;
        the_hub.limit = arg;
        the_hub.joinable = true;
        for (bool& m : the_hub.in) m = false;
        the_hub.in[from] = true;
        hub_to(from, record('A', 0, from, 1, hub_list()));
        break;
    case 'J':
        if (!the_hub.made || !the_hub.joinable || count >= the_hub.limit || the_hub.in[from]) {
            hub_to(from, record('X', 0, from, 0));
            break;
        }
        the_hub.in[from] = true;
        hub_to(from, record('A', 0, from, 0, hub_list()));
        for (int j = 0; j < hub_max; ++j)
            if (j != from && the_hub.in[j]) hub_to(j, record('U', from, j, 1));
        break;
    case 'L': hub_left(from); break;
    case 'O':
        if (from == the_hub.owner) the_hub.joinable = arg != 0;
        break;
    case 'M':
        if (from == the_hub.owner) the_hub.limit = arg;
        break;
    case 'C':
        if (!the_hub.in[from]) break;
        for (int j = 0; j < hub_max; ++j)
            if (j != from && the_hub.in[j]) hub_to(j, record('C', from, j, 0, payload));
        break;
    case 'P':
        if (from != the_hub.owner && to != the_hub.owner) ++the_hub.friend_packets;
        if (arg == 9 && to >= 0 && to < hub_max) ++the_hub.tunnel[from][to];
        hub_to(to, record('P', from, to, arg, payload));
        break;
    default: break;
    }
}
// Accepts up to four members; each connection starts with H naming its index.
void start_hub_server(u_short port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(l, hub_max)) return;
    std::thread([l] {
        for (int accepted = 0; accepted < hub_max; ++accepted) {
            SOCKET c = accept(l, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::thread([c] {
                char type = 0;
                int from = 0, to = 0, arg = 0;
                std::string payload;
                if (!read_record(c, type, from, to, arg, payload) || type != 'H' || from < 0 || from >= hub_max) {
                    closesocket(c);
                    return;
                }
                const int who = from;
                {
                    std::lock_guard<std::mutex> g(the_hub.lock);
                    the_hub.socks[who] = c;
                }
                while (read_record(c, type, from, to, arg, payload)) hub_route(who, type, to, arg, payload);
                // The connection closed: its game is gone, so Steam takes it out of the lobby.
                std::lock_guard<std::mutex> g(the_hub.lock);
                hub_left(who);
                the_hub.socks[who] = INVALID_SOCKET;
            }).detach();
        }
    }).detach();
}
// Joins the hub when GCT_FAKE_HUB_PORT is set; index 0 also runs it.
void start_hub() {
    char port_text[16], index_text[8];
    if (!GetEnvironmentVariableA("GCT_FAKE_HUB_PORT", port_text, sizeof port_text)) return;
    hub = true;
    me = GetEnvironmentVariableA("GCT_FAKE_INDEX", index_text, sizeof index_text) ? atoi(index_text) : 0;
    if (me < 0 || me >= hub_max) me = 0;
    members = 0;
    const u_short port = static_cast<u_short>(atoi(port_text));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (me == 0) start_hub_server(port);
    std::thread([port] {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        SOCKET sock = INVALID_SOCKET;
        for (int i = 0; i < 200 && sock == INVALID_SOCKET; ++i) {
            SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (!::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a)) sock = c;
            else {
                closesocket(c);
                Sleep(50);
            }
        }
        if (sock == INVALID_SOCKET) return;
        {
            std::lock_guard<std::mutex> g(hub_write);
            const std::string hello = record('H', me, 0, 0);
            send_all(sock, hello.data(), static_cast<int>(hello.size()));
            hub_link = sock;
            for (const std::string& r : hub_outbox) send_all(hub_link, r.data(), static_cast<int>(r.size()));
            hub_outbox.clear();
        }
        char type = 0;
        int from = 0, to = 0, arg = 0;
        std::string payload;
        while (read_record(sock, type, from, to, arg, payload)) hub_deliver(type, from, arg, std::move(payload));
    }).detach();
}
}

// Steam's flat API, as far as the invite layer and the game use it.
API int SteamAPI_GetHSteamPipe() { return 1; }
API int SteamAPI_GetHSteamUser() { return 1; }
// The first call reads the scenario and starts the hub or the two-process link.
API void* SteamClient() {
    static bool started = false;
    if (!started) {
        started = true;
        load_scenario();
        start_hub();
        if (!hub) start_link();
    }
    return dummy[0];
}
API void* SteamAPI_ISteamClient_GetISteamNetworking(void*, int, int, const char* v) {
    return strcmp(v, "SteamNetworking005") ? nullptr : dummy[1];
}
API bool SteamAPI_ISteamNetworking_SendP2PPacket(void*, u64 remote, const void* data, uint32_t size, int type,
                                                 int channel) {
    // Reliable (2) for the session, unreliable (0, 1) for the battle tunnel, which Steam limits to
    // 1200 bytes; here every packet arrives.
    if (type < 0 || type > 2 || size > (1u << 20) || (type < 2 && size > 1200)) return false;
    if (hub) {
        const int to = hub_index(remote);
        if (to < 0 || to == me) return false;
        hub_send(record('P', me, to, channel, std::string(static_cast<const char*>(data), size)));
        return true;
    }
    std::lock_guard<std::recursive_mutex> g(lock);
    if (remote != peer_id) return false;
    std::string f(5 + size, '\0');
    memcpy(&f[0], &size, 4);
    f[4] = static_cast<char>(channel);
    if (size) memcpy(&f[5], data, size);
    if (link == INVALID_SOCKET) outbox.push_back(f);
    else if (!send_all(link, f.data(), static_cast<int>(f.size()))) return false;
    return true;
}
API bool SteamAPI_ISteamNetworking_IsP2PPacketAvailable(void*, uint32_t* size, int channel) {
    std::lock_guard<std::recursive_mutex> g(lock);
    for (const Packet& p : inbox)
        if (p.channel == channel) {
            *size = static_cast<uint32_t>(p.data.size());
            return true;
        }
    return false;
}
API bool SteamAPI_ISteamNetworking_ReadP2PPacket(void*, void* dest, uint32_t cub, uint32_t* size, u64* remote,
                                                 int channel) {
    std::lock_guard<std::recursive_mutex> g(lock);
    for (auto i = inbox.begin(); i != inbox.end(); ++i) {
        if (i->channel != channel) continue;
        const uint32_t n = static_cast<uint32_t>(i->data.size()) < cub ? static_cast<uint32_t>(i->data.size()) : cub;
        memcpy(dest, i->data.data(), n);
        *size = n;
        *remote = i->remote;
        inbox.erase(i);
        return true;
    }
    return false;
}
API bool SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(void*, u64 remote) {
    return hub ? hub_index(remote) >= 0 : remote == peer_id;
}
API bool SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(void*, u64) { return true; }
API void* SteamAPI_ISteamClient_GetISteamUser(void*, int, int, const char* v) {
    return strcmp(v, "SteamUser019") ? nullptr : dummy[1];
}
API void* SteamAPI_ISteamClient_GetISteamFriends(void*, int, int, const char* v) {
    return strcmp(v, "SteamFriends015") ? nullptr : dummy[2];
}
API void* SteamAPI_ISteamClient_GetISteamUtils(void*, int, const char* v) {
    return strcmp(v, "SteamUtils008") ? nullptr : dummy[3];
}
API void* SteamAPI_ISteamClient_GetISteamMatchmaking(void*, int, int, const char* v) {
    return strcmp(v, "SteamMatchMaking009") ? nullptr : dummy[0];
}
API u64 SteamAPI_ISteamUser_GetSteamID(void*) { return hub ? hub_id(me) : self_id; }
API unsigned SteamAPI_ISteamUtils_GetAppID(void*) { return 6060; }
API bool SteamAPI_ISteamUtils_IsOverlayEnabled(void*) { return true; }
// Only the game's own call, 0x9001, completes, once FakeSteam_CompleteGameCall has run.
API bool SteamAPI_ISteamUtils_IsAPICallCompleted(void*, u64 call, bool* failed) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (failed) *failed = false;
    return call == 0x9001 && game_call_done;
}
API u64 SteamAPI_ISteamMatchmaking_CreateLobby(void*, int type, int max) {
    std::lock_guard<std::recursive_mutex> g(lock);
    // A friends-only lobby (type 1) of four for co-op or two for Versus.
    if (type != 1 || (max != 2 && max != 4)) return 0;
    created = true;
    member_limit = max;
    joinable = true;
    if (hub) hub_send(record('N', me, 0, max));
    else push(513, 0x1001, LobbyCreated{1, lobby_id});
    return 0x1001;
}
// Outside the hub, the scripted host answers a join with its HELLO and offer.
API u64 SteamAPI_ISteamMatchmaking_JoinLobby(void*, u64 lobby) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (lobby != lobby_id) return 0;
    created = false;
    if (hub) {
        hub_send(record('J', me, 0, 0));
        return 0x2001;
    }
    push(504, 0x2001, LobbyEnter{lobby_id, 0, false, 1});
    members = 2;
    say(peer_id, "GCT1 HELLO 6217431 coop-12");
    say(peer_id, "GCT1 OFFER host=10.0.0.9");
    return 0x2001;
}
API void SteamAPI_ISteamMatchmaking_LeaveLobby(void*, u64) {
    std::lock_guard<std::recursive_mutex> g(lock);
    members = 0;
    if (hub) {
        for (bool& m : hub_member) m = false;
        hub_send(record('L', me, 0, 0));
    }
}
API bool SteamAPI_ISteamMatchmaking_SetLobbyData(void*, u64, const char*, const char*) { return true; }
// The host's lobby data: guest_badbuild sees an older build, the versus scenarios a Versus lobby.
API const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void*, u64, const char* key) {
    if (!strcmp(key, "gct")) return "GCT1";
    if (!strcmp(key, "gct_build")) return strcmp(scenario, "guest_badbuild") ? "6217431 coop-12" : "6217431";
    if (!strcmp(key, "gct_mode")) return strncmp(scenario, "versus", 6) ? "coop" : "versus";
    return "";
}
API bool SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(void*, u64 lobby, const void* body, int size) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (lobby != lobby_id || size <= 0) return false;
    const std::string text(static_cast<const char*>(body), strnlen(static_cast<const char*>(body), size));
    sent.push_back(text);
    say(hub ? hub_id(me) : self_id, text.c_str());  // Steam echoes lobby chat to the sender too
    if (hub) hub_send(record('C', me, 0, 0, text));
    return true;
}
API int SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(void*, u64, int id, u64* user, void* data, int size, int* type) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (id < 0 || id >= static_cast<int>(chat.size())) return 0;
    const std::string& t = chat[id].text;
    const int n = static_cast<int>(t.size()) + 1 < size ? static_cast<int>(t.size()) + 1 : size;
    memcpy(data, t.c_str(), n);
    *user = chat[id].user;
    *type = 1;
    return n;
}
API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void*, u64) {
    std::lock_guard<std::recursive_mutex> g(lock);
    return members;
}
API bool SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void*, u64, bool value) {
    std::lock_guard<std::recursive_mutex> g(lock);
    joinable = value;
    if (hub) hub_send(record('O', me, 0, value ? 1 : 0));
    return true;
}
// The lobby's owner: the hub's host, else this process once it has created the lobby.
API u64 SteamAPI_ISteamMatchmaking_GetLobbyOwner(void*, u64 lobby) {
    if (lobby != lobby_id) return 0;
    if (hub) return hub_id(0);
    return created ? self_id : peer_id;
}
API bool SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(void*, u64 lobby, int max) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (lobby != lobby_id || max < 1) return false;
    member_limit = max;
    if (hub) hub_send(record('M', me, 0, max));
    return true;
}
// Only the "connect" key is kept, for FakeSteam_PresenceConnect.
API bool SteamAPI_ISteamFriends_SetRichPresence(void*, const char* key, const char* value) {
    if (!strcmp(key, "connect")) presence_connect = value;
    return true;
}
API void SteamAPI_ISteamFriends_ClearRichPresence(void*) { presence_connect.clear(); }
// Persona names: Ana, Ben, Cai and Dee in the hub, else a peer name that needs JSON escaping.
API const char* SteamAPI_ISteamFriends_GetFriendPersonaName(void*, u64 id) {
    static const char* const names[hub_max] = {"Ana", "Ben", "Cai", "Dee"};
    if (hub) return hub_index(id) >= 0 ? names[hub_index(id)] : "";
    return id == peer_id ? "Peer \"Two\"" : "";
}
// Invite overlays this process opened for its lobby.
int invite_dialogs = 0;
API void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void*, u64 lobby) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (lobby == lobby_id) ++invite_dialogs;
    if (lobby != lobby_id || hub) return;  // in the hub, the friends' processes join by themselves
    // Outside the hub, the invited friend accepts at once.
    members = 2;
    push(506, 0, LobbyChatUpdate{lobby_id, peer_id, peer_id, 1});
    say(peer_id, "GCT1 HELLO 6217431 coop-12");
    say(peer_id, "GCT1 OFFER bind=10.0.0.5 code=aaaa-bbbb-cccc-dddd-eeee");
}
API void SteamAPI_RegisterCallback(CallbackBase* cb, int id) {
    std::lock_guard<std::recursive_mutex> g(lock);
    broadcast[id] = cb;
}
API void SteamAPI_UnregisterCallback(CallbackBase* cb) {
    std::lock_guard<std::recursive_mutex> g(lock);
    for (auto i = broadcast.begin(); i != broadcast.end();) i = i->second == cb ? broadcast.erase(i) : std::next(i);
}
API void SteamAPI_RegisterCallResult(CallbackBase* cb, u64 call) {
    std::lock_guard<std::recursive_mutex> g(lock);
    results[call] = cb;
}
API void SteamAPI_UnregisterCallResult(CallbackBase*, u64 call) {
    std::lock_guard<std::recursive_mutex> g(lock);
    results.erase(call);
}
// Runs the queued callbacks on the calling thread, counting calls made off the main thread.
API void SteamAPI_RunCallbacks() {
    std::vector<Pending> now;
    {
        std::lock_guard<std::recursive_mutex> g(lock);
        if (main_thread && GetCurrentThreadId() != main_thread) InterlockedIncrement(&worker_runs);
        now.swap(queue);
    }
    for (Pending& p : now) {
        CallbackBase* cb = nullptr;
        {
            std::lock_guard<std::recursive_mutex> g(lock);
            if (p.call) {
                auto i = results.find(p.call);
                if (i != results.end()) {
                    cb = i->second;
                    results.erase(i);
                }
            } else {
                auto i = broadcast.find(p.id);
                if (i != broadcast.end()) cb = i->second;
            }
        }
        if (!cb) continue;  // unclaimed results are dropped, as steam_api does
        if (p.call) cb->Run(p.data.data(), false, p.call);
        else cb->Run(p.data.data());
    }
}

// Harness controls.
API void FakeSteam_SetMainThread(DWORD id) { main_thread = id; }
API LONG FakeSteam_WorkerRuns() { return InterlockedCompareExchange(&worker_runs, 0, 0); }
// Completes the game's own call result (0x9001) with `size` bytes.
API void FakeSteam_CompleteGameCall(int size) {
    std::lock_guard<std::recursive_mutex> g(lock);
    game_call_done = true;
    Pending p{0, 0x9001, std::vector<unsigned char>(size)};
    queue.push_back(p);
}
// Lobby chat messages this process sent.
API int FakeSteam_SentCount() {
    std::lock_guard<std::recursive_mutex> g(lock);
    return static_cast<int>(sent.size());
}
API bool FakeSteam_Sent(int i, char* out, int size) {
    std::lock_guard<std::recursive_mutex> g(lock);
    if (i < 0 || i >= static_cast<int>(sent.size())) return false;
    strncpy_s(out, size, sent[i].c_str(), _TRUNCATE);
    return true;
}
// The lobby as Steam keeps it; in the hub, the host's process reads the hub's.
API bool FakeSteam_Joinable() {
    if (!hub) return joinable;
    std::lock_guard<std::mutex> g(the_hub.lock);
    return the_hub.joinable;
}
API int FakeSteam_MemberLimit() {
    if (!hub) return member_limit;
    std::lock_guard<std::mutex> g(the_hub.lock);
    return the_hub.limit;
}
// P2P packets the hub passed between two friends.
API unsigned FakeSteam_FriendPackets() {
    std::lock_guard<std::mutex> g(the_hub.lock);
    return the_hub.friend_packets;
}
// Battle tunnel packets the hub passed from one member to another.
API unsigned FakeSteam_TunnelPackets(int from, int to) {
    if (from < 0 || from >= hub_max || to < 0 || to >= hub_max) return 0;
    std::lock_guard<std::mutex> g(the_hub.lock);
    return the_hub.tunnel[from][to];
}
// How often this process opened the invite overlay for its lobby.
API int FakeSteam_InviteDialogs() {
    std::lock_guard<std::recursive_mutex> g(lock);
    return invite_dialogs;
}
// Reports a member leaving the lobby (state 2): this process's user, or the peer.
API void FakeSteam_LobbyLeft(bool self) {
    std::lock_guard<std::recursive_mutex> g(lock);
    members = self ? 0 : 1;
    const u64 who = self ? self_id : peer_id;
    push(506, 0, LobbyChatUpdate{lobby_id, who, who, 2});
}
// The peer's game quits: nothing it sends arrives any more.
API void FakeSteam_Deaf(bool on) {
    std::lock_guard<std::recursive_mutex> g(lock);
    deaf = on;
    if (on) inbox.clear();
}
// The player accepts the peer's invite in the overlay while the game runs.
API void FakeSteam_JoinRequest() {
    std::lock_guard<std::recursive_mutex> g(lock);
    push(333, 0, JoinRequested{lobby_id, peer_id});
}
// Delivers a P2P packet as if the lobby peer sent it, for forged-frame tests.
API void FakeSteam_Inject(int channel, const void* data, int size) {
    std::lock_guard<std::recursive_mutex> g(lock);
    inbox.push_back({channel, std::string(static_cast<const char*>(data), static_cast<size_t>(size)), peer_id});
}
// The connect string from rich presence, if any.
API bool FakeSteam_PresenceConnect(char* out, int size) {
    strncpy_s(out, size, presence_connect.c_str(), _TRUNCATE);
    return !presence_connect.empty();
}
