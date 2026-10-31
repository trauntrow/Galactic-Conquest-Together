// End-to-end test of the Steam invite layer against the fake steam_api.dll (tests/fake_steam_api.cpp).
// Each run plays one scenario; this exe imports steam_api as the game does, so the IAT hooks run too.
#include <winsock2.h>
#include "../src/steam_invite.h"
#include "../src/diagnostics.h"
#include "../src/capture_decode.h"
#include "../src/capture_live.h"
#include "../src/session_logic.h"
#include "../src/apply_mailbox.h"
#include "../src/battle_net.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using u64 = uint64_t;
// Same layout as the Steamworks CCallbackBase.
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
// Three of the steam_api imports the game has, so the layer's IAT hooks see these calls.
extern "C" __declspec(dllimport) void SteamAPI_RunCallbacks();
extern "C" __declspec(dllimport) void SteamAPI_RegisterCallResult(CallbackBase*, u64);
extern "C" __declspec(dllimport) void SteamAPI_UnregisterCallResult(CallbackBase*, u64);
// Controls of the fake steam_api.dll.
extern "C" __declspec(dllimport) void FakeSteam_SetMainThread(DWORD);
extern "C" __declspec(dllimport) LONG FakeSteam_WorkerRuns();
extern "C" __declspec(dllimport) void FakeSteam_CompleteGameCall(int);
extern "C" __declspec(dllimport) int FakeSteam_SentCount();
extern "C" __declspec(dllimport) bool FakeSteam_Sent(int, char*, int);
extern "C" __declspec(dllimport) bool FakeSteam_Joinable();
extern "C" __declspec(dllimport) int FakeSteam_MemberLimit();
extern "C" __declspec(dllimport) unsigned FakeSteam_FriendPackets();
extern "C" __declspec(dllimport) int FakeSteam_InviteDialogs();
extern "C" __declspec(dllimport) unsigned FakeSteam_TunnelPackets(int, int);
extern "C" __declspec(dllimport) bool FakeSteam_PresenceConnect(char*, int);
extern "C" __declspec(dllimport) void FakeSteam_Inject(int, const void*, int);
extern "C" __declspec(dllimport) void FakeSteam_LobbyLeft(bool);
extern "C" __declspec(dllimport) void FakeSteam_Deaf(bool);
extern "C" __declspec(dllimport) void FakeSteam_JoinRequest();
extern "C" __declspec(dllimport) void FakeSteam_SetOverlay(bool);

#include "check.h"

// Stands in for the game's EncryptedAppTicket call result.
class GameTicket final : public CallbackBase {
public:
    GameTicket() { id = 154; }
    void Run(void*) override {}
    void Run(void*, bool, u64) override { thread = GetCurrentThreadId(); }
    int GetCallbackSizeBytes() override { return 1032; }
    DWORD thread = 0;
};

