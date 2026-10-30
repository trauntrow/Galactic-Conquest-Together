#pragma once
// Pure helpers of the Steam layer (no Steam, Windows or game calls), unit-tested directly.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gc::steam {
// Marks a GCT lobby (its "gct" data) and prefixes every lobby chat message.
constexpr char protocol[] = "GCT1";
// The game build and the co-op protocol revision, compared exactly in the lobby data and HELLO.
constexpr char build_id[] = "6217431 coop-12";
// HELLO ends in " remaster" when the Remaster addon is installed. It adds a bonus card, so card tables
// and saves only agree when both PCs have it or neither does. The lobby's gct_build is build_id alone.
inline std::string hello_body(bool remaster) { return std::string(build_id) + (remaster ? " remaster" : ""); }
// Longest offer body, and longest lobby chat message accepted.
constexpr unsigned max_offer = 1024;
constexpr unsigned max_message = 1100;
// Steamworks callback ids, from the SDK whose interface versions the game uses.
constexpr int k_lobby_created = 513, k_lobby_enter = 504, k_lobby_chat_update = 506, k_lobby_chat_msg = 507,
              k_lobby_join_requested = 333, k_rich_presence_join = 337, k_p2p_session_request = 1202,
              k_p2p_connect_fail = 1203;

// Public-universe (1) SteamIDs with a nonzero account: type 8 is a chat (lobby), type 1 a user.
inline bool valid_lobby_id(uint64_t id) {
    return (id >> 56) == 1 && ((id >> 52) & 0xF) == 8 && (id & 0xFFFFFFFFull) != 0;
}
inline bool valid_user_id(uint64_t id) {
    return (id >> 56) == 1 && ((id >> 52) & 0xF) == 1 && (id & 0xFFFFFFFFull) != 0;
}

// A co-op lobby holds the host and up to three friends; a Versus lobby holds two.
constexpr int max_friends = 3;
inline int lobby_limit(bool versus) { return versus ? 2 : 1 + max_friends; }
// The host's lobby stays open until it is full, also once its co-op game has started.
inline bool lobby_joinable(int members, int limit) { return members < limit; }
// The host accepts HELLO from any other member; a friend's PC only from its host (the lobby owner
// when it joins), so friends never pair with each other.
inline bool takes_hello(bool host, uint64_t user, uint64_t self, uint64_t host_id) {
    return valid_user_id(user) && user != self && (host || user == host_id);
}

// Parses up to 20 decimal digits; false on anything else or on overflow.
inline bool parse_u64(const char* p, size_t n, uint64_t& out) {
    if (!n || n > 20) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        const uint64_t d = static_cast<uint64_t>(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// Finds "+connect_lobby <id>" in a command line or rich-presence connect string. Only a whole token
// carrying a valid lobby SteamID counts.
inline bool parse_connect_lobby(const char* text, uint64_t& lobby) {
    static const char key[] = "+connect_lobby";
    if (!text) return false;
    for (const char* at = strstr(text, key); at; at = strstr(at + 1, key)) {
        if (at != text && at[-1] != ' ' && at[-1] != '\t' && at[-1] != '"') continue;
        const char* p = at + sizeof key - 1;
        if (*p != ' ' && *p != '\t') continue;
        while (*p == ' ' || *p == '\t') ++p;
        const char* end = p;
        while (*end >= '0' && *end <= '9') ++end;
        if (*end && *end != ' ' && *end != '\t' && *end != '"') continue;
        uint64_t id = 0;
        if (parse_u64(p, static_cast<size_t>(end - p), id) && valid_lobby_id(id)) {
            lobby = id;
            return true;
        }
    }
    return false;
}

// Offers travel through lobby chat into the JSON status file, so only printable ASCII is accepted.
inline bool safe_offer(const char* p, size_t n) {
    if (!n || n > max_offer) return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    return true;
}

// Lobby chat messages: "GCT1 HELLO <build>" and "GCT1 OFFER <text>".
enum class Kind { none, hello, offer };

inline std::string encode_message(Kind kind, const std::string& body) {
    return std::string(protocol) + (kind == Kind::hello ? " HELLO " : " OFFER ") + body;
}

// Decodes either message; anything else, too long or unprintable is Kind::none.
inline Kind decode_message(const char* data, size_t n, std::string& body) {
    if (n && data[n - 1] == '\0') --n;  // the sender's NUL terminator
    if (n > max_message) return Kind::none;
    static const char hello[] = "GCT1 HELLO ", offer[] = "GCT1 OFFER ";
    const size_t h = sizeof hello - 1, o = sizeof offer - 1;
    if (n > h && !memcmp(data, hello, h)) {
        if (!safe_offer(data + h, n - h) || n - h > 32) return Kind::none;
        body.assign(data + h, n - h);
        return Kind::hello;
    }
    if (n > o && !memcmp(data, offer, o)) {
        if (!safe_offer(data + o, n - o)) return Kind::none;
        body.assign(data + o, n - o);
        return Kind::offer;
    }
    return Kind::none;
}

// Escapes a JSON string value. UTF-8 bytes (persona names) pass through unchanged.
inline std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            out += u;
        } else out += static_cast<char>(c);
    }
    return out;
}

