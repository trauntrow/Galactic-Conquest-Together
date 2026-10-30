// Tests the Steam invite layer's pure logic (src/steam_invite_logic.h); needs neither Steam nor the game.
#include "../src/steam_invite_logic.h"
#include <cstdio>
#include <string>

namespace s = gc::steam;
#include "check.h"

int main() {
    const uint64_t lobby = 109775241021786432ull;  // universe 1, type 8 (chat)
    const uint64_t user = 76561197960287930ull;    // universe 1, type 1 (individual)
    // Lobby and user IDs are told apart by their universe and type.
    CHECK(s::valid_lobby_id(lobby));
    CHECK(!s::valid_lobby_id(user));
    CHECK(s::valid_user_id(user));
    CHECK(!s::valid_user_id(lobby));
    CHECK(!s::valid_lobby_id(0));

    uint64_t id = 0;
    // The +connect_lobby argument Steam adds to the command line: a whole token naming a lobby.
    CHECK(s::parse_connect_lobby("\"C:\\Steam\\BattlefrontII.exe\" +connect_lobby 109775241021786432", id) &&
          id == lobby);
    id = 0;
    CHECK(s::parse_connect_lobby("+connect_lobby  109775241021786432 /nointro", id) && id == lobby);
    CHECK(!s::parse_connect_lobby("+connect_lobby 76561197960287930", id));       // a user, not a lobby
    CHECK(!s::parse_connect_lobby("+connect_lobby 1097752410217864320000", id));  // overflow
    CHECK(!s::parse_connect_lobby("+connect_lobby 10977524102178643x", id));      // trailing junk
    CHECK(!s::parse_connect_lobby("x+connect_lobby 109775241021786432", id));     // not a token
    CHECK(!s::parse_connect_lobby("+connect_lobby", id));
    CHECK(!s::parse_connect_lobby(nullptr, id));

    // Offers: 1 to max_offer bytes of printable ASCII.
    CHECK(s::safe_offer("bind=192.168.1.20 code=abcd-efgh", 32));
    CHECK(!s::safe_offer("", 0));
    CHECK(!s::safe_offer("tab\there", 8));
    std::string big(s::max_offer + 1, 'a');
    CHECK(!s::safe_offer(big.data(), big.size()));

    // Lobby chat messages, "GCT1 <KIND> <body>", round-tripped.
    std::string body;
    std::string wire = s::encode_message(s::Kind::offer, "host=10.0.0.2");
    CHECK(s::decode_message(wire.c_str(), wire.size() + 1, body) == s::Kind::offer && body == "host=10.0.0.2");
    wire = s::encode_message(s::Kind::hello, s::build_id);
    CHECK(s::decode_message(wire.c_str(), wire.size(), body) == s::Kind::hello && body == "6217431 coop-12");
    // No earlier build id matches this one, so earlier DLLs never pair with it.
    for (const char* earlier :
         {"6217431", "6217431 coop-2", "6217431 coop-3", "6217431 coop-4", "6217431 coop-5", "6217431 coop-6",
          "6217431 coop-7", "6217431 coop-8", "6217431 coop-9", "6217431 coop-10", "6217431 coop-11"})
        CHECK(std::string(s::build_id) != earlier);
    // The Remaster in HELLO: a game with it and one without never pair.
    CHECK(s::hello_body(false) == s::build_id && s::hello_body(true) == std::string(s::build_id) + " remaster");
    wire = s::encode_message(s::Kind::hello, s::hello_body(true));
    CHECK(s::decode_message(wire.c_str(), wire.size(), body) == s::Kind::hello && body == s::hello_body(true));
    CHECK(s::hello_body(true) != s::hello_body(false));
    // Malformed messages decode as none: an empty body, another version, a newline, no prefix.
    CHECK(s::decode_message("GCT1 OFFER ", 11, body) == s::Kind::none);
    CHECK(s::decode_message("GCT2 OFFER x", 12, body) == s::Kind::none);
    CHECK(s::decode_message("GCT1 OFFER a\nb", 14, body) == s::Kind::none);
    CHECK(s::decode_message("hello", 5, body) == s::Kind::none);

    // JSON escaping: quotes, backslashes and control characters; UTF-8 passes through.
    CHECK(s::json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\u000a");
    CHECK(s::json_escape("Kl\xc3\xa4us") == "Kl\xc3\xa4us");

    // The status file's JSON.
    s::Status st;
    st.ready = true;
    st.app_id = 6060;
    st.state = "joined";
    st.role = "guest";
    st.self = user;
    st.lobby = lobby;
    st.members = 2;
    st.peer = user + 1;
    st.peer_name = "Dr \"Q\"";
    st.peer_offer = "host=10.0.0.2";
    st.peer_offer_valid = true;
    const std::string json = s::format_status(st, 1234);
    CHECK(json.find("\"pid\": 1234,") != std::string::npos);
    CHECK(json.find("\"lobby\": \"109775241021786432\",") != std::string::npos);
    CHECK(json.find("\"peer_name\": \"Dr \\\"Q\\\"\",") != std::string::npos);
    CHECK(json.find("\"peer_offer\": \"host=10.0.0.2\",") != std::string::npos);
    st.peer_offer_valid = false;
    CHECK(s::format_status(st, 1).find("\"peer_offer\": null,") != std::string::npos);

    // Relay frames: a type letter, the 32-bit epoch, then the payload.
    s::Frame type;
    uint32_t epoch = 0;
    size_t payload = 0;
    std::string f = s::encode_frame(s::Frame::data, 0xA1B2C3D4u, "xyz", 3);
    CHECK(f.size() == 8 && f[0] == 'D' && (unsigned char)f[1] == 0xD4);
    CHECK(s::decode_frame(f.data(), f.size(), type, epoch, payload) && type == s::Frame::data && epoch == 0xA1B2C3D4u &&
          payload == 3);
    f = s::encode_frame(s::Frame::ping, 0);
    CHECK(s::decode_frame(f.data(), f.size(), type, epoch, payload) && type == s::Frame::ping);
    f = s::encode_frame(s::Frame::open, 0);
    CHECK(!s::decode_frame(f.data(), f.size(), type, epoch, payload));  // epoch 0 only for ping
    f = s::encode_frame(s::Frame::data, 7);
    CHECK(!s::decode_frame(f.data(), f.size(), type, epoch, payload));  // empty data
    f = s::encode_frame(s::Frame::close, 7, "x", 1);
    CHECK(!s::decode_frame(f.data(), f.size(), type, epoch, payload));  // close carries nothing
    f = s::encode_frame(s::Frame::data, 7);
    f[0] = 'Z';
    CHECK(!s::decode_frame(f.data(), f.size(), type, epoch, payload));
    std::string huge(s::relay_max_payload + 1, 'a');
    f = s::encode_frame(s::Frame::data, 7, huge.data(), huge.size());
    CHECK(!s::decode_frame(f.data(), f.size(), type, epoch, payload));
    CHECK(!s::decode_frame("D", 1, type, epoch, payload));
    // Relay requests: "host", or "target <port>" with a port from 1024 to 65535.
    bool host = false;
    unsigned port = 0;
    CHECK(s::parse_relay_request("host", host, port) && host);
    CHECK(s::parse_relay_request("target 47615", host, port) && !host && port == 47615);
    CHECK(!s::parse_relay_request("target 80", host, port));
    CHECK(!s::parse_relay_request("target 70000", host, port));
    CHECK(!s::parse_relay_request("target 4761x", host, port));
    CHECK(!s::parse_relay_request("guest", host, port));
    // The session's end and the relay's state in the status file.
    st.session_end = "the host left its galaxy";
    CHECK(s::format_status(st, 1).find("\"session_end\": \"the host left its galaxy\"") != std::string::npos);
    st.relay = "open";
    st.relay_port = 51000;
    st.relay_up = 5;
    CHECK(s::format_status(st, 1).find("\"relay\": \"open\",\n  \"relay_port\": 51000,") != std::string::npos);

    // The host's Online menu popup: its answers come from the lobby and each peer's pairing.
    s::Status m;
    m.ready = true;
    m.overlay = true;
    m.state = "creating";
    m.role = "host";
    CHECK(s::menu_answers(m) == 0);  // lobby on its way
    m.state = "hosting";
    CHECK(s::menu_answers(m) == 0);  // invite out, nobody yet
    m.peers.resize(1);
    m.peers[0].id = user + 1;
    m.peers[0].index = 1;
    CHECK(s::menu_answers(m) == 0);  // joined, no HELLO yet
    m.peers[0].build = s::build_id;
    m.peers[0].build_match = true;
    m.peers[0].session = "opening";
    CHECK(s::menu_answers(m) == 0);  // paired, session not answered
    m.peers[0].session = "ready";
    CHECK(s::menu_answers(m) == s::menu_paired);
    m.peers[0].build = "6217000";
    m.peers[0].build_match = false;
    m.peers[0].session = "off";
    CHECK(s::menu_answers(m) == (s::menu_failed | s::menu_build));
    // A friend with another build ends the wait only while nobody is paired.
    m.peers.resize(2);
    m.peers[1].id = user + 2;
    m.peers[1].index = 2;
    m.peers[1].build = s::build_id;
    m.peers[1].build_match = true;
    m.peers[1].session = "opening";
    CHECK(s::menu_answers(m) == (s::menu_failed | s::menu_build));
    m.peers[1].session = "ready";
    CHECK(s::menu_answers(m) == s::menu_paired);
    m.peers.clear();
    m.state = "error";
    CHECK(s::menu_answers(m) == s::menu_failed);  // Steam refused the lobby
    m.state = "joined";
    m.role = "guest";
    CHECK(s::menu_answers(m) == (s::menu_failed | s::menu_guest));  // in someone else's game
    m.state = "left";
    CHECK(s::menu_answers(m) == 0);  // left it: may host now
    m.overlay = false;
    m.role = "host";
    m.state = "hosting";
    CHECK(s::menu_answers(m) == s::menu_no_overlay);
    // Each answer's name maps to its bit; anything else to none.
    CHECK(s::menu_answer_bit("paired") == s::menu_paired && s::menu_answer_bit("failed") == s::menu_failed &&
          s::menu_answer_bit("build") == s::menu_build && s::menu_answer_bit("guest") == s::menu_guest &&
          s::menu_answer_bit("no_overlay") == s::menu_no_overlay);
    CHECK(!s::menu_answer_bit("paired ") && !s::menu_answer_bit("") && !s::menu_answer_bit(nullptr));
    // The friend's menus: waiting while paired with nothing of the host's yet.
    s::Status g;
    g.ready = true;
    g.overlay = true;
    g.role = "guest";
    g.state = "joining";
    CHECK(!(s::menu_answers(g) & s::menu_guest_wait));
    g.state = "joined";
    g.peer = user;
    g.peer_build = s::build_id;
    g.build_match = true;
    g.session = "ready";
    CHECK(s::menu_answers(g) & s::menu_guest_wait);
    g.apply_pending = true;
    CHECK(!(s::menu_answers(g) & s::menu_guest_wait));  // the galaxy is on its way
    g.apply_pending = false;
    g.apply_in_galaxy = true;  // the host's galaxy shows
    CHECK(!(s::menu_answers(g) & s::menu_guest_wait));
    g.apply_in_galaxy = false;
    g.session_received = 9;
    g.apply_applied = 9;  // back at the menus after it
    CHECK(s::menu_answers(g) & s::menu_guest_wait);
    g.apply = "stopped";  // applying is off: nothing to wait for
    CHECK(!(s::menu_answers(g) & s::menu_guest_wait));
    g.apply = "waiting";
    g.peer_build = "6217431";
    g.build_match = false;
    g.session = "off";  // a host on another build
    CHECK((s::menu_answers(g) & (s::menu_guest_build | s::menu_guest_wait)) == s::menu_guest_build);
    g.peer = 0;
    g.peer_build.clear();
    g.state = "error";
    g.error = "lobby_build_mismatch";
    CHECK(s::menu_answers(g) & s::menu_guest_build);
    g.error = "join_lobby_failed";
    CHECK((s::menu_answers(g) & (s::menu_join_failed | s::menu_guest_build)) == s::menu_join_failed);
    CHECK(s::menu_answer_bit("guest_wait") == s::menu_guest_wait &&
          s::menu_answer_bit("guest_build") == s::menu_guest_build &&
          s::menu_answer_bit("join_failed") == s::menu_join_failed);
    CHECK(!s::menu_answer_bit("host_left"));  // the end of a co-op game shows nothing

    // A co-op lobby holds the host and three friends, a Versus lobby two; joinable until full.
    CHECK(s::lobby_limit(false) == 4 && s::lobby_limit(true) == 2 && s::max_friends == 3);
    CHECK(s::lobby_joinable(1, 4) && s::lobby_joinable(3, 4) && s::lobby_joinable(1, 2));
    CHECK(!s::lobby_joinable(4, 4) && !s::lobby_joinable(2, 2));
    // Whose HELLO a PC takes: the host any other member's; a friend only its host's.
    const uint64_t host_user = user, friend_a = user + 1, friend_b = user + 2;
    CHECK(s::takes_hello(true, friend_a, host_user, 0) && s::takes_hello(true, friend_b, host_user, 0));
    CHECK(!s::takes_hello(true, host_user, host_user, 0));  // its own echo
    CHECK(s::takes_hello(false, host_user, friend_a, host_user));
    CHECK(!s::takes_hello(false, friend_b, friend_a, host_user));  // another friend
    CHECK(!s::takes_hello(false, host_user, friend_a, 0));         // no host known yet
    CHECK(!s::takes_hello(true, lobby, host_user, 0));             // not a user
    // The single peer fields describe the first paired peer, else the first listed.
    s::Status h;
    h.role = "host";
    s::summarize(h);
    CHECK(!h.peer && h.peer_name.empty() && !h.build_match);
    h.peers.resize(3);
    h.peers[0].id = friend_a;
    h.peers[0].index = 1;
    h.peers[0].name = "Ann";
    h.peers[0].build = "6217431";
    h.peers[1].id = friend_b;
    h.peers[1].index = 2;
    h.peers[1].name = "Bo";
    h.peers[1].build = s::build_id;
    h.peers[1].build_match = true;
    h.peers[1].offer = "bind=10.0.0.7";
    h.peers[1].offer_valid = true;
    h.peers[1].session = "ready";
    h.peers[1].session_id = 0xA1B2C3D4u;
    h.peers[1].sent = 7;
    h.peers[1].acked = 6;
    h.peers[2].id = friend_b + 1;
    h.peers[2].index = 3;
    h.peers[2].left_lobby = true;
    s::summarize(h);
    CHECK(h.peer == friend_b && h.peer_name == "Bo" && h.peer_build == s::build_id && h.build_match);
    CHECK(h.peer_offer == "bind=10.0.0.7" && h.peer_offer_valid);
    h.peers[1].build_match = false;
    s::summarize(h);
    CHECK(h.peer == friend_a && h.peer_name == "Ann" && !h.build_match && !h.peer_offer_valid);
    // The status file lists every peer, one line each, after the single fields.
    h.peers[1].build_match = true;
    const std::string listed = s::format_status(h, 1);
    CHECK(
        listed.find(
            "\"peers\": [\n    {\"id\": \"76561197960287931\", \"index\": 1, \"name\": \"Ann\", \"build\": \"6217431\", "
            "\"build_match\": false, \"left_lobby\": false, \"session\": \"off\", \"session_id\": 0, \"sent\": 0, "
            "\"acked\": 0},\n") != std::string::npos);
    CHECK(
        listed.find("{\"id\": \"76561197960287932\", \"index\": 2, \"name\": \"Bo\", \"build\": \"6217431 coop-12\", "
                    "\"build_match\": true, \"left_lobby\": false, \"session\": \"ready\", \"session_id\": 2712847316, "
                    "\"sent\": 7, \"acked\": 6},\n") != std::string::npos);
    CHECK(
        listed.find("\"left_lobby\": true, \"session\": \"off\", \"session_id\": 0, \"sent\": 0, \"acked\": 0}\n  ],\n"
                    "  \"error\": \"\"\n}\n") != std::string::npos);
    CHECK(listed.find("\"peers\"") > listed.find("\"session_id\": "));  // the single fields' keys come first
    h.peers.clear();
    CHECK(s::format_status(h, 1).find("  \"peers\": [],\n  \"error\": \"\"") != std::string::npos);

    // Party names: printable ASCII cut to party_name_max, otherwise "Friend <slot + 1>".
    CHECK(s::party_name("Ben", 1) == "Ben" && s::party_name("Dr \"Q\": two", 2) == "Dr \"Q\": two");
    CHECK(s::party_name("Kl\xc3\xa4us", 1) == "Friend 2" && s::party_name("", 3) == "Friend 4");
    CHECK(s::party_name("tab\there", 2) == "Friend 3");
    CHECK(s::party_name(std::string(40, 'x'), 1) == std::string(s::party_name_max, 'x'));
    // Party screen rows: the member count, then each friend's slot, state and name.
    s::Status party;
    party.members = 4;
    party.peers.resize(3);
    party.peers[0].index = 1;
    party.peers[0].name = "Ben";
    party.peers[0].build = s::build_id;
    party.peers[0].build_match = true;
    party.peers[0].session = "ready";
    party.peers[1].index = 2;
    party.peers[1].name = "Kl\xc3\xa4us";  // no HELLO yet
    party.peers[2].index = 3;
    party.peers[2].name = "Dee";
    party.peers[2].build = "6217431 coop-11";
    CHECK(s::party_rows(party) == "4\n1:ready:Ben\n2:joining:Friend 3\n3:build:Dee\n");
    party.peers[0].session = "opening";  // paired, not answered yet
    party.peers.resize(1);
    party.members = 2;
    CHECK(s::party_rows(party) == "2\n1:joining:Ben\n");
    party.peers.clear();
    party.members = 1;
    CHECK(s::party_rows(party) == "1\n");
    // Then a line per note not yet shown: L for a friend that left, J for one that joined mid-game.
    party.notes.push_back(s::PartyNote{3, 2, "Cai"});
    party.notes.push_back(s::PartyNote{4, 3, "Friend 4"});
    CHECK(s::party_rows(party) == "1\nL3:2:Cai\nL4:3:Friend 4\n");
    party.notes.push_back(s::PartyNote{5, 2, "Eve", true});
    CHECK(s::party_rows(party) == "1\nL3:2:Cai\nL4:3:Friend 4\nJ5:2:Eve\n");
    // The co-op host's pause menu offers Invite friends while its lobby has room; never in Versus,
    // without a lobby, when full or after an error.
    party.ready = true;
    party.overlay = true;
    party.role = "host";
    party.state = "hosting";
    party.mode = "coop";
    party.lobby = lobby;
    CHECK(s::menu_answers(party) & s::menu_party_invite);
    CHECK(s::menu_answer_bit("party_invite") == s::menu_party_invite);
    party.members = 4;
    CHECK(!(s::menu_answers(party) & s::menu_party_invite));
    party.members = 3;
    party.mode = "versus";
    CHECK(!(s::menu_answers(party) & s::menu_party_invite));
    party.mode = "coop";
    party.lobby = 0;
    CHECK(!(s::menu_answers(party) & s::menu_party_invite));
    party.lobby = lobby;
    party.state = "error";
    CHECK(!(s::menu_answers(party) & s::menu_party_invite));
    // The friend's waiting popup: its co-op game ended before it began.
    s::Status ended;
    ended.ready = true;
    ended.overlay = true;
    ended.role = "guest";
    ended.state = "joined";
    CHECK(!(s::menu_answers(ended) & s::menu_guest_ended));
    ended.state = "left";
    CHECK(!(s::menu_answers(ended) & s::menu_guest_ended));  // left without a game
    ended.session_end = "the host left its galaxy";
    CHECK(s::menu_answers(ended) & s::menu_guest_ended);
    CHECK(s::menu_answer_bit("guest_ended") == s::menu_guest_ended);

    if (failures) {
        std::printf("%d steam invite check(s) failed\n", failures);
        return 1;
    }
    std::printf("Steam invite logic checks passed\n");
    return 0;
}