// The layer's files sit in a steam folder beside the exe, each name carrying the process id.
static std::wstring steam_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    *wcsrchr(p, L'\\') = L'\0';
    return std::wstring(p) + L"\\steam\\";
}
static std::wstring file(const wchar_t* stem, const wchar_t* ext) {
    return steam_dir() + stem + L"-" + std::to_wstring(GetCurrentProcessId()) + ext;
}
// Writes a request or offer file whole: a temporary file, then a rename.
static void put(const std::wstring& path, const char* text) {
    const std::wstring temp = path + L".w";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") || !f) return;
    fputs(text, f);
    fclose(f);
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
// This process's status file, as the layer last wrote it.
static std::string status() {
    FILE* f = nullptr;
    std::string s;
    if (_wfopen_s(&f, file(L"status", L".json").c_str(), L"rb") || !f) return s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
// Waits until the status file contains `needle`; on a timeout, prints the status.
static bool wait_for(const char* needle, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (status().find(needle) != std::string::npos) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for %s\n%s", needle, status().c_str());
    return false;
}
// Whether this process sent `text` as lobby chat.
static bool sent(const char* text) {
    char b[1200];
    for (int i = 0; i < FakeSteam_SentCount(); ++i)
        if (FakeSteam_Sent(i, b, sizeof b) && !strcmp(b, text)) return true;
    return false;
}

// Relay scenario: the parent process is the host PC, a child process the target PC.
static unsigned char pattern(size_t i, unsigned seed) {
    return static_cast<unsigned char>((i * 2654435761u + seed) >> 13);
}
static bool send_all(SOCKET s, const char* p, int n) {
    while (n > 0) {
        const int k = ::send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}
static bool recv_all(SOCKET s, char* p, int n) {
    while (n > 0) {
        const int k = recv(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}
// A number from the status file; 0 when missing.
static unsigned status_number(const char* key) {
    const std::string s = status(), k = std::string("\"") + key + "\": ";
    const size_t at = s.find(k);
    return at == std::string::npos ? 0 : static_cast<unsigned>(strtoul(s.c_str() + at + k.size(), nullptr, 10));
}
constexpr int greeting_bytes = 64 * 1024, echo_bytes = 256 * 1024;

// The target PC: a loopback service behind the relay that greets, echoes, then expects EOF.
static void relay_target_side() {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    CHECK(!bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) && !listen(l, 1) &&
          !getsockname(l, reinterpret_cast<sockaddr*>(&a), &len));
    const std::string request = "target " + std::to_string(ntohs(a.sin_port));
    put(file(L"relay", L".txt"), request.c_str());
    fd_set set;
    FD_ZERO(&set);
    FD_SET(l, &set);
    timeval wait{30, 0};
    if (select(0, &set, nullptr, nullptr, &wait) != 1) {
        CHECK(!"relay never connected to the target bridge");
        return;
    }
    SOCKET c = accept(l, nullptr, nullptr);
    closesocket(l);
    std::string greeting(greeting_bytes, '\0');
    for (int i = 0; i < greeting_bytes; ++i) greeting[i] = static_cast<char>(pattern(i, 11));
    CHECK(send_all(c, greeting.data(), greeting_bytes));
    std::string echo(echo_bytes, '\0');
    CHECK(recv_all(c, &echo[0], echo_bytes));
    CHECK(send_all(c, echo.data(), echo_bytes));
    char extra;
    CHECK(recv(c, &extra, 1, 0) == 0);  // the host closes after the echo: EOF crosses the relay
    closesocket(c);
    CHECK(wait_for("\"relay\": \"ready\"", 5000));
    CHECK(status_number("relay_streams") == 1);
}

// The host PC: connects to its relay port and checks the greeting and the echo come through intact.
static void relay_host_side() {
    if (!wait_for("\"build_match\": true", 5000)) {
        ++failures;
        return;
    }
    put(file(L"relay", L".txt"), "host");
    if (!wait_for("\"relay\": \"listening\"", 3000)) {
        ++failures;
        return;
    }
    const unsigned port = status_number("relay_port");
    CHECK(port >= 1024);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    CHECK(!connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a));
    DWORD timeout = 30000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    std::string greeting(greeting_bytes, '\0');
    CHECK(recv_all(c, &greeting[0], greeting_bytes));
    bool same = true;
    for (int i = 0; i < greeting_bytes; ++i) same &= static_cast<unsigned char>(greeting[i]) == pattern(i, 11);
    CHECK(same);
    std::string data(echo_bytes, '\0'), back(echo_bytes, '\0');
    for (int i = 0; i < echo_bytes; ++i) data[i] = static_cast<char>(pattern(i, 97));
    CHECK(send_all(c, data.data(), echo_bytes));
    CHECK(recv_all(c, &back[0], echo_bytes));
    CHECK(back == data);
    closesocket(c);
    CHECK(wait_for("\"relay_streams\": 1,", 5000));
    CHECK(wait_for("\"relay\": \"listening\"", 5000));
    CHECK(status_number("relay_up") == echo_bytes && status_number("relay_down") == greeting_bytes + echo_bytes);
}

// Session scenario: the parent is the host PC, a child process the guest PC. Both replay the
// Player capture log named by GCT_SESSION_FIXTURE.
namespace cap = gc::capture;
namespace ss = gc::session;

// The fixture log's CHANNEL tags, as the capture hook receives them.
static std::vector<std::string> fixture_tags() {
    char path[MAX_PATH];
    std::vector<std::string> tags;
    FILE* f = nullptr;
    if (!GetEnvironmentVariableA("GCT_SESSION_FIXTURE", path, sizeof path) || fopen_s(&f, path, "rb") || !f)
        return tags;
    std::string text;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    for (std::string_view line : cap::split_lines(text, false)) {
        const size_t at = line.find(" CHANNEL ");
        if (at != std::string_view::npos) tags.emplace_back(line.substr(at + 9));
    }
    return tags;
}

// What the host's in-process capture validates from those tags.
static std::vector<cap::Validated> fixture_snapshots() {
    cap::Stream stream;
    std::vector<cap::Validated> out;
    for (const std::string& tag : fixture_tags()) stream.tag(tag, out);
    return out;
}

// Host: pairs, holds its galaxy at Start, sends all seven revisions, and releases the hold once the
// friend's game reports one applied.
static void session_host_side() {
    if (!wait_for("\"build_match\": true", 5000) || !wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    // The Online menu's waiting popup hears that the friend has paired.
    Sleep(300);
    CHECK(gc::menu_answer("paired") && !gc::menu_answer("failed"));
    // The party screen's rows show the friend ready.
    unsigned seq = 0;
    std::string rows;
    CHECK(gc::party_rows(seq, rows) && rows == "2\n1:ready:Peer \"Two\"\n");
    // The ready check: Start holds the host's galaxy (player_hold.lua asks hold_wanted) until the
    // friend's game reports L.
    CHECK(!gc::hold_wanted());
    gc::menu_event("start");
    Sleep(100);
    CHECK(gc::hold_wanted() && gc::menu_answer("hold_loading_1") && !gc::menu_answer("hold_loading_2"));
    gc::hold_event("shown");
    const std::vector<std::string> tags = fixture_tags();
    const std::vector<cap::Validated> expected = fixture_snapshots();
    CHECK(tags.size() > 1000 && expected.size() == 7);
    // Plays the game thread: every record goes through the capture hook.
    for (size_t i = 0; i < tags.size(); ++i) {
        gc::capture_channel(tags[i].c_str());
        if (i % 100 == 0) Sleep(5);
    }
    CHECK(wait_for("\"session_acked\": 7,", 20000));
    CHECK(status_number("session_latest") == 7 && status_number("session_sent") == 7);
    // Steam ends the host's own membership; the session with the paired guest stays.
    FakeSteam_LobbyLeft(true);
    Sleep(400);
    CHECK(status().find("\"session\": \"ready\"") != std::string::npos);
    CHECK(status_number("session_id") != 0);
    CHECK(!expected.empty() &&
          status().find("\"session_digest\": \"" + expected.back().normalized_sha256) != std::string::npos);
    // Still held while the friend's game has applied nothing; released once it has.
    CHECK(gc::hold_wanted());
    bool released = false;
    for (int i = 0; i < 200 && !released; ++i) {
        released = !gc::hold_wanted();
        if (!released) Sleep(100);
    }
    CHECK(released);
}

// Sends a snapshot frame as if from the lobby peer; tamper 1 breaks its digest, 2 its canonical JSON.
static void inject(uint32_t session, uint32_t revision, const cap::Validated& v, int tamper) {
    ss::Frame f;
    f.type = ss::Type::snapshot;
    f.session = session;
    f.revision = revision;
    f.meta.producer = 6;
    f.meta.generation = v.generation;
    f.meta.sequence = v.sequence;
    f.meta.source_table = v.source_table;
    f.meta.update_ticket = v.update_ticket;
    f.meta.unchanged_samples = v.unchanged_samples;
    f.json = cap::canonical_json(v.data);
    if (tamper == 2) f.json.insert(1, " ");  // valid JSON, but not canonical
    const std::string digest = cap::sha256_hex(f.json);
    for (size_t i = 0; i < 32; ++i)
        f.digest[i] = static_cast<uint8_t>(std::stoul(digest.substr(2 * i, 2), nullptr, 16));
    if (tamper == 1) f.digest[0] ^= 1;
    const std::string bytes = ss::encode(f);
    FakeSteam_Inject(ss::channel, bytes.data(), static_cast<int>(bytes.size()));
}

// Whether the guest holds `revision` with this digest, after a short wait.
static bool holds(unsigned revision, const std::string& digest) {
    Sleep(400);
    return status_number("session_received") == revision &&
           status().find("\"session_digest\": \"" + digest + "\"") != std::string::npos;
}

// Guest: receives all seven revisions, refuses forged frames, and reports revision 7 applied.
static void session_guest_side() {
    const std::vector<cap::Validated> expected = fixture_snapshots();
    if (expected.size() != 7) {
        CHECK(!"fixture did not validate");
        return;
    }
    if (!wait_for("\"session_received\": 7,", 20000)) {
        ++failures;
        return;
    }
    const std::string last = expected.back().normalized_sha256;
    CHECK(holds(7, last));
    // Every received revision became a save candidate for the apply loop, the newest one held.
    CHECK(status_number("apply_offered") == 7);
    // The host's lobby membership ends; the paired session runs on P2P and carries on.
    FakeSteam_LobbyLeft(false);
    Sleep(400);
    CHECK(status().find("\"session\": \"ready\"") != std::string::npos);
    CHECK(status().find("\"state\": \"joined\"") != std::string::npos);
    const uint32_t id = status_number("session_id");
    CHECK(id != 0);
    // Forged frames, as if from the lobby peer: none may displace revision 7.
    inject(id, 3, expected[2], 0);  // stale
    CHECK(holds(7, last));
    inject(id, 8, expected[2], 1);  // the digest does not match
    CHECK(holds(7, last));
    inject(id, 8, expected[2], 2);  // not canonical
    CHECK(holds(7, last));
    inject(id ^ 0x5A5A, 9, expected[2], 0);  // another session
    CHECK(holds(7, last));
    inject(id, 8, expected[2], 0);  // a genuine newer revision is accepted
    CHECK(holds(8, expected[2].normalized_sha256));
    inject(id, 8, expected[3], 0);  // a different snapshot under the same revision
    CHECK(holds(8, expected[2].normalized_sha256));
    // The apply loop applied host revision 7: the session reports it with L, releasing the host's hold.
    gc::apply_progress().applied_offer.store(7, std::memory_order_release);
    gc::apply_progress().applied.store(7, std::memory_order_release);
    Sleep(1500);
}

// Versus scenario: the parent is the host (side 1), a child process the friend (side 2). Both replay
// a Versus capture; the friend's copy has its controller on side 2.
static std::vector<std::string> versus_tags(bool friend_side) {
    std::vector<std::string> tags = fixture_tags();
    if (friend_side) {
        static const std::string mine = "controllerTeam:i:0:i:1", theirs = "controllerTeam:i:0:i:2";
        for (std::string& t : tags) {
            const size_t at = t.find(mine);
            if (at != std::string::npos) t.replace(at, mine.size(), theirs);
        }
    }
    return tags;
}
// Plays this PC's game thread: feeds the capture until the newest validated revision is `upto`.
struct Feed {
    std::vector<std::string> tags;
    size_t at = 0;
    bool until(unsigned upto) {
        gc::CaptureOffer newest;
        while (!gc::capture_latest(upto - 1, newest) && at < tags.size()) gc::capture_channel(tags[at++].c_str());
        return gc::capture_latest(upto - 1, newest) && newest.revision == upto;
    }
};
// Waits for a Versus answer to turn true.
static bool versus_wait(const char* question, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (gc::versus_answer(question)) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for %s\n", question);
    return false;
}
// Waits for a status number to reach `at_least`.
static bool wait_number(const char* key, unsigned at_least, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (status_number(key) >= at_least) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for %s >= %u\n%s", key, at_least, status().c_str());
    return false;
}

// Versus host: picks side 1, starts the galaxy, and sends its turns as played and the turn it hands over.
static void versus_host_side() {
    // Invited from Online > Versus > New.
    gc::menu_event("invite:vs_new");
    if (!wait_for("\"build_match\": true", 5000) || !wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    Sleep(300);
    CHECK(status().find("\"mode\": \"versus\"") != std::string::npos);
    // A Versus lobby holds two, so it is full once the friend is in.
    CHECK(FakeSteam_MemberLimit() == 2 && !FakeSteam_Joinable());
    CHECK(gc::menu_answer("paired") && gc::versus_answer("versus") && gc::versus_answer("versus_host"));
    // The side screen: side 1 in the Clone Wars, then Ready.
    gc::versus_event("sides");
    gc::versus_event("era:1");
    gc::versus_event("pick:1");
    gc::versus_event("ready:1");
    CHECK(gc::versus_answer("versus_mine_1") && gc::versus_answer("versus_mine_ready") &&
          !gc::versus_answer("versus_go"));
    gc::versus_event("started");
    CHECK(!gc::versus_answer("versus_started"));  // not before both are ready
    CHECK(versus_wait("versus_go", 8000));        // the friend is ready on side 2
    CHECK(gc::versus_answer("versus_peer_2") && gc::versus_answer("versus_peer_ready"));
    gc::versus_event("started");
    CHECK(gc::versus_answer("versus_started"));
    // The galaxy: the friend's game loads its first state while it is held.
    CHECK(gc::hold_wanted());
    Feed feed{versus_tags(false)};
    CHECK(feed.until(1));
    CHECK(wait_number("session_acked", 1, 8000));
    CHECK(status_number("session_sent") == 1);
    // Side 1 plays the rest of turn 1 here; it goes as it is played, so the friend watches it.
    CHECK(feed.until(6));
    CHECK(wait_number("session_acked", 6, 8000));
    // NextTurn: turn 2 is side 2's, so it goes.
    CHECK(feed.until(8));
    CHECK(wait_number("session_acked", 7, 8000));
    // The friend plays turn 2 and hands turn 3 back, for this PC's own galaxy to take in place.
    CHECK(wait_number("session_received", 9, 15000));
    CHECK(status_number("apply_offered") >= 9);
    // Side 1's turn 3 is played here, and goes as it is played.
    CHECK(feed.until(14));
    CHECK(wait_number("session_acked", 14, 8000));
    // This PC's NextTurn ends the game: the friend is told.
    gc::versus_event("over:1:5");
    Sleep(1500);
}

// Versus friend: takes side 2 once side 1 is taken, loads the host's first state and plays turn 2.
static void versus_guest_side() {
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(status().find("\"mode\": \"versus\"") != std::string::npos);
    CHECK(gc::versus_answer("versus") && !gc::versus_answer("versus_host"));
    // The host is on the side screen: its era and its pick arrive.
    CHECK(versus_wait("versus_heard", 8000) && versus_wait("versus_era_1", 8000));
    gc::versus_event("sides");
    CHECK(versus_wait("versus_peer_ready", 8000) && gc::versus_answer("versus_peer_1"));
    gc::versus_event("pick:1");
    gc::versus_event("ready:1");
    CHECK(gc::versus_answer("versus_mine_1") && !gc::versus_answer("versus_mine_ready"));  // side 1 is taken
    gc::versus_event("pick:2");
    gc::versus_event("ready:1");
    CHECK(gc::versus_answer("versus_mine_2") && gc::versus_answer("versus_mine_ready"));
    CHECK(versus_wait("versus_started", 8000));
    // The host's first state arrives (its turn 1) and this game loads it; the rest of that turn follows
    // as the host plays it.
    CHECK(wait_number("session_received", 1, 8000));
    gc::apply_progress().applied_offer.store(1, std::memory_order_release);
    gc::apply_progress().applied.store(1, std::memory_order_release);
    // This PC's own galaxy, side 2, showing the host's turn: nothing goes back.
    Feed feed{versus_tags(true)};
    CHECK(feed.until(6));
    // The host hands over turn 2 (revision 7 or 8, whichever is newest when it goes).
    CHECK(wait_number("session_received", 7, 10000));
    CHECK(status_number("session_received") <= 8 && status_number("session_sent") == 0);
    // Turn 2 is this PC's: it goes as it is played. Its NextTurn hands turn 3 back.
    CHECK(feed.until(8));
    CHECK(wait_number("session_acked", 8, 8000));
    CHECK(feed.until(14));
    CHECK(wait_number("session_acked", 9, 8000));
    // The host's game has ended, side 1 the winner.
    CHECK(versus_wait("versus_over_1", 10000) && !gc::versus_answer("versus_over_2"));
}

// Loaded Versus game (versusload): no side screen. The host's save resumes in side 2's turn 2, which
// the friend's game loads and plays, watched here, before handing turn 3 back.
static void versusload_host_side() {
    Feed feed{versus_tags(false)};
    CHECK(feed.until(8));  // revisions 7 and 8 are turn 2
    gc::menu_event("invite:vs_load");
    if (!wait_for("\"build_match\": true", 5000) || !wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    Sleep(300);
    CHECK(status().find("\"mode\": \"versus\"") != std::string::npos);
    CHECK(gc::menu_answer("paired") && gc::versus_answer("versus") && gc::versus_answer("versus_host"));
    CHECK(wait_number("session_acked", 8, 8000));
    CHECK(status_number("session_sent") == 8);
    // The friend plays turn 2: it arrives as it is played, then turn 3 handed back.
    CHECK(wait_number("session_received", 8, 15000));
    CHECK(wait_number("session_received", 9, 15000));
    CHECK(status_number("session_sent") == 8);  // nothing more of turn 2 goes
    // Side 1's turn 3 is played here and goes as it is played.
    CHECK(feed.until(14));
    CHECK(wait_number("session_acked", 14, 8000));
    Sleep(500);
}
// Loaded Versus friend: loads the save's turn 2, plays it and hands turn 3 back.
static void versusload_guest_side() {
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(status().find("\"mode\": \"versus\"") != std::string::npos);
    CHECK(gc::versus_answer("versus") && !gc::versus_answer("versus_host"));
    CHECK(!gc::versus_answer("versus_heard"));  // no side screen for a load
    // The host's first state, the save's turn 2, is this PC's side's: it loads.
    CHECK(wait_number("session_received", 8, 8000));
    CHECK(status_number("session_received") == 8 && status_number("session_sent") == 0);
    gc::apply_progress().applied_offer.store(1, std::memory_order_release);
    gc::apply_progress().applied.store(8, std::memory_order_release);
    // This PC's galaxy, side 2: the host's turn 1 never goes back; its own turn 2 goes as it is played.
    Feed feed{versus_tags(true)};
    CHECK(feed.until(6));
    Sleep(300);
    CHECK(status_number("session_sent") == 0);
    CHECK(feed.until(8));
    CHECK(wait_number("session_acked", 8, 8000));
    // NextTurn hands turn 3 back.
    CHECK(feed.until(14));
    CHECK(wait_number("session_acked", 9, 8000));
    // The host's turn 3 comes as it is played.
    CHECK(wait_number("session_received", 9, 15000));
}

// hostgone, the friend's side: the host leaves the lobby and goes silent; the session ends after
// departed_limit, and the game thread is told once.
static void hostgone_guest_side() {
    if (!wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    Sleep(300);
    CHECK(gc::menu_answer("guest_wait"));  // paired, nothing to load yet
    // The host quits: it leaves the lobby and its keepalive stops.
    const ULONGLONG gone = GetTickCount64();
    FakeSteam_LobbyLeft(false);
    FakeSteam_Deaf(true);
    Sleep(2000);
    CHECK(status().find("\"session_end\": \"\"") != std::string::npos);  // the lobby alone is not enough
    CHECK(wait_for("\"session_end\": \"the host left the lobby and went silent\"", 15000));
    const ULONGLONG took = GetTickCount64() - gone;
    CHECK(took >= 9000 && took < 15000);  // departed_limit, 10 s
    CHECK(wait_for("\"state\": \"left\"", 2000));
    CHECK(gc::apply_progress().host_left.load() == 1);  // the game thread is told once
    CHECK(!gc::menu_answer("guest_wait"));              // nothing is shown
}

// hostends and friendends: a co-op game is one galaxy. The host returning to the menus, or the
// friend's game leaving the host's galaxy, ends the session.
static void ends_host_side(bool host_leaves) {
    gc::menu_event("invite:3");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    gc::note_place(2);  // the host's galaxy
    Sleep(300);
    CHECK(status().find("\"state\": \"hosting\"") != std::string::npos);
    if (host_leaves) {
        gc::note_place(1);  // saved and quit to the menus
        CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 2000));
        CHECK(gc::apply_progress().friend_left.load() == 0);
    } else {
        CHECK(wait_for("\"session_end\": \"the friend left\"", 8000));
        CHECK(gc::apply_progress().friend_left.load() == 1);  // the host's game offers to save
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
    CHECK(status().find("\"peer\": \"\"") != std::string::npos);
    CHECK(!gc::menu_answer("paired"));  // the next game is a new invite
}

// hostcloses and friendcloses: Exit to Windows says goodbye at once (menu:closing), so the other PC
// leaves well before the silence would tell it. menu_event returns once the other PC is told.
static void closes_host_side(bool host_closes) {
    gc::menu_event("invite:3");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    gc::note_place(2);  // the host's galaxy
    Sleep(300);
    const ULONGLONG start = GetTickCount64();
    if (host_closes) {
        gc::menu_event("closing");
        CHECK(GetTickCount64() - start < 600);
        CHECK(wait_for("\"session_end\": \"the host closed its game\"", 1000));  // the status file, next tick
        CHECK(gc::apply_progress().friend_left.load() == 0);
    } else {
        CHECK(wait_for("\"session_end\": \"the friend left\"", 5000));
        CHECK(GetTickCount64() - start < 5000);               // its goodbye, not the silence
        CHECK(gc::apply_progress().friend_left.load() == 1);  // the host's game offers to save
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
    CHECK(!gc::menu_answer("paired"));
}
// The friend's side of hostcloses and friendcloses.
static void closes_guest_side(bool host_closes) {
    if (!wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    gc::apply_progress().in_galaxy.store(true);  // the host's galaxy shows here
    gc::note_place(2);
    Sleep(300);
    const ULONGLONG start = GetTickCount64();
    if (host_closes) {
        CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 5000));
        CHECK(GetTickCount64() - start < 5000);               // its goodbye, not the silence
        CHECK(gc::apply_progress().host_left.load() == 1);    // the game thread leaves the galaxy
        CHECK(gc::apply_progress().friend_left.load() == 0);  // co-op: no save prompt on the friend's PC
    } else {
        gc::menu_event("closing");
        CHECK(GetTickCount64() - start < 600);
        CHECK(wait_for("\"session_end\": \"the friend closed its game\"", 1000));  // the status file, next tick
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
    Sleep(500);
}

// friendgone: the friend's game quits without a goodbye; the host ends the session on the silence.
static void friendgone_host_side() {
    gc::menu_event("invite:4");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    gc::note_place(2);
    Sleep(300);
    FakeSteam_LobbyLeft(false);  // the friend's game quits
    FakeSteam_Deaf(true);
    CHECK(wait_for("\"session_end\": \"the friend left the lobby and went silent\"", 15000));
    CHECK(wait_for("\"state\": \"left\"", 1000));
    CHECK(gc::apply_progress().friend_left.load() == 1);
}
// The friend's side of hostends and friendends.
static void ends_guest_side(bool host_leaves) {
    if (!wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    gc::apply_progress().in_galaxy.store(true);  // the host's galaxy shows here
    gc::note_place(2);
    Sleep(300);
    if (host_leaves) {
        CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 5000));
        CHECK(gc::apply_progress().host_left.load() == 1);    // the game thread leaves the galaxy
        CHECK(gc::apply_progress().friend_left.load() == 0);  // co-op: no save prompt on the friend's PC
    } else {
        Sleep(1000);                                  // the host sees it paired first
        gc::apply_progress().in_galaxy.store(false);  // the friend quits to the menus
        gc::note_place(1);
        CHECK(wait_for("\"session_end\": \"the friend left the host's galaxy\"", 2000));
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
    CHECK(!gc::menu_answer("guest_wait") && !gc::menu_answer("failed"));  // no popup on the menus
    Sleep(500);
}

// battle: the host announces battle 1 (F), both games seal their LAN sockets, and game packets
// cross the tunnel (channel 9) both ways. The friend reports J; the host sends V, then E.
static const char* const battle_text =
    "GCT-0a1b2c3d\npol1g_con\nimp\npol\nall=1111100,imp=1111111\n1=autoturrets,2=leader";
static void battle_host_side() {
    gc::menu_event("invite:3");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    gc::note_place(2);
    gc::battle_host_announce(1, battle_text);
    gc::battle_seal(true);
    // The game sends every frame; the friend's answers once its own sockets are sealed.
    bool answered = false;
    for (int i = 0; i < 100 && !answered; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::listener, gc::battle::From::broadcast, "session info", 12);
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "host state", 10);
        Sleep(100);
        answered = gc::battle_tunnel_waiting() > 0;
    }
    CHECK(answered);
    CHECK(gc::battle_seal_counts().dropped == 0);  // the friend is the peer for every packet
    // The friend's game reports leaving battle 1 before its end (J).
    unsigned seen = 0, seq = 0, number = 0;
    gc::battle::Part part = gc::battle::Part::none;
    bool left = false;
    for (int i = 0; i < 100 && !left; ++i) {
        if (gc::battle_host_report(1, seen, seq, number, part)) seen = seq;
        left = number == 1 && part == gc::battle::Part::left;
        if (!left) Sleep(50);
    }
    CHECK(left);
    CHECK(!gc::battle_host_report(1, seen, seq, number, part));  // nothing newer
    // The result (V) goes only with a winning team, its mask whole; then the battle's end (E).
    gc::battle_host_result(1, 0);
    gc::battle_host_result(1, 0xFFFFFFFBu);
    gc::battle_host_over(1);
    Sleep(1500);
    gc::battle_seal(false);
}
// The friend's side: refuses an early tunnel frame, joins, leaves before the end, then gets V and E.
static void battle_guest_side() {
    if (!wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    const unsigned session = gc::battle_guest_session();  // the first this game opened
    CHECK(session == 1);
    // Not yet sealed: a frame on channel 9, even from the peer, is refused.
    const std::string early = gc::battle::encode_tunnel(gc::battle::To::game, gc::battle::From::game, 0, "early", 5);
    FakeSteam_Inject(gc::battle::tunnel_channel, early.data(), static_cast<int>(early.size()));
    unsigned number = 0;
    std::string text;
    bool offered = false;
    for (int i = 0; i < 80 && !offered; ++i) {
        offered = gc::battle_guest_offered(session, 0, number, text);
        if (!offered) Sleep(100);
    }
    CHECK(offered && number == 1 && text == battle_text);
    CHECK(!gc::battle_guest_offered(session, 1, number, text));  // nothing newer
    unsigned won = 0, mask = 0;
    CHECK(!gc::battle_guest_result(session, won, mask));  // no result before the battle's end
    CHECK(gc::battle_tunnel_waiting() == 0);              // the early frame is refused
    gc::battle_seal(true);
    bool received = false;
    for (int i = 0; i < 100 && !received; ++i) {
        received = gc::battle_tunnel_waiting() >= 2;
        if (!received) Sleep(50);
    }
    CHECK(received);
    for (int i = 0; i < 10; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "join", 4);
        Sleep(50);
    }
    // In the battle, then out of it before the host's end (the friend's Quit).
    gc::battle_guest_report(1, gc::battle::Part::joined);
    Sleep(300);
    gc::battle_guest_report(1, gc::battle::Part::left);
    gc::battle_guest_report(1, gc::battle::Part::none);  // nothing to tell: ignored
    bool over = false;
    for (int i = 0; i < 100 && !over; ++i) {
        over = gc::battle_guest_over(session) == 1;
        if (!over) Sleep(100);
    }
    CHECK(over);
    // V arrives before E, with the host's winning-team mask, for this session only.
    CHECK(gc::battle_guest_result(session, won, mask) && won == 1 && mask == 0xFFFFFFFBu);
    CHECK(!gc::battle_guest_result(session + 1, won, mask));
    gc::battle_seal(false);
}

// versusbattle: the friend's PC attacks. It asks for the defender's card (C), gets the pick (D),
// hosts the battle (F) with the roles reversed, hears this PC's quit (J) and sends V and E.
static const char* const versus_battle_text =
    "GCT-1a2b3c4d\nyav1g_con\nimp\nyav\nall=1111111,imp=1111111\n1=bacta,2=autoturrets";
static void versusbattle_host_side() {
    gc::menu_event("invite:vs_new");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(versus_wait("versus_paired", 5000));
    const unsigned session = gc::battle_guest_session();  // the friend's battles count afresh here
    CHECK(session >= 1);
    // The friend's card screen asks for this side's card: shown once, answered.
    unsigned number = 0;
    std::string data;
    bool request = false;
    for (int i = 0; i < 100 && !request; ++i) {
        request = gc::versus_card_pending(number, data);
        if (!request) Sleep(50);
    }
    CHECK(request && number == 1 && data == "yav1g_con\n4,0,7" && gc::versus_answer("versus_card_open"));
    gc::versus_card_taken(number);
    CHECK(!gc::versus_card_pending(number, data) && gc::versus_answer("versus_card_open"));  // shown, not answered
    gc::versus_event("card:1:1");
    CHECK(!gc::versus_answer("versus_card_open"));
    // The friend's battle: this PC joins it, the peer being the battle host.
    std::string text;
    bool offered = false;
    for (int i = 0; i < 100 && !offered; ++i) {
        offered = gc::battle_guest_offered(session, 0, number, text);
        if (!offered) Sleep(50);
    }
    CHECK(offered && number == 1 && text == versus_battle_text);
    gc::battle_tunnel_role(2);
    gc::battle_seal(true);
    CHECK(gc::battle_tunnel_peer_role() == 2);
    bool received = false;
    for (int i = 0; i < 100 && !received; ++i) {
        received = gc::battle_tunnel_waiting() >= 2;
        if (!received) Sleep(50);
    }
    CHECK(received);
    for (int i = 0; i < 10; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "join", 4);
        Sleep(50);
    }
    // In the battle, then this PC's player quits it (the pause menu's Quit).
    gc::battle_guest_report(1, gc::battle::Part::joined);
    Sleep(300);
    gc::battle_guest_report(1, gc::battle::Part::quit);
    bool over = false;
    for (int i = 0; i < 100 && !over; ++i) {
        over = gc::battle_guest_over(session) == 1;
        if (!over) Sleep(100);
    }
    CHECK(over);
    unsigned battle = 0, mask = 0;
    CHECK(gc::battle_guest_result(session, battle, mask) && battle == 1 && mask == 0x00000004u);
    gc::battle_seal(false);
    gc::battle_tunnel_role(0);
    // The ground battle's request: open until that battle is announced, then closed unanswered.
    request = false;
    for (int i = 0; i < 100 && !request; ++i) {
        request = gc::versus_card_pending(number, data);
        if (!request) Sleep(50);
    }
    CHECK(request && number == 2 && data == "yav1g_con\n0,0,7");
    bool closed = false;
    for (int i = 0; i < 100 && !closed; ++i) {
        closed = !gc::versus_answer("versus_card_open");
        if (!closed) Sleep(50);
    }
    CHECK(closed && gc::battle_guest_offered(session, 1, number, text) && number == 2);
}
// The friend's side: asks for the card, hosts the battle, hears the quit, and sends V and E.
static void versusbattle_guest_side() {
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(versus_wait("versus_paired", 5000) && gc::versus_answer("versus") && !gc::versus_answer("versus_host"));
    // This PC attacks: its card screen reaches the defender's PC.
    gc::versus_event("card_ask:yav1g_con:4,0,7");
    CHECK(!gc::versus_answer("versus_card_answered"));
    CHECK(versus_wait("versus_card_answered", 8000));
    CHECK(gc::versus_answer("versus_card_slot_1") && !gc::versus_answer("versus_card_slot_0"));
    // It hosts the battle: the peer is its client.
    gc::battle_host_announce(1, versus_battle_text);
    gc::battle_tunnel_role(1);
    gc::battle_seal(true);
    bool answered = false;
    for (int i = 0; i < 100 && !answered; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::listener, gc::battle::From::broadcast, "session info", 12);
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "host state", 10);
        Sleep(100);
        answered = gc::battle_tunnel_waiting() > 0;
    }
    CHECK(answered && gc::battle_seal_counts().dropped == 0 && gc::battle_tunnel_peer_role() == 1);
    // The host's player quits the battle (J quit).
    unsigned seen = 0, seq = 0, number = 0;
    gc::battle::Part part = gc::battle::Part::none;
    bool quit = false;
    for (int i = 0; i < 100 && !quit; ++i) {
        if (gc::battle_host_report(1, seen, seq, number, part)) seen = seq;
        quit = number == 1 && part == gc::battle::Part::quit;
        if (!quit) Sleep(50);
    }
    CHECK(quit);
    // This side won: MissionVictory(2) sets 1 << 2.
    gc::battle_host_result(1, 0x00000004u);
    gc::battle_host_over(1);
    Sleep(800);
    gc::battle_seal(false);
    gc::battle_tunnel_role(0);
    // The ground battle: its request, then the battle announced without an answer.
    gc::versus_event("card_ask:yav1g_con:0,0,7");
    Sleep(1500);
    gc::battle_host_announce(2, versus_battle_text);
    Sleep(2000);
}

// versushostcloses and versusfriendcloses: Exit to Windows inside a Versus battle the host hosts.
// The closing side loses its battle (V or J quit, then B), and the PC left offers to save.
static void versuscloses_host_side(bool host_closes) {
    gc::menu_event("invite:vs_new");
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(versus_wait("versus_paired", 5000));
    gc::note_place(2);  // the galaxy; its battles count as in it
    gc::battle_host_announce(1, versus_battle_text);
    if (host_closes) {
        Sleep(1000);  // F goes, and the friend's game joins
        gc::battle_closing(1, true, gc::battle::concede_mask(1));
        const ULONGLONG start = GetTickCount64();
        gc::menu_event("closing");
        CHECK(GetTickCount64() - start < 600);
        CHECK(wait_for("\"session_end\": \"the host closed its game\"", 1000));
        CHECK(gc::apply_progress().friend_left.load() == 0);
    } else {
        unsigned seen = 0, seq = 0, number = 0;
        gc::battle::Part part = gc::battle::Part::none;
        bool quit = false;
        for (int i = 0; i < 100 && !quit; ++i) {
            if (gc::battle_host_report(1, seen, seq, number, part)) seen = seq;
            quit = number == 1 && part == gc::battle::Part::quit;
            if (!quit) Sleep(50);
        }
        CHECK(quit);  // it arrives before B, which ends the peer here
        CHECK(wait_for("\"session_end\": \"the friend left\"", 5000));
        CHECK(gc::apply_progress().friend_left.load() == 1);  // the host's game offers to save
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
}
// The friend's side of versushostcloses and versusfriendcloses.
static void versuscloses_guest_side(bool host_closes) {
    if (!wait_for("\"session\": \"ready\"", 8000)) {
        ++failures;
        return;
    }
    CHECK(versus_wait("versus_paired", 5000));
    gc::note_place(2);
    const unsigned session = gc::battle_guest_session();
    unsigned number = 0;
    std::string text;
    bool offered = false;
    for (int i = 0; i < 100 && !offered; ++i) {
        offered = gc::battle_guest_offered(session, 0, number, text);
        if (!offered) Sleep(50);
    }
    CHECK(offered && number == 1);
    gc::battle_guest_report(1, gc::battle::Part::joined);
    if (host_closes) {
        CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 5000));
        CHECK(gc::apply_progress().host_left.load() == 1);
        CHECK(gc::apply_progress().friend_left.load() == 1);  // Versus: the one left offers to save
        unsigned battle = 0, mask = 0;
        CHECK(gc::battle_guest_result(session, battle, mask) && battle == 1 && mask == 0x00000004u);
    } else {
        Sleep(500);
        gc::battle_closing(1, false, 0);
        const ULONGLONG start = GetTickCount64();
        gc::menu_event("closing");
        CHECK(GetTickCount64() - start < 600);
        CHECK(wait_for("\"session_end\": \"the friend closed its game\"", 1000));
        CHECK(gc::apply_progress().friend_left.load() == 0);
    }
    CHECK(wait_for("\"state\": \"left\"", 1000));
    Sleep(500);
}

// Sends a session frame as if the lobby peer sent it.
static void forge(ss::Type type, uint32_t session, uint32_t revision, const std::string& text = std::string()) {
    ss::Frame f;
    f.type = type;
    f.session = session;
    f.revision = revision;
    f.json = text;
    const std::string bytes = ss::encode(f);
    FakeSteam_Inject(ss::channel, bytes.data(), static_cast<int>(bytes.size()));
}
// The text of battle `n` as host game `game` announces it.
static std::string announced(char game, unsigned n) {
    return std::string("GCT-") + game + std::to_string(n) + "\npol1g_con\nimp\npol\nall=1111111\n";
}
// The newest battle announced in `session` after `after`, within 2 s; 0 if none.
static unsigned heard(unsigned session, unsigned after, std::string& text) {
    unsigned number = 0;
    for (int i = 0; i < 40; ++i) {
        if (gc::battle_guest_offered(session, after, number, text)) return number;
        Sleep(50);
    }
    return 0;
}
// Whether battle `n` of `session` has ended, within 2 s.
static bool ended(unsigned session, unsigned n) {
    for (int i = 0; i < 40; ++i) {
        if (gc::battle_guest_over(session) == n) return true;
        Sleep(50);
    }
    return false;
}
// Once host_left reaches `count`, nothing of `session` is offered or ended any more. Checked the
// moment the count changes, so news dropped only later fails.
static bool forgotten(unsigned session, unsigned count) {
    const ULONGLONG until = GetTickCount64() + 3000;
    while (gc::apply_progress().host_left.load() < count)
        if (GetTickCount64() > until) return false;
    unsigned number = 0;
    std::string text;
    return !gc::battle_guest_offered(session, 0, number, text) && gc::battle_guest_over(session) == 0;
}
// hostrestart (one process, the friend): forged hosts in sessions A, B and C. Each session numbers
// its battles from 1, and a battle still pending when its host leaves goes with its session.
static void hostrestart_guest_side() {
    const uint32_t a = 0x0A0A0A0A, b = 0x0B0B0B0B, c = 0x0C0C0C0C;
    forge(ss::Type::hello, a, 0);
    if (!wait_for("\"session\": \"ready\"", 5000)) {
        ++failures;
        return;
    }
    const unsigned first = gc::battle_guest_session();
    CHECK(first == 1);
    std::string text;
    for (unsigned n = 1; n <= 3; ++n) {
        forge(ss::Type::battle, a, n, announced('a', n));
        CHECK(heard(first, n - 1, text) == n && text == announced('a', n));
        forge(ss::Type::battle_over, a, n);
        CHECK(ended(first, n));
    }
    // In the session, a repeat, an earlier battle and its H again change nothing.
    forge(ss::Type::battle, a, 3, announced('x', 3));
    forge(ss::Type::battle, a, 2, announced('a', 2));
    forge(ss::Type::battle_over, a, 2);
    forge(ss::Type::hello, a, 0);
    Sleep(400);
    unsigned number = 0;
    CHECK(gc::battle_guest_session() == first && !gc::battle_guest_offered(first, 3, number, text));
    CHECK(gc::battle_guest_offered(first, 2, number, text) && number == 3 && text == announced('a', 3));
    CHECK(gc::battle_guest_over(first) == 3);
    // Battle 4 waits for this PC's busy game; then the host's game goes, and this PC leaves the lobby.
    // Battle 4 goes with the session.
    forge(ss::Type::battle, a, 4, announced('a', 4));
    CHECK(heard(first, 3, text) == 4);
    forge(ss::Type::bye, a, 0);
    CHECK(forgotten(first, 1));
    if (!wait_for("\"state\": \"left\"", 3000)) {
        ++failures;
        return;
    }
    CHECK(gc::apply_progress().host_left.load() == 1);
    // The next invite: a new lobby and a new session, which starts with nothing announced or ended.
    // The earlier session hears nothing more.
    FakeSteam_JoinRequest();
    if (!wait_for("\"state\": \"joined\"", 5000) || !wait_for("\"build_match\": true", 5000)) {
        ++failures;
        return;
    }
    forge(ss::Type::hello, b, 0);
    unsigned second = first;
    for (int i = 0; i < 40 && second == first; ++i) {
        second = gc::battle_guest_session();
        if (second == first) Sleep(50);
    }
    CHECK(second == first + 1);
    CHECK(!gc::battle_guest_offered(second, 0, number, text) && gc::battle_guest_over(second) == 0);
    CHECK(!gc::battle_guest_offered(first, 0, number, text) && gc::battle_guest_over(first) == 0);
    // Its battle 1 is heard, though the last session reached battle 3.
    forge(ss::Type::battle, b, 1, announced('b', 1));
    CHECK(heard(second, 0, text) == 1 && text == announced('b', 1));
    forge(ss::Type::battle, b, 1, announced('x', 1));
    forge(ss::Type::battle, a, 4, announced('a', 4));
    forge(ss::Type::battle_over, a, 1);
    Sleep(400);
    CHECK(!gc::battle_guest_offered(second, 1, number, text));
    CHECK(gc::battle_guest_offered(second, 0, number, text) && number == 1 && text == announced('b', 1));
    CHECK(gc::battle_guest_over(second) == 0);
    forge(ss::Type::battle_over, b, 1);
    CHECK(ended(second, 1));
    // The game is busy again when this host announces battle 2 and leaves too: battle 2 goes with
    // session B, so a game thread that takes session B only now finds nothing to join.
    forge(ss::Type::battle, b, 2, announced('b', 2));
    CHECK(heard(second, 1, text) == 2);
    forge(ss::Type::bye, b, 0);
    CHECK(forgotten(second, 2));
    if (!wait_for("\"state\": \"left\"", 3000)) {
        ++failures;
        return;
    }
    CHECK(gc::apply_progress().host_left.load() == 2);
    // The invite after that: session C counts from 1, and B's battle 2 never comes back.
    FakeSteam_JoinRequest();
    if (!wait_for("\"state\": \"joined\"", 5000) || !wait_for("\"build_match\": true", 5000)) {
        ++failures;
        return;
    }
    forge(ss::Type::hello, c, 0);
    unsigned third = second;
    for (int i = 0; i < 40 && third == second; ++i) {
        third = gc::battle_guest_session();
        if (third == second) Sleep(50);
    }
    CHECK(third == second + 1);
    CHECK(!gc::battle_guest_offered(third, 0, number, text) && !gc::battle_guest_offered(second, 0, number, text));
    forge(ss::Type::battle, c, 1, announced('c', 1));
    CHECK(heard(third, 0, text) == 1 && text == announced('c', 1));
}

// Co-op for up to four players (coop3, coop4, battle3, coopback, coopjoin): the host's process starts
// its friends' processes, each its own Steam user in one lobby (the fake's hub).
constexpr char host_id[] = "76561197960287930";  // the host, the hub's index 0
// The status file's peer lines, one per friend.
static std::vector<std::string> peer_lines() {
    const std::string s = status();
    std::vector<std::string> lines;
    const size_t at = s.find("\"peers\": [");
    if (at == std::string::npos) return lines;
    const size_t end = s.find("],", at);
    for (size_t p = s.find("{\"id\": ", at); p != std::string::npos && p < end; p = s.find("{\"id\": ", p + 1))
        lines.push_back(s.substr(p, s.find('}', p) - p + 1));
    return lines;
}
// How many peer lines contain `needle`.
static unsigned peers_with(const char* needle) {
    unsigned n = 0;
    for (const std::string& line : peer_lines()) n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}
// Waits until exactly `count` peer lines contain `needle`.
static bool wait_peers(const char* needle, unsigned count, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (peers_with(needle) == count) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for %u peer(s) with %s\n%s", count, needle, status().c_str());
    return false;
}
// Waits for the lobby to be joinable, or not.
static bool wait_joinable(bool open, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (FakeSteam_Joinable() == open) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for the lobby's joinable=%d\n", open);
    return false;
}
// Waits for the host's party rows to read exactly `want`.
static bool wait_rows(const std::string& want, DWORD ms) {
    unsigned seq = 0;
    std::string rows;
    for (DWORD t = 0; t < ms; t += 50) {
        if (gc::party_rows(seq, rows) && rows == want) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for the party rows %s(now %s)\n", want.c_str(), rows.c_str());
    return false;
}
// Starts a friend's process for `mode` as hub member `index`.
static PROCESS_INFORMATION start_friend(const std::string& mode, int index) {
    SetEnvironmentVariableA("GCT_FAKE_INDEX", std::to_string(index).c_str());
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring line = L"\"" + std::wstring(exe) + L"\" " + std::wstring(mode.begin(), mode.end()) +
                        L" +connect_lobby 109775241021786432";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(exe, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    return pi;
}
// Waits for a menu answer to be `yes`.
static bool wait_answer(const std::string& question, bool yes, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 50) {
        if (gc::menu_answer(question.c_str()) == yes) return true;
        Sleep(50);
    }
    std::printf("Timed out waiting for %s=%d\n", question.c_str(), yes);
    return false;
}
// Waits for every friend's process to exit with 0.
static bool wait_children(std::vector<PROCESS_INFORMATION>& children) {
    bool ok = true;
    for (PROCESS_INFORMATION& child : children) {
        DWORD code = 1;
        ok = WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code) &&
             code == 0 && ok;
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    }
    return ok;
}
// coop3 and coop4, the host: two or three friends pair, each in its own session. Start holds for
// all; the last friend to join leaves mid-game; the host leaving its galaxy ends the rest.
static void coop_host_side(const std::string& friend_mode, int friends) {
    gc::menu_event("invite:3");
    if (!wait_for("\"state\": \"hosting\"", 5000)) {
        ++failures;
        return;
    }
    CHECK(wait_joinable(true, 2000) && FakeSteam_MemberLimit() == 4);  // a co-op lobby holds four
    std::vector<PROCESS_INFORMATION> children;
    // The first friend joins and pairs in slot 1; the party rows show it ready, and nothing holds
    // before Start.
    children.push_back(start_friend(friend_mode, 1));
    if (!wait_peers("\"session\": \"ready\"", 1, 10000)) {
        ++failures;
        return;
    }
    Sleep(300);
    CHECK(gc::menu_answer("paired") && !gc::hold_wanted());
    CHECK(status().find("\"peer\": \"76561197960287931\"") != std::string::npos);
    unsigned seq = 0;
    std::string rows;
    CHECK(gc::party_rows(seq, rows) && rows == "2\n1:ready:Ben\n");
    // The others join together.
    for (int i = 2; i <= friends; ++i) children.push_back(start_friend(friend_mode, i));
    CHECK(wait_peers("\"session\": \"ready\"", static_cast<unsigned>(friends), 15000));
    const std::vector<std::string> lines = peer_lines();
    CHECK(lines.size() == static_cast<size_t>(friends));
    CHECK(!lines.empty() && lines[0].find("{\"id\": \"76561197960287931\", \"index\": 1, \"name\": \"Ben\"") == 0);
    std::vector<std::string> sessions;
    for (int i = 0; i < friends && i < static_cast<int>(lines.size()); ++i) {
        CHECK(lines[i].find("\"index\": " + std::to_string(i + 1) + ",") != std::string::npos);
        CHECK(lines[i].find("\"build_match\": true") != std::string::npos);
        const size_t at = lines[i].find("\"session_id\": ");
        sessions.push_back(at == std::string::npos ? std::string() : lines[i].substr(at, lines[i].find(',', at) - at));
    }
    for (size_t i = 0; i < sessions.size(); ++i)
        for (size_t j = i + 1; j < sessions.size(); ++j) CHECK(sessions[i] != sessions[j]);  // a session id each
    CHECK(status().find("\"members\": " + std::to_string(friends + 1) + ",") != std::string::npos);
    CHECK(wait_joinable(friends + 1 < 4, 2000));  // full at four; until then it takes friends
    Sleep(200);
    CHECK(gc::party_rows(seq, rows) && rows.find(std::to_string(friends + 1) + "\n1:ready:Ben\n") == 0);
    CHECK(rows.find(":ready:Cai\n") != std::string::npos &&
          (friends < 3 || rows.find(":ready:Dee\n") != std::string::npos));
    // Invite friends on the party screen opens the overlay again, until the lobby is full.
    const int dialogs = FakeSteam_InviteDialogs();
    CHECK(dialogs == 1);
    gc::menu_event("invite_more");
    Sleep(300);
    CHECK(FakeSteam_InviteDialogs() == (friends < 3 ? 2 : 1) && gc::menu_answer("paired"));
    // Start: the host's galaxy is held for every friend ready now.
    gc::menu_event("start");
    CHECK(wait_answer("hold_loading_" + std::to_string(friends), true, 1000) && gc::hold_wanted());
    gc::hold_event("shown");
    // The galaxy: every friend gets every revision.
    const std::vector<std::string> tags = fixture_tags();
    CHECK(tags.size() > 1000 && fixture_snapshots().size() == 7);
    for (size_t i = 0; i < tags.size(); ++i) {
        gc::capture_channel(tags[i].c_str());
        if (i % 100 == 0) Sleep(5);
    }
    CHECK(wait_peers("\"acked\": 7}", static_cast<unsigned>(friends), 20000));
    // The friends' games show it one after another, 1.5 s apart: the hold waits for the last.
    CHECK(wait_answer("hold_loading_" + std::to_string(friends - 1), true, 5000) && gc::hold_wanted());
    bool released = false;
    for (int i = 0; i < 200 && !released; ++i) {
        released = !gc::hold_wanted();
        if (!released) Sleep(100);
    }
    CHECK(released && !gc::menu_answer("hold_loading_1"));
    // The host's galaxy shows and the game has started; the lobby still takes friends until full, and
    // the pause menu offers Invite friends while it does.
    gc::note_place(2);
    Sleep(300);
    CHECK(FakeSteam_Joinable() == (friends + 1 < 4) && gc::menu_answer("party_invite") == (friends + 1 < 4));
    // The last friend to join leaves the host's galaxy: only its session ends.
    const std::string leaver = "\"id\": \"7656119796028793" + std::to_string(friends) + "\"";
    CHECK(wait_peers(leaver.c_str(), 0, 15000));
    Sleep(300);
    CHECK(peer_lines().size() == static_cast<size_t>(friends - 1));
    CHECK(peers_with("\"session\": \"ready\"") == static_cast<unsigned>(friends - 1));
    CHECK(status().find("\"state\": \"hosting\"") != std::string::npos);
    CHECK(status().find("\"session_end\": \"\"") != std::string::npos);
    CHECK(gc::menu_answer("paired") && gc::apply_progress().friend_left.load() == 0);  // no save prompt yet
    CHECK(wait_joinable(true, 3000) && gc::menu_answer("party_invite"));               // room again
    // The party rows carry a note naming it (L, with the slot it joined in) until the host's galaxy
    // has shown it.
    const std::string gone = friends == 2 ? "Cai" : "Dee";
    CHECK(gc::party_rows(seq, rows) && rows.find(":ready:" + gone + "\n") == std::string::npos);
    const size_t note = rows.find("\nL1:");
    CHECK(note != std::string::npos && rows.substr(note + 5) == ":" + gone + "\n");
    CHECK(friends == 3 || (note != std::string::npos && rows.substr(note) == "\nL1:2:Cai\n"));
    gc::menu_event("noted:1");
    bool dropped = false;
    for (int i = 0; i < 40 && !dropped; ++i) {
        dropped = gc::party_rows(seq, rows) && rows.find("\nL") == std::string::npos;
        if (!dropped) Sleep(50);
    }
    CHECK(dropped && rows.find("\n1:ready:Ben\n") != std::string::npos);
    // The host leaves its galaxy: each friend left is told.
    gc::note_place(1);
    CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 3000));
    CHECK(wait_for("\"state\": \"left\"", 2000));
    CHECK(wait_children(children));
    CHECK(FakeSteam_FriendPackets() == 0);  // no friend's game ever sends to another friend's
}
// A coop3 or coop4 friend: pairs with the host alone and shows revision 7 in turn; the last to join
// leaves mid-game, the others stay until the host leaves.
static void coop_friend_side(int friends, int index) {
    const std::vector<cap::Validated> expected = fixture_snapshots();
    if (expected.size() != 7) {
        CHECK(!"fixture did not validate");
        return;
    }
    if (!wait_for("\"state\": \"joined\"", 10000) || !wait_for("\"session\": \"ready\"", 10000)) {
        ++failures;
        return;
    }
    // Its host alone, the lobby's owner, though the other friends' HELLOs reach it too.
    CHECK(status().find(std::string("\"peer\": \"") + host_id + "\"") != std::string::npos);
    if (!wait_for("\"session_received\": 7,", 20000)) {
        ++failures;
        return;
    }
    CHECK(holds(7, expected.back().normalized_sha256));
    CHECK(peer_lines().size() == 1 && peers_with(host_id) == 1);
    // Its waiting popup counts the party: every friend has joined.
    CHECK(gc::menu_answer(("lobby_members_" + std::to_string(friends + 1)).c_str()));
    CHECK(!gc::menu_answer("guest_ended"));
    // Its game shows the host's revision 7 (L for the host's ready check), 1.5 s after the friend
    // before it.
    Sleep(static_cast<DWORD>(index - 1) * 1500);
    gc::apply_progress().applied_offer.store(7, std::memory_order_release);
    gc::apply_progress().applied.store(7, std::memory_order_release);
    gc::apply_progress().in_galaxy.store(true);
    gc::note_place(2);
    if (index == friends) {
        // The last friend to join leaves the host's galaxy for the menus.
        Sleep(2500);
        gc::apply_progress().in_galaxy.store(false);
        gc::note_place(1);
        CHECK(wait_for("\"session_end\": \"the friend left the host's galaxy\"", 3000));
        CHECK(wait_for("\"state\": \"left\"", 2000));
        return;
    }
    // The others play on until the host leaves its galaxy.
    CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 30000));
    CHECK(gc::apply_progress().host_left.load() == 1);
    CHECK(gc::apply_progress().friend_left.load() == 0);  // co-op: no save prompt on a friend's PC
    CHECK(wait_for("\"state\": \"left\"", 2000));
}