// A peer as the status file shows it: on the host each friend in its slot, on a friend's PC its host.
// Paired once its HELLO matches this PC's.
struct PeerStatus {
    uint64_t id = 0;
    unsigned index = 0;  // host: the friend's slot, 1 to 3; 0 on a friend's PC
    std::string name, build, offer;
    bool build_match = false, offer_valid = false;
    bool left_lobby = false;      // it has left the lobby; a paired peer stays on P2P
    std::string session = "off";  // off, opening, ready
    uint32_t session_id = 0;
    unsigned sent = 0, acked = 0;  // revisions sent to it and acknowledged
};

// A friend that has left or joined the host's co-op game in progress, for the galaxy to announce once.
// Notes are numbered so the shell script can acknowledge each one it has shown.
struct PartyNote {
    unsigned seq = 0, slot = 0;
    std::string name;
    bool joined = false;
};

// The layer's state, as the status file and the Online menu see it.
struct Status {
    bool ready = false;
    unsigned app_id = 0;
    bool overlay = false;
    std::string state = "starting";  // starting, idle, creating, hosting, joining, joined, left, error
    std::string role;                // host, guest
    uint64_t self = 0, lobby = 0, peer = 0;
    std::string peer_name, peer_build, peer_offer, local_offer, error;
    std::vector<PeerStatus> peers;  // peer, peer_name etc. describe the first one (summarize)
    std::vector<PartyNote> notes;   // party notes the galaxy has not shown yet
    bool peer_offer_valid = false, offer_sent = false, build_match = false;
    int members = 0;
    unsigned revision = 0;
    // Developer relay of the bridge's TCP stream over Steam P2P.
    std::string relay = "off";  // off, listening, ready, connecting, open, failed
    unsigned relay_port = 0, relay_streams = 0;
    uint64_t relay_up = 0, relay_down = 0;
    // Snapshot session (session_logic.h).
    std::string session = "off";  // off, opening, ready
    uint32_t session_id = 0;
    unsigned session_latest = 0, session_sent = 0, session_acked = 0, session_received = 0;
    std::string session_digest;  // host: the newest revision's hash; friend: the held revision's
    // The friend's apply loop (apply_logic.h).
    std::string apply = "off";  // off, waiting, settling, committing, confirming, stopped
    unsigned apply_offered = 0, apply_applied = 0;
    std::string apply_reason;
    bool apply_in_galaxy = false;  // the host's galaxy shows on this PC
    bool apply_pending = false;    // a newer revision waits to load
    // Why the last co-op game ended; empty while one runs.
    std::string session_end;
    // "coop" or "versus": the host's from its invite, a friend's from the lobby data.
    std::string mode = "coop";
};

// Fills the single peer fields, which tools and the friend's menus read, from the first paired peer
// (the lowest slot; a friend's PC has only its host), else from the first one listed.
inline void summarize(Status& s) {
    const PeerStatus* first = nullptr;
    for (const PeerStatus& p : s.peers)
        if (p.build_match) {
            first = &p;
            break;
        }
    if (!first && !s.peers.empty()) first = &s.peers.front();
    s.peer = first ? first->id : 0;
    s.peer_name = first ? first->name : std::string();
    s.peer_build = first ? first->build : std::string();
    s.build_match = first && first->build_match;
    s.peer_offer = first ? first->offer : std::string();
    s.peer_offer_valid = first && first->offer_valid;
}