// battle3: the host's battle with two friends. Broadcasts reach both, packets for one friend reach
// it alone, each friend's J is kept by slot, and no tunnel packet passes between the friends.
static void battle3_host_side() {
    gc::menu_event("invite:3");
    if (!wait_for("\"state\": \"hosting\"", 5000)) {
        ++failures;
        return;
    }
    std::vector<PROCESS_INFORMATION> children;
    children.push_back(start_friend("battle3_friend", 1));
    if (!wait_peers("\"session\": \"ready\"", 1, 10000)) {
        ++failures;
        return;
    }
    children.push_back(start_friend("battle3_friend", 2));
    if (!wait_peers("\"session\": \"ready\"", 2, 10000)) {
        ++failures;
        return;
    }
    Sleep(300);
    CHECK(gc::battle_party() == 3);
    gc::note_place(2);
    gc::battle_host_announce(1, battle_text);
    gc::battle_seal(true);
    // Until each friend's game answers: broadcasts for both, and a packet for each.
    bool answered = false;
    for (int i = 0; i < 100 && !answered; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::listener, gc::battle::From::broadcast, "session info", 12);
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "for 1", 5, 1);
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "for 2", 5, 2);
        Sleep(100);
        answered = gc::battle_tunnel_waiting(1) > 0 && gc::battle_tunnel_waiting(2) > 0;
    }
    CHECK(answered);
    CHECK(gc::battle_host_told(1) == (0x2u | 0x4u) && !gc::battle_host_told(2));
    // Three packets for friend 1 alone, then five broadcasts.
    const unsigned to1 = FakeSteam_TunnelPackets(0, 1), to2 = FakeSteam_TunnelPackets(0, 2);
    for (int i = 0; i < 3; ++i) gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "only 1", 6, 1);
    for (int i = 0; i < 5; ++i)
        gc::battle_tunnel_queue(gc::battle::To::listener, gc::battle::From::broadcast, "all", 3);
    Sleep(500);
    CHECK(FakeSteam_TunnelPackets(0, 1) - to1 == 8 && FakeSteam_TunnelPackets(0, 2) - to2 == 5);
    CHECK(gc::battle_seal_counts().dropped == 0);
    // Each friend's report, by its slot.
    unsigned seen[3] = {}, seq = 0, number = 0;
    gc::battle::Part last[3] = {};
    gc::battle::Part part = gc::battle::Part::none;
    for (int i = 0; i < 100 && !(last[1] == gc::battle::Part::left && last[2] == gc::battle::Part::joined); ++i) {
        for (unsigned slot = 1; slot <= 2; ++slot)
            if (gc::battle_host_report(slot, seen[slot], seq, number, part)) {
                seen[slot] = seq;
                if (number == 1) last[slot] = part;
            }
        Sleep(50);
    }
    CHECK(last[1] == gc::battle::Part::left && last[2] == gc::battle::Part::joined);
    // The result and the end, for both.
    gc::battle_host_result(1, 0xFFFFFFFBu);
    gc::battle_host_over(1);
    Sleep(1500);
    gc::battle_seal(false);
    // The host closes its game: each friend is told.
    gc::menu_event("closing");
    CHECK(wait_for("\"state\": \"left\"", 2000));
    CHECK(wait_children(children));
    CHECK(FakeSteam_FriendPackets() == 0 && !FakeSteam_TunnelPackets(1, 2) && !FakeSteam_TunnelPackets(2, 1));
}
// A battle3 friend: joins the battle; friend 1 leaves it early, friend 2 stays to the end.
static void battle3_friend_side(int index) {
    if (!wait_for("\"state\": \"joined\"", 10000) || !wait_for("\"session\": \"ready\"", 10000)) {
        ++failures;
        return;
    }
    const unsigned session = gc::battle_guest_session();
    unsigned number = 0;
    std::string text;
    bool offered = false;
    for (int i = 0; i < 100 && !offered; ++i) {
        offered = gc::battle_guest_offered(session, 0, number, text);
        if (!offered) Sleep(100);
    }
    CHECK(offered && number == 1 && text == battle_text);
    gc::apply_progress().in_galaxy.store(true);
    gc::note_place(2);
    gc::battle_seal(true);
    bool received = false;
    for (int i = 0; i < 200 && !received; ++i) {
        gc::battle_tunnel_queue(gc::battle::To::game, gc::battle::From::game, "join", 4);
        Sleep(50);
        received = gc::battle_tunnel_waiting() >= 2;  // the host's broadcast and its packet for this friend
    }
    CHECK(received);
    gc::battle_guest_report(1, gc::battle::Part::joined);
    if (index == 1) {
        Sleep(300);
        gc::battle_guest_report(1, gc::battle::Part::left);
    }
    bool over = false;
    for (int i = 0; i < 100 && !over; ++i) {
        over = gc::battle_guest_over(session) == 1;
        if (!over) Sleep(100);
    }
    unsigned won = 0, mask = 0;
    CHECK(over && gc::battle_guest_result(session, won, mask) && won == 1 && mask == 0xFFFFFFFBu);
    CHECK(gc::battle_seal_counts().stray == 0);  // its game sends only to its host
    gc::battle_seal(false);
    CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 10000));
    CHECK(wait_for("\"state\": \"left\"", 2000));
}

// coopback: two friends pair and the second backs out of its waiting popup, with no note before the
// game starts. Then the host backs out of its party screen; the first friend is told. Nobody saves.
static void coopback_host_side() {
    gc::menu_event("invite:2");
    if (!wait_for("\"state\": \"hosting\"", 5000)) {
        ++failures;
        return;
    }
    std::vector<PROCESS_INFORMATION> children;
    for (int i = 1; i <= 2; ++i) children.push_back(start_friend("coopback_friend", i));
    CHECK(wait_peers("\"session\": \"ready\"", 2, 15000));
    CHECK(wait_peers("\"id\": \"76561197960287932\"", 0, 10000));
    CHECK(wait_rows("2\n1:ready:Ben\n", 3000));  // no "L" line: nothing to say
    CHECK(peers_with("\"session\": \"ready\"") == 1 && status().find("\"session_end\": \"\"") != std::string::npos);
    CHECK(gc::apply_progress().friend_left.load() == 0);
    Sleep(300);
    gc::menu_event("leave");
    CHECK(wait_for("\"session_end\": \"the host left the party screen\"", 2000));
    CHECK(wait_for("\"state\": \"left\"", 2000));
    unsigned seq = 0;
    std::string rows;
    CHECK(!gc::party_rows(seq, rows) && !gc::hold_wanted());  // no lobby, no party
    CHECK(gc::apply_progress().friend_left.load() == 0);
    CHECK(wait_children(children));
}
// A coopback friend: the second backs out of its popup; the first hears the game ended before it began.
static void coopback_friend_side(int index) {
    if (!wait_for("\"state\": \"joined\"", 10000) || !wait_for("\"session\": \"ready\"", 10000)) {
        ++failures;
        return;
    }
    CHECK(gc::menu_answer("guest_wait") && !gc::menu_answer("guest_ended"));
    if (index == 2) {
        // Back on its waiting popup, once the host has had time to pair both.
        Sleep(1500);
        gc::menu_event("leave");
        CHECK(wait_for("\"session_end\": \"the friend left on its waiting popup\"", 2000));
        CHECK(wait_for("\"state\": \"left\"", 2000));
        return;
    }
    CHECK(wait_for("\"state\": \"left\"", 10000));
    CHECK(wait_answer("guest_ended", true, 1000) && !gc::menu_answer("guest_wait"));
    CHECK(gc::apply_progress().host_left.load() == 1 && gc::apply_progress().friend_left.load() == 0);
}