// Answers to the Online menu's popups (player_menu.lua), one bit each. The host waits for paired or
// failed; a friend's PC reports its wait, a build mismatch, a failed join or the game's end.
enum MenuAnswer : unsigned {
    menu_paired = 1,
    menu_failed = 2,
    menu_build = 4,
    menu_guest = 8,
    menu_no_overlay = 16,
    menu_guest_wait = 32,
    menu_guest_build = 64,
    menu_join_failed = 128,
    menu_guest_ended = 256,
    menu_party_invite = 512
};
// The answers that hold now. The host is paired once any friend's session is ready; a friend whose
// build differs fails the invite only while nobody is paired.
inline unsigned menu_answers(const Status& s) {
    unsigned a = s.ready && !s.overlay ? menu_no_overlay : 0;
    if (s.role == "guest") {
        if (s.state == "joining" || s.state == "joined") a |= menu_failed | menu_guest;
        if (s.state == "joined" && s.build_match && s.session == "ready" && !s.apply_in_galaxy && !s.apply_pending &&
            s.apply != "stopped")
            a |= menu_guest_wait;
        if (s.peer && !s.peer_build.empty() && !s.build_match) a |= menu_guest_build;
        if (s.state == "error") a |= s.error == "lobby_build_mismatch" ? menu_guest_build : menu_join_failed;
        if (s.state == "left" && !s.session_end.empty()) a |= menu_guest_ended;
        return a;
    }
    if (s.role != "host") return a;
    if (s.state == "error") return a | menu_failed;
    if (s.state == "hosting" && s.mode == "coop" && s.lobby && s.members < lobby_limit(false)) a |= menu_party_invite;
    bool differs = false;
    for (const PeerStatus& p : s.peers) {
        if (p.build_match && p.session == "ready") return a | menu_paired;
        if (!p.build.empty() && !p.build_match) differs = true;
    }
    return differs ? a | menu_failed | menu_build : a;
}
// The bit for a GCOBS:ask question name; 0 if unknown.
inline unsigned menu_answer_bit(const char* question) {
    if (!question) return 0;
    if (!strcmp(question, "paired")) return menu_paired;
    if (!strcmp(question, "failed")) return menu_failed;
    if (!strcmp(question, "build")) return menu_build;
    if (!strcmp(question, "guest")) return menu_guest;
    if (!strcmp(question, "no_overlay")) return menu_no_overlay;
    if (!strcmp(question, "guest_wait")) return menu_guest_wait;
    if (!strcmp(question, "guest_build")) return menu_guest_build;
    if (!strcmp(question, "join_failed")) return menu_join_failed;
    if (!strcmp(question, "guest_ended")) return menu_guest_ended;
    if (!strcmp(question, "party_invite")) return menu_party_invite;
    return 0;
}

// A friend's name as the shell can show it. ScriptCB_tounicode copies bytes one by one, so a name that
// is not printable ASCII, or not known yet, becomes "Friend 2" to "Friend 4"; long ones are cut.
constexpr size_t party_name_max = 24;
inline std::string party_name(const std::string& name, unsigned index) {
    bool plain = !name.empty();
    for (unsigned char c : name) plain = plain && c >= 0x20 && c <= 0x7E;
    return plain ? name.substr(0, party_name_max) : "Friend " + std::to_string(index + 1);
}
// The host's party screen rows: the member count, then "<slot>:<state>:<name>" per friend (ready,
// build or joining), then "<L|J><seq>:<slot>:<name>" per friend that left (L) or joined (J).
inline std::string party_rows(const Status& s) {
    std::string rows = std::to_string(s.members) + "\n";
    for (const PeerStatus& p : s.peers) {
        const char* state = p.build_match && p.session == "ready" ? "ready"
                            : !p.build.empty() && !p.build_match  ? "build"
                                                                  : "joining";
        rows += std::to_string(p.index) + ":" + state + ":" + party_name(p.name, p.index) + "\n";
    }
    for (const PartyNote& n : s.notes)
        rows += (n.joined ? "J" : "L") + std::to_string(n.seq) + ":" + std::to_string(n.slot) + ":" + n.name + "\n";
    return rows;
}

// Developer relay frames on one reliable P2P channel: type, LE epoch, payload. P opens the P2P
// session, O asks the target to connect to its bridge, A accepts, D carries bytes, C closes.
constexpr int relay_channel = 7;
constexpr size_t relay_header = 5, relay_max_payload = 16384;
enum class Frame : char { ping = 'P', open = 'O', accept = 'A', data = 'D', close = 'C' };

inline std::string encode_frame(Frame type, uint32_t epoch, const char* data = nullptr, size_t n = 0) {
    std::string f(relay_header + n, char(0));
    f[0] = static_cast<char>(type);
    for (int i = 0; i < 4; ++i) f[1 + i] = static_cast<char>((epoch >> (8 * i)) & 0xFF);
    if (n) memcpy(&f[relay_header], data, n);
    return f;
}