// coopjoin: the host starts with one friend and its lobby stays open. A second friend joins mid-game,
// gets the newest revision without holding the host, and the host's galaxy names it once (J).
static void coopjoin_host_side() {
    gc::menu_event("invite:3");
    if (!wait_for("\"state\": \"hosting\"", 5000)) {
        ++failures;
        return;
    }
    std::vector<PROCESS_INFORMATION> children;
    children.push_back(start_friend("coopjoin_friend", 1));
    if (!wait_peers("\"session\": \"ready\"", 1, 10000)) {
        ++failures;
        return;
    }
    Sleep(300);
    gc::menu_event("start");
    CHECK(wait_answer("hold_loading_1", true, 1000) && gc::hold_wanted());
    gc::hold_event("shown");
    const std::vector<std::string> tags = fixture_tags();
    CHECK(tags.size() > 1000 && fixture_snapshots().size() == 7);
    for (size_t i = 0; i < tags.size(); ++i) {
        gc::capture_channel(tags[i].c_str());
        if (i % 100 == 0) Sleep(5);
    }
    CHECK(wait_peers("\"acked\": 7}", 1, 20000));
    // Up to 30 s: a slow CI runner can delay the friend's report, and the game itself waits up to 60 s.
    bool released = false;
    for (int i = 0; i < 300 && !released; ++i) {
        released = !gc::hold_wanted();
        if (!released) Sleep(100);
    }
    CHECK(released);
    // The host's galaxy shows: the game has started, and the lobby still takes friends.
    gc::note_place(2);
    Sleep(300);
    CHECK(FakeSteam_Joinable() && gc::menu_answer("party_invite"));
    CHECK(wait_rows("2\n1:ready:Ben\n", 2000));  // no note for the friend waited for at Start
    // The second friend joins the game in progress: paired, sent revision 7 (the newest), and never
    // waited for.
    const int dialogs = FakeSteam_InviteDialogs();
    gc::menu_event("invite_more");  // the pause menu's Invite friends
    Sleep(300);
    CHECK(FakeSteam_InviteDialogs() == dialogs + 1);
    children.push_back(start_friend("coopjoin_friend", 2));
    CHECK(wait_peers("\"session\": \"ready\"", 2, 15000));
    CHECK(wait_peers("\"acked\": 7}", 2, 20000));
    CHECK(!gc::hold_wanted() && !gc::menu_answer("hold_loading_1"));
    // Its game shows the galaxy: the host's galaxy names it, once.
    CHECK(wait_rows("3\n1:ready:Ben\n2:ready:Cai\nJ1:2:Cai\n", 10000));
    gc::menu_event("noted:1");
    CHECK(wait_rows("3\n1:ready:Ben\n2:ready:Cai\n", 2000));
    CHECK(FakeSteam_Joinable() && gc::menu_answer("party_invite"));  // room for a third
    // The host leaves its galaxy: each friend is told.
    gc::note_place(1);
    CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 3000));
    CHECK(wait_for("\"state\": \"left\"", 2000));
    CHECK(wait_children(children));
}
// A coopjoin friend: gets revision 7 and shows it, then waits for the host to leave.
static void coopjoin_friend_side(int index) {
    const std::vector<cap::Validated> expected = fixture_snapshots();
    if (expected.size() != 7) {
        CHECK(!"fixture did not validate");
        return;
    }
    if (!wait_for("\"state\": \"joined\"", 10000) || !wait_for("\"session\": \"ready\"", 10000)) {
        ++failures;
        return;
    }
    if (!wait_for("\"session_received\": 7,", 20000)) {
        ++failures;
        return;
    }
    CHECK(holds(7, expected.back().normalized_sha256));
    // Its game shows the host's revision 7: for the first friend, the host's ready check; for the
    // second, the game in progress.
    gc::apply_progress().applied_offer.store(7, std::memory_order_release);
    gc::apply_progress().applied.store(7, std::memory_order_release);
    gc::apply_progress().in_galaxy.store(true);
    gc::note_place(2);
    CHECK(wait_for("\"session_end\": \"the host left its galaxy\"", 40000));
    CHECK(gc::apply_progress().host_left.load() == 1 && gc::apply_progress().friend_left.load() == 0);
    CHECK(wait_for("\"state\": \"left\"", 2000));
    CHECK(index == 1 || index == 2);
}

// Runs one scenario. Two-PC scenarios start the other PC as a child process (mode_guest); the co-op
// ones start their friends from the host (mode_friend).
static int run(int argc, char** argv) {
    if (argc < 2) return 2;
    std::string mode = argv[1];
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    PROCESS_INFORMATION child{};
    if (mode == "session" || mode == "versus" || mode == "versusload" || mode == "coop3" || mode == "coop4" ||
        mode == "coopjoin") {
        if (argc < 3) return 2;
        char full[MAX_PATH];
        if (!GetFullPathNameA(argv[2], MAX_PATH, full, nullptr)) return 2;
        SetEnvironmentVariableA("GCT_SESSION_FIXTURE", full);
    }
    if (mode == "coop3" || mode == "coop4" || mode == "coopback" || mode == "battle3" || mode == "coopjoin") {
        // A free loopback port for the hub; this process is the host (index 0) and starts its friends once
        // its lobby exists.
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof a;
        bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof a);
        getsockname(probe, reinterpret_cast<sockaddr*>(&a), &len);
        closesocket(probe);
        SetEnvironmentVariableA("GCT_FAKE_HUB_PORT", std::to_string(ntohs(a.sin_port)).c_str());
        SetEnvironmentVariableA("GCT_FAKE_INDEX", "0");
        mode += "_host";
    }
    if (mode == "relay" || mode == "session" || mode == "hostgone" || mode == "hostends" || mode == "friendends" ||
        mode == "hostcloses" || mode == "friendcloses" || mode == "friendgone" || mode == "battle" ||
        mode == "versus" || mode == "versusbattle" || mode == "versusload" || mode == "versushostcloses" ||
        mode == "versusfriendcloses") {
        // A free loopback port for the two fakes' P2P link, then the other PC's process.
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof a;
        bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof a);
        getsockname(probe, reinterpret_cast<sockaddr*>(&a), &len);
        closesocket(probe);
        SetEnvironmentVariableA("GCT_FAKE_P2P_PORT", std::to_string(ntohs(a.sin_port)).c_str());
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring line = L"\"" + std::wstring(exe) + (L"\" " + std::wstring(mode.begin(), mode.end()) + L"_guest") +
                            L" +connect_lobby 109775241021786432";
        STARTUPINFOW si{sizeof si};
        if (!CreateProcessW(exe, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &child)) return 1;
        mode += "_host";
    }
    SetEnvironmentVariableA("GCT_FAKE_SCENARIO", mode.c_str());
    FakeSteam_SetMainThread(GetCurrentThreadId());
    HMODULE self = GetModuleHandleW(nullptr);
    CreateDirectoryW(steam_dir().c_str(), nullptr);
    if (!gc::open_log(self) || !gc::start_steam_invite(self, self)) {
        std::printf("start failed\n");
        return 1;
    }
    CHECK(!gc::start_steam_invite(self, self));  // one instance per process
    if (!wait_for("\"ready\": true", 5000)) return 1;

    if (mode == "host") {
        // The overlay comes on once it has hooked the renderer, and the menu stops saying it is off.
        CHECK(status().find("\"overlay\": false") != std::string::npos && gc::menu_answer("no_overlay"));
        FakeSteam_SetOverlay(true);
        CHECK(wait_for("\"overlay\": true", 2000) && !gc::menu_answer("no_overlay"));
        // While the game's own call result is outstanding, the layer must not pump callbacks.
        GameTicket ticket;
        SteamAPI_RegisterCallResult(&ticket, 0x9001);  // through the hooked IAT slot
        Sleep(300);
        const LONG before = FakeSteam_WorkerRuns();
        Sleep(800);
        CHECK(FakeSteam_WorkerRuns() == before);
        FakeSteam_CompleteGameCall(1032);
        SteamAPI_RunCallbacks();  // the game's own wait loop
        CHECK(ticket.thread == GetCurrentThreadId());
        Sleep(500);
        CHECK(FakeSteam_WorkerRuns() > before);
        SteamAPI_UnregisterCallResult(&ticket, 0x9001);

        // An invite the scripted friend accepts at once: HELLO and offers both ways, then leaving.
        put(file(L"offer", L".txt"), "host=10.0.0.2\r\n");
        put(file(L"request", L".txt"), "invite\n");
        if (!wait_for("\"offer_sent\": true", 5000)) return 1;
        const std::string s = status();
        CHECK(s.find("\"state\": \"hosting\"") != std::string::npos);
        CHECK(s.find("\"role\": \"host\"") != std::string::npos);
        CHECK(s.find("\"members\": 2,") != std::string::npos);
        CHECK(s.find("\"peer\": \"76561197960287931\"") != std::string::npos);
        CHECK(s.find("\"peer_name\": \"Peer \\\"Two\\\"\"") != std::string::npos);
        CHECK(s.find("\"build_match\": true") != std::string::npos);
        CHECK(s.find("\"peer_offer\": \"bind=10.0.0.5 code=aaaa-bbbb-cccc-dddd-eeee\"") != std::string::npos);
        CHECK(sent("GCT1 HELLO 6217431 coop-12"));
        CHECK(sent("GCT1 OFFER host=10.0.0.2"));
        // A co-op lobby holds four and takes friends until full.
        CHECK(FakeSteam_MemberLimit() == 4 && FakeSteam_Joinable());
        CHECK(status().find(
                  "\"peers\": [\n    {\"id\": \"76561197960287931\", \"index\": 1, \"name\": \"Peer \\\"Two\\\"\"") !=
              std::string::npos);
        char connect[64];
        CHECK(FakeSteam_PresenceConnect(connect, sizeof connect) &&
              !strcmp(connect, "+connect_lobby 109775241021786432"));
        put(file(L"request", L".txt"), "leave");
        CHECK(wait_for("\"state\": \"left\"", 3000));
        CHECK(!FakeSteam_PresenceConnect(connect, sizeof connect));
    } else if (mode == "guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        // Joined from +connect_lobby: offers go both ways, and an invite of its own is refused.
        put(file(L"offer", L".txt"), "bind=10.0.0.5 code=aaaa-bbbb-cccc-dddd-eeee");
        if (!wait_for("\"offer_sent\": true", 5000)) return 1;
        const std::string s = status();
        CHECK(s.find("\"role\": \"guest\"") != std::string::npos);
        CHECK(s.find("\"lobby\": \"109775241021786432\"") != std::string::npos);
        CHECK(s.find("\"peer_offer\": \"host=10.0.0.9\"") != std::string::npos);
        CHECK(sent("GCT1 OFFER bind=10.0.0.5 code=aaaa-bbbb-cccc-dddd-eeee"));
        put(file(L"request", L".txt"), "invite");  // refused while joined
        CHECK(wait_for("\"error\": \"invite_ignored_session_active\"", 3000));
        // The Online menu on a PC that is already a friend's guest says so, once the Steam thread has
        // handled the request.
        gc::menu_event("invite:1");
        CHECK(!gc::menu_answer("failed"));
        Sleep(400);
        CHECK(gc::menu_answer("failed") && gc::menu_answer("guest") && !gc::menu_answer("paired"));
        CHECK(status().find("\"state\": \"joined\"") != std::string::npos);
    } else if (mode == "relay_host") {
        put(file(L"offer", L".txt"), "role=host relay=steam");
        put(file(L"request", L".txt"), "invite");
        relay_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "relay_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        relay_target_side();
    } else if (mode == "session_host") {
        // Invited from the Online menu (player_menu.lua), not the request file.
        gc::menu_event("invite:3");
        Sleep(50);
        CHECK(!gc::menu_answer("paired") && !gc::menu_answer("failed"));
        session_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "session_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        session_guest_side();
    } else if (mode == "hostgone_host") {
        gc::menu_event("invite:2");
        CHECK(wait_for("\"session\": \"ready\"", 8000));
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 40000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "hostgone_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        hostgone_guest_side();
    } else if (mode == "hostends_host" || mode == "friendends_host") {
        ends_host_side(mode == "hostends_host");
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "hostcloses_host" || mode == "friendcloses_host") {
        closes_host_side(mode == "hostcloses_host");
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "hostcloses_guest" || mode == "friendcloses_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        closes_guest_side(mode == "hostcloses_guest");
    } else if (mode == "friendgone_host") {
        friendgone_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "friendgone_guest") {
        if (!wait_for("\"session\": \"ready\"", 8000)) return 1;
        Sleep(14000);  // the host decides without this game's help
    } else if (mode == "hostends_guest" || mode == "friendends_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        ends_guest_side(mode == "hostends_guest");
    } else if (mode == "battle_host") {
        battle_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 40000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "battle_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        battle_guest_side();
    } else if (mode == "hostrestart") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        hostrestart_guest_side();
    } else if (mode == "versus_host") {
        versus_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "versus_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        versus_guest_side();
    } else if (mode == "versusload_host") {
        versusload_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "versusload_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        versusload_guest_side();
    } else if (mode == "versushostcloses_host" || mode == "versusfriendcloses_host") {
        versuscloses_host_side(mode == "versushostcloses_host");
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 30000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "versushostcloses_guest" || mode == "versusfriendcloses_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        versuscloses_guest_side(mode == "versushostcloses_guest");
    } else if (mode == "versusbattle_host") {
        versusbattle_host_side();
        DWORD code = 1;
        CHECK(WaitForSingleObject(child.hProcess, 40000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code) && code == 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    } else if (mode == "versusbattle_guest") {
        if (!wait_for("\"state\": \"joined\"", 5000)) return 1;
        versusbattle_guest_side();
    } else if (mode == "coop3_host" || mode == "coop4_host") {
        coop_host_side(mode.substr(0, 5) + "_friend", mode == "coop3_host" ? 2 : 3);
    } else if (mode == "coop3_friend" || mode == "coop4_friend") {
        char index[8] = "0";
        GetEnvironmentVariableA("GCT_FAKE_INDEX", index, sizeof index);
        coop_friend_side(mode == "coop3_friend" ? 2 : 3, atoi(index));
    } else if (mode == "battle3_host") {
        battle3_host_side();
    } else if (mode == "battle3_friend") {
        char index[8] = "0";
        GetEnvironmentVariableA("GCT_FAKE_INDEX", index, sizeof index);
        battle3_friend_side(atoi(index));
    } else if (mode == "coopback_host") {
        coopback_host_side();
    } else if (mode == "coopjoin_host") {
        coopjoin_host_side();
    } else if (mode == "coopjoin_friend") {
        char index[8] = "0";
        GetEnvironmentVariableA("GCT_FAKE_INDEX", index, sizeof index);
        coopjoin_friend_side(atoi(index));
    } else if (mode == "coopback_friend") {
        char index[8] = "0";
        GetEnvironmentVariableA("GCT_FAKE_INDEX", index, sizeof index);
        coopback_friend_side(atoi(index));
    } else if (mode == "guest_badbuild") {
        // The lobby's build differs, so this PC sends no HELLO.
        CHECK(wait_for("\"error\": \"lobby_build_mismatch\"", 5000));
        CHECK(status().find("\"state\": \"error\"") != std::string::npos);
        CHECK(!sent("GCT1 HELLO 6217431 coop-12"));
    } else return 2;

    if (failures) {
        std::printf("%d steam harness check(s) failed (%s)\n", failures, mode.c_str());
        return 1;
    }
    std::printf("Steam invite harness passed (%s)\n", mode.c_str());
    return 0;
}

// The Steam layer's thread never stops, so main ends through ExitProcess, which stops that thread before any
// global is destroyed.
int main(int argc, char** argv) {
    const int code = run(argc, argv);
    std::fflush(stdout);
    ExitProcess(static_cast<UINT>(code));
}