// Accepts a known type, a payload on D only, and a nonzero epoch on everything but P.
inline bool decode_frame(const char* p, size_t n, Frame& type, uint32_t& epoch, size_t& payload) {
    if (n < relay_header || n > relay_header + relay_max_payload) return false;
    const char t = p[0];
    if (t != 'P' && t != 'O' && t != 'A' && t != 'D' && t != 'C') return false;
    payload = n - relay_header;
    if (t != 'D' && payload) return false;
    if (t == 'D' && !payload) return false;
    type = static_cast<Frame>(t);
    epoch = 0;
    for (int i = 0; i < 4; ++i) epoch |= static_cast<uint32_t>(static_cast<unsigned char>(p[1 + i])) << (8 * i);
    return t == 'P' || epoch != 0;
}

// relay-<pid>.txt: "host" (listen on loopback for the host bridge) or "target <port>" (the target
// bridge listens on 127.0.0.1:<port>).
inline bool parse_relay_request(const std::string& text, bool& host, unsigned& port) {
    if (text == "host") {
        host = true;
        port = 0;
        return true;
    }
    if (text.rfind("target ", 0) != 0) return false;
    uint64_t v = 0;
    const std::string digits = text.substr(7);
    if (!parse_u64(digits.data(), digits.size(), v) || v < 1024 || v > 65535) return false;
    host = false;
    port = static_cast<unsigned>(v);
    return true;
}

// A SteamID as decimal text; empty for 0.
inline std::string id_text(uint64_t id) { return id ? std::to_string(id) : std::string(); }

// The status-<pid>.json document the external tools read.
inline std::string format_status(const Status& s, unsigned long pid) {
    std::string j = "{\n";
    auto str = [&](const char* k, const std::string& v, bool last = false) {
        j += "  \"";
        j += k;
        j += "\": \"";
        j += json_escape(v);
        j += last ? "\"\n" : "\",\n";
    };
    auto raw = [&](const char* k, const std::string& v) {
        j += "  \"";
        j += k;
        j += "\": ";
        j += v;
        j += ",\n";
    };
    raw("version", "1");
    raw("pid", std::to_string(pid));
    raw("revision", std::to_string(s.revision));
    raw("ready", s.ready ? "true" : "false");
    raw("app_id", std::to_string(s.app_id));
    raw("overlay", s.overlay ? "true" : "false");
    str("state", s.state);
    str("role", s.role);
    str("self", id_text(s.self));
    str("lobby", id_text(s.lobby));
    raw("members", std::to_string(s.members));
    str("peer", id_text(s.peer));
    str("peer_name", s.peer_name);
    str("peer_build", s.peer_build);
    raw("build_match", s.build_match ? "true" : "false");
    if (s.peer_offer_valid) str("peer_offer", s.peer_offer);
    else raw("peer_offer", "null");
    raw("offer_sent", s.offer_sent ? "true" : "false");
    str("relay", s.relay);
    raw("relay_port", std::to_string(s.relay_port));
    raw("relay_streams", std::to_string(s.relay_streams));
    raw("relay_up", std::to_string(s.relay_up));
    raw("relay_down", std::to_string(s.relay_down));
    str("session", s.session);
    raw("session_id", std::to_string(s.session_id));
    raw("session_latest", std::to_string(s.session_latest));
    raw("session_sent", std::to_string(s.session_sent));
    raw("session_acked", std::to_string(s.session_acked));
    raw("session_received", std::to_string(s.session_received));
    str("session_digest", s.session_digest);
    str("apply", s.apply);
    raw("apply_offered", std::to_string(s.apply_offered));
    raw("apply_applied", std::to_string(s.apply_applied));
    str("apply_reason", s.apply_reason);
    raw("apply_in_galaxy", s.apply_in_galaxy ? "true" : "false");
    str("session_end", s.session_end);
    str("mode", s.mode);
    // Then every peer, one line each.
    j += "  \"peers\": [";
    for (size_t i = 0; i < s.peers.size(); ++i) {
        const PeerStatus& p = s.peers[i];
        j += i ? ",\n    {" : "\n    {";
        j += "\"id\": \"" + id_text(p.id) + "\", \"index\": " + std::to_string(p.index) + ", \"name\": \"" +
             json_escape(p.name) + "\", \"build\": \"" + json_escape(p.build) +
             "\", \"build_match\": " + (p.build_match ? "true" : "false") +
             ", \"left_lobby\": " + (p.left_lobby ? "true" : "false") + ", \"session\": \"" + json_escape(p.session) +
             "\", \"session_id\": " + std::to_string(p.session_id) + ", \"sent\": " + std::to_string(p.sent) +
             ", \"acked\": " + std::to_string(p.acked) + "}";
    }
    j += s.peers.empty() ? "],\n" : "\n  ],\n";
    str("error", s.error, true);
    j += "}\n";
    return j;
}
}
