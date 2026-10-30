// Tests src/battle_logic.h, and the LAN socket wrappers of src/battle_net.cpp over stand-in Winsock calls.
#include <winsock2.h>
#include "../src/battle_logic.h"
#include "../src/battle_net.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The test hook in battle_net.cpp (GC_BATTLE_NET_TEST): wraps stand-in Winsock functions and sockets.
namespace gc {
using SendTo = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using Send = int(WSAAPI*)(SOCKET, const char*, int, int);
using RecvFrom = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
void battle_net_test_setup(const DWORD* globals, SendTo fake_sendto, Send fake_send, RecvFrom fake_recvfrom,
                           SendTo& wrapped_sendto, Send& wrapped_send, RecvFrom& wrapped_recvfrom);
}

#include "check.h"

namespace b = gc::battle;
using Event = b::Tracker::Event;
using GuestEvent = b::Guest::Event;

// The battles= setting and who hosts a battle.
static void modes() {
    CHECK(b::parse_mode("shared") == b::Mode::shared);
    CHECK(b::parse_mode("Hosted") == b::Mode::hosted);
    CHECK(b::parse_mode("single") == b::Mode::single);
    // Shared is the default, for a missing or empty value and for any typo: a typo never turns on hosted.
    CHECK(b::default_mode == b::Mode::shared);
    CHECK(b::parse_mode("") == b::Mode::shared);
    CHECK(b::parse_mode(nullptr) == b::Mode::shared);
    CHECK(b::parse_mode("share") == b::Mode::shared);
    CHECK(b::parse_mode("hosted ") == b::Mode::shared);
    CHECK(b::parse_mode("hostedx") == b::Mode::shared);
    // The words log= takes for off select single: no sharing.
    for (const char* off : {"off", "OFF", "no", "0", "false", "Single"}) CHECK(b::parse_mode(off) == b::Mode::single);
    CHECK(b::parse_mode("offline") == b::Mode::shared);
    CHECK(!strcmp(b::mode_name(b::Mode::hosted), "hosted"));
    CHECK(!strcmp(b::mode_name(b::default_mode), "shared"));
    // Who hosts: every battle when hosted (the one-PC test); only a paired co-op host when shared.
    CHECK(!b::host_battle(b::Mode::single, true));
    CHECK(b::host_battle(b::Mode::hosted, false));
    CHECK(b::host_battle(b::Mode::shared, true));
    CHECK(!b::host_battle(b::Mode::shared, false));
}

// Field checks for a battle announcement: token, mission, side, world and unit lists.
static void names() {
    CHECK(b::valid_token("GCT-0a1b2c3d"));
    CHECK(!b::valid_token("GCT"));
    CHECK(!b::valid_token("XYZ-0a1b"));
    CHECK(!b::valid_token("GCT-0a1b:pol"));
    CHECK(!b::valid_token(std::string("GCT-") + std::string(28, 'a')));
    CHECK(b::valid_mission("pol1g_con") && b::valid_mission("KOR1g_con"));
    CHECK(!b::valid_mission("") && !b::valid_mission("pol1g con") && !b::valid_mission("pol1g:con"));
    CHECK(b::valid_side("imp") && b::valid_side("cis") && !b::valid_side("IMP") && !b::valid_side("xyz"));
    CHECK(b::valid_world("pol") && b::valid_world("spa") && !b::valid_world("") && !b::valid_world("a b"));
    CHECK(b::valid_units("all=1111111") && b::valid_units("all=1111100,imp=0000001"));
    CHECK(!b::valid_units("") && !b::valid_units("all=111111") && !b::valid_units("all=11111112"));
    CHECK(!b::valid_units("xyz=1111111") && !b::valid_units("all=1111111,") &&
          !b::valid_units("all=1111111;imp=1111111"));
    CHECK(!b::valid_units("all=1111111,imp=1111111,rep=1111111,cis=1111111,all=1111111"));
}

// The hosting report the battle script writes, and the session frame's battle text.
static void info() {
    b::Info i;
    CHECK(b::parse_hosting("GCT-0a1b2c3d:pol1g_con:imp:pol:all=1111100,imp=1111111", i));
    CHECK(i.token == "GCT-0a1b2c3d" && i.mission == "pol1g_con" && i.side == "imp" && i.world == "pol" &&
          i.units == "all=1111100,imp=1111111" && i.complete());
    // A report with only the token and mission still hosts.
    CHECK(b::parse_hosting("GCT-1:fel1g_con", i) && i.side.empty() && i.units.empty() && i.valid() && !i.complete());
    // A setup field that does not check out is dropped; the battle is still hosted.
    CHECK(b::parse_hosting("GCT-2:fel1g_con:xyz:pol:all=12", i) && i.side.empty() && i.world == "pol" &&
          i.units.empty());
    CHECK(!b::parse_hosting("GCT-3", i) && !b::parse_hosting("GCT-3:fel1g_con:imp", i));
    CHECK(!b::parse_hosting("GCT-3:fel1g_con:imp:pol:all=1111111:1=leader:extra", i));
    CHECK(!b::parse_hosting("bad token:fel1g_con", i) && !b::parse_hosting(nullptr, i));
    // The sides' bonus cards after the setup, or after empty setup fields.
    CHECK(b::parse_hosting("GCT-4:nab2g_con:all:nab:all=1100000,imp=1100000:1=autoturrets,2=sensor_array", i) &&
          i.complete() && i.bonus == "1=autoturrets,2=sensor_array");
    CHECK(b::parse_hosting("GCT-5:fel1g_con::::2=leader", i) && i.side.empty() && i.units.empty() &&
          i.bonus == "2=leader" && i.valid());
    // A card list that does not check out is dropped; the battle is still hosted.
    CHECK(b::parse_hosting("GCT-6:fel1g_con:imp:pol:all=1111111:extra", i) && i.bonus.empty() && i.side == "imp");
    // The session frame's text: six lines, both ways.
    b::Info full{"GCT-0a1b2c3d", "spa1g_ass", "rep", "kas", "cis=1111111,rep=1011111", "1=bacta_tanks"}, back;
    CHECK(b::decode_info(b::encode_info(full), back) && back.token == full.token && back.mission == full.mission &&
          back.side == full.side && back.world == full.world && back.units == full.units && back.bonus == full.bonus);
    b::Info bare{"GCT-9", "pol1g_con", "", "", "", ""};
    CHECK(b::encode_info(bare) == "GCT-9\npol1g_con\n\n\n\n");
    CHECK(b::decode_info(b::encode_info(bare), back) && back.side.empty() && back.units.empty() && back.bonus.empty());
    CHECK(!b::decode_info("GCT-9\npol1g_con\nimp\npol", back));                                      // four fields
    CHECK(!b::decode_info("GCT-9\npol1g_con\nimp\npol\nall=1111111", back));                         // five fields
    CHECK(b::decode_info("GCT-9\npol1g_con\nimp\npol\nall=1111111\n", back) && back.bonus.empty());  // six: no cards
    CHECK(!b::decode_info("GCT-9\npol1g_con\nimp\npol\nall=1111111\n\n", back));                     // seven fields
    CHECK(!b::decode_info("GCT-9\npol1g_con\nimp\npol\nall=12\n", back));                            // bad units
    CHECK(!b::decode_info("GCT-9\npol1g_con\nimp\npol\nall=1111111\n3=leader", back));               // bad cards
}

// Bonus cards: the list rules, the engine's hashes, the Leader card and the slots each game reads.
static void bonuses() {
    std::string n[2];
    CHECK(b::parse_bonus("", n) && n[0].empty() && n[1].empty());
    CHECK(b::parse_bonus("1=autoturrets", n) && n[0] == "autoturrets" && n[1].empty());
    CHECK(b::parse_bonus("2=leader", n) && n[0].empty() && n[1] == "leader");
    CHECK(b::parse_bonus("1=sensor_array,2=energy_boost", n) && n[0] == "sensor_array" && n[1] == "energy_boost");
    // Each galaxy team once, in order, with a name the script would accept.
    for (const char* bad : {"2=a,1=b", "1=a,1=b", "3=leader", "0=leader", "1=", "=leader", "1=a,", ",1=a", "1=a b",
                            "1=a:b", "1=a\nb", "1:leader", "12=a"})
        CHECK(!b::valid_bonus(bad));
    CHECK(b::valid_bonus(std::string("1=") + std::string(31, 'a')) &&
          !b::valid_bonus(std::string("1=") + std::string(32, 'a')));
    // The engine's hash of each card's full name, as the game's own checks compare it.
    CHECK(b::bonus_hash("autoturrets") == 0xDE68CB69 && b::bonus_hash("leader") == 0xEAC1E9D4);
    CHECK(b::bonus_hash("bacta_tanks") == 0xC10B9CDC && b::bonus_hash("sabotage") == 0xD265E905);
    CHECK(b::bonus_hash("energy_boost") == 0x85AD826D && b::bonus_hash("supplies") == 0xC81956E6);
    CHECK(b::bonus_hash("garrison") == 0x96F36950 && b::bonus_hash("combat_shielding") == 0xDDB212E1);
    CHECK(b::bonus_hash("advanced_blasters") == 0xBB03442E && b::bonus_hash("sensor_array") == 0x56E2CE71);
    CHECK(b::engine_hash("shell") == b::state_shell && b::engine_hash("mission") == b::state_mission);
    CHECK(b::engine_hash("TEAM_BONUS_LEADER") == b::bonus_hash("leader"));  // case does not count
    CHECK(b::bonus_hash("") == 0);                                          // no card: an empty slot
    // The Leader card, on either side.
    CHECK(b::has_leader("1=leader") && b::has_leader("1=supplies,2=leader") && !b::has_leader("1=leaders"));
    CHECK(!b::has_leader("") && !b::has_leader("1=sabotage") && !b::has_leader("3=leader"));
    // The game's slot rule: swapped only under metagame rules when team 2's faction is lower
    // (all 1, imp 2, rep 3, cis 4).
    CHECK(b::bonus_slot(1, true, 2, 1) == 1 && b::bonus_slot(2, true, 2, 1) == 0);    // nab2g: imp, all
    CHECK(b::bonus_slot(1, true, 1, 2) == 0 && b::bonus_slot(2, true, 1, 2) == 1);    // all, imp
    CHECK(b::bonus_slot(1, false, 2, 1) == 0 && b::bonus_slot(2, false, 2, 1) == 1);  // the friend's game
    CHECK(b::bonus_slot(1, true, 3, 3) == 0);
    // Galaxy team 1 (all) has auto-turrets, team 2 (imp) the sensor array. In nab2g_con battle team 1
    // is imp, so the host's game finds all's card in slot 0 while the friend's game reads slot 1.
    const uint32_t galaxy[2] = {b::bonus_hash("autoturrets"), b::bonus_hash("sensor_array")};
    uint32_t out[2] = {7, 7};
    CHECK(b::friend_bonus_slots(galaxy, 2, 1, false, out) && out[0] == galaxy[1] && out[1] == galaxy[0]);
    // Factions in galaxy order: the slots stay as the friend's script filled them.
    CHECK(b::friend_bonus_slots(galaxy, 1, 2, false, out) && out[0] == galaxy[0] && out[1] == galaxy[1]);
    CHECK(b::friend_bonus_slots(galaxy, 3, 4, false, out) && out[0] == galaxy[0] && out[1] == galaxy[1]);
    CHECK(b::friend_bonus_slots(galaxy, 4, 3, false, out) && out[0] == galaxy[1] && out[1] == galaxy[0]);  // cis, rep
    // A friend's game under metagame rules would swap as the host's does.
    CHECK(b::friend_bonus_slots(galaxy, 2, 1, true, out) && out[0] == galaxy[0] && out[1] == galaxy[1]);
    // One side's card only: it moves with its side, and the empty slot too.
    const uint32_t one[2] = {0, b::bonus_hash("leader")};
    CHECK(b::friend_bonus_slots(one, 2, 1, false, out) && out[0] == one[1] && out[1] == 0);
    // Until both teams have a faction, nothing is decided.
    out[0] = out[1] = 7;
    CHECK(!b::friend_bonus_slots(galaxy, 0, 1, false, out) && !b::friend_bonus_slots(galaxy, 2, -1, false, out));
    CHECK(out[0] == 7 && out[1] == 7);
    // For every faction order, the friend's game finds in its slot the card the host's game finds.
    for (int f1 = 1; f1 <= 4; ++f1)
        for (int f2 = 1; f2 <= 4; ++f2) {
            CHECK(b::friend_bonus_slots(galaxy, f1, f2, false, out));
            for (int team = 1; team <= 2; ++team)
                CHECK(out[b::bonus_slot(team, false, f1, f2)] == galaxy[b::bonus_slot(team, true, f1, f2)]);
        }
}

// The host's tracker: a hosting report seals the LAN sockets until the battle's network game closes.
static void tracker() {
    b::Tracker t;
    CHECK(!t.sealed());
    // The script's report seals at once, before its first network call.
    CHECK(t.report("step:connect", 0) == Event::none);
    CHECK(t.report("hosting:GCT-0a1b2c3d:pol1g_con:imp:pol:all=1111100,imp=1111111", 1000) == Event::hosting);
    CHECK(t.sealed() && t.info.token == "GCT-0a1b2c3d" && t.info.mission == "pol1g_con" && t.info.side == "imp" &&
          t.number == 1);
    CHECK(t.state(b::state_shell, 1100) == Event::none);  // the battle card, still in the shell
    CHECK(t.state(b::state_mission, 2000) == Event::launched && t.launched == 1);
    CHECK(t.state(b::state_mission, 2100) == Event::none);  // seen once
    CHECK(!t.cleanup_due() && t.sealed());
    // Back in the shell: the network game is closed once, then the seal lifts.
    CHECK(t.state(b::state_shell, 300000) == Event::ended && t.cleanup_due() && t.sealed());
    t.cleaned();
    CHECK(!t.cleanup_due() && !t.sealed());
    // A single-player battle afterwards is not followed.
    CHECK(t.state(b::state_mission, 400000) == Event::none && t.state(b::state_shell, 500000) == Event::none);
    CHECK(!t.sealed() && t.launched == 1);
    // The next battle has the next number.
    CHECK(t.report("hosting:GCT-11:fel1g_con", 600000) == Event::hosting && t.number == 2 && t.info.side.empty());

    // A fallback to the single-player battle unseals, and its mission is not tracked.
    b::Tracker f;
    CHECK(f.report("hosting:GCT-1:fel1g_con", 0) == Event::hosting);
    CHECK(f.report("fallback:netshell:boom", 10) == Event::fallback && !f.sealed());
    CHECK(f.state(b::state_mission, 20) == Event::none && !f.cleanup_due());
    CHECK(f.report("fallback:again", 30) == Event::none);  // nothing to fall back from

    // No launch and no report: the backstop unseals after a minute.
    b::Tracker a;
    a.state(b::state_shell, 0);
    CHECK(a.report("hosting:GCT-2:fel1g_con", 1000) == Event::hosting);
    CHECK(a.state(b::state_shell, 1000 + b::Tracker::launch_limit_ms) == Event::none && a.sealed());
    CHECK(a.state(b::state_shell, 1001 + b::Tracker::launch_limit_ms) == Event::abandoned && !a.sealed());

    // Malformed reports change nothing.
    b::Tracker m;
    CHECK(m.report("hosting:GCT-3", 0) == Event::none);
    CHECK(m.report("hosting:bad token:pol1g_con", 0) == Event::none);
    CHECK(m.report("hosting:GCT-3:pol1g con", 0) == Event::none);
    CHECK(m.report(nullptr, 0) == Event::none && !m.sealed());
    CHECK(m.state(0, 0) == Event::none);
}

// The friend's tracker for the host's battles: announcement, join, launch, return, and each way out.
static void guest() {
    const b::Info i{"GCT-0a1b2c3d", "pol1g_con", "imp", "pol", "all=1111100,imp=1111111"};
    b::Guest g;
    g.state(b::state_shell, 0);
    CHECK(!g.busy() && !g.sealed());
    // Announced: busy (applying waits), not yet sealed.
    CHECK(g.offer(1, i, 1000) == GuestEvent::offered && g.busy() && !g.sealed() && g.info.mission == "pol1g_con");
    CHECK(g.offer(1, i, 1100) == GuestEvent::none);  // a repeat
    // One battle at a time: a newer one waits, turned down once per announcement though it reads as new
    // on every frame.
    CHECK(g.offer(2, i, 1100) == GuestEvent::ignored && g.ignored == 2 && g.number == 1);
    CHECK(g.offer(2, i, 1116) == GuestEvent::none && g.offer(2, i, 1133) == GuestEvent::none);
    g.joining(5000);
    CHECK(g.phase == b::Guest::Phase::joining && g.sealed() && !g.lobby_due());
    CHECK(g.report("step:connect") == GuestEvent::none);
    // Found: from now on the game thread updates the lobby every frame.
    CHECK(g.report("found") == GuestEvent::found && g.found && g.lobby_due());
    CHECK(g.report("found") == GuestEvent::none && g.lobby_due());  // once
    CHECK(g.state(b::state_mission, 9000) == GuestEvent::launched && g.joined == 1 && g.sealed() && !g.lobby_due());
    // The shell build after the battle asks before the frame hook sees the shell.
    CHECK(!g.returning_now(b::state_mission) && g.returning_now(b::state_shell));
    CHECK(g.state(b::state_shell, 400000) == GuestEvent::ended && g.returning_now(b::state_shell) && g.sealed());
    g.returned();
    CHECK(!g.busy() && !g.sealed() && !g.returning_now(b::state_shell));
    // The host's next battle, turned down while busy, is taken once idle and starts afresh: not found yet.
    CHECK(g.offer(2, i, 500000) == GuestEvent::offered && !g.found);
    CHECK(g.report("found") == GuestEvent::none);  // only while joining

    // The join script gives up: back to spectating; its popup closed the network game.
    b::Guest gave;
    gave.offer(1, i, 0);
    gave.joining(10);
    CHECK(gave.report("fallback:x") == GuestEvent::none);  // the host's word, not the join's
    gave.report("found");
    CHECK(gave.report("joinfail:the friend stayed in the galaxy") == GuestEvent::gave_up && !gave.busy());
    CHECK(!gave.lobby_due() && !gave.close_due() && !gave.sealed());

    // The host's battle ends before this one launches: no more lobby updates.
    b::Guest ended;
    ended.offer(1, i, 0);
    ended.joining(10);
    ended.report("found");
    ended.host_over(1, 20);
    CHECK(!ended.lobby_due() && ended.sealed());

    // An announcement the friend's game cannot start joining in time expires, and so does one whose
    // battle the host has already ended.
    b::Guest late;
    late.offer(1, i, 0);
    CHECK(late.state(0, b::Guest::offer_limit_ms) == GuestEvent::none);
    CHECK(late.state(0, b::Guest::offer_limit_ms + 1) == GuestEvent::expired && !late.busy());
    b::Guest over;
    over.offer(3, i, 0);
    over.host_over(2, 10);  // another battle's end
    CHECK(over.state(0, 20) == GuestEvent::none);
    over.host_over(3, 30);
    CHECK(over.state(0, 40) == GuestEvent::expired);

    // A join that never launches is given up by the backstop. Its network game is still open: sealed
    // until the game thread has closed it.
    b::Guest stuck;
    stuck.offer(1, i, 0);
    stuck.joining(0);
    stuck.report("found");
    CHECK(stuck.state(0, b::Guest::join_limit_ms + 1) == GuestEvent::abandoned);
    CHECK(stuck.close_due() && stuck.sealed() && stuck.busy() && !stuck.lobby_due());
    CHECK(stuck.report("joinfail:late") == GuestEvent::none && stuck.close_due());
    stuck.closed();
    CHECK(!stuck.close_due() && !stuck.sealed() && !stuck.busy());
    CHECK(stuck.offer(2, i, 100000) == GuestEvent::offered);

    // Still in the battle after the host's ended: leave it, once.
    b::Guest left;
    left.state(b::state_shell, 0);
    left.offer(1, i, 0);
    left.joining(0);
    left.state(b::state_mission, 100);
    CHECK(!left.quit_due(200));
    left.host_over(1, 1000);
    CHECK(!left.quit_due(1000 + b::Guest::quit_after_ms - 1));
    CHECK(left.quit_due(1000 + b::Guest::quit_after_ms) && !left.quit_due(1000 + b::Guest::quit_after_ms + 5));
    // Or after its own match ended (the scoreboard), if the host's end comes later.
    b::Guest board;
    board.state(b::state_shell, 0);
    board.offer(1, i, 0);
    board.joining(0);
    board.match_over(50);  // not in the battle yet: ignored
    CHECK(board.match_over_at == 0);
    board.state(b::state_mission, 100);
    board.match_over(2000);
    board.match_over(2500);  // the first end counts
    CHECK(board.match_over_at == 2000 && !board.quit_due(2000 + b::Guest::quit_after_match_ms - 1));
    CHECK(board.quit_due(2000 + b::Guest::quit_after_match_ms) && !board.quit_due(9000));
    board.host_over(1, 9100);
    CHECK(!board.quit_due(20000));                           // once per battle
    CHECK(board.offer(2, i, 30000) == GuestEvent::ignored);  // still in battle 1
    board.state(b::state_shell, 31000);
    board.returned();
    CHECK(board.offer(2, i, 32000) == GuestEvent::offered && board.match_over_at == 0 && !board.quit_sent);

    // The host's result (V): if this game has not ended its match itself within result_wait_ms, the
    // mask is handed over, once per battle.
    b::Guest won;
    won.state(b::state_shell, 0);
    won.offer(1, i, 0);
    won.joining(0);
    won.host_result(1, 4, 50);  // not in the battle yet
    CHECK(won.result_due(60, false) == 0);
    won.state(b::state_mission, 100);
    won.host_result(2, 4, 200);  // another battle's: ignored
    won.host_result(1, 0, 200);  // no winner: ignored
    CHECK(won.result_due(5000, false) == 0);
    won.host_result(1, 4, 1000);
    won.host_result(1, 2, 1100);  // the first result counts
    CHECK(won.result_due(1000 + b::Guest::result_wait_ms - 1, false) == 0);
    CHECK(won.result_due(1000 + b::Guest::result_wait_ms, false) == 4);
    CHECK(won.result_due(9000, false) == 0);  // once
    b::Guest own;                             // this game ended its match itself
    own.state(b::state_shell, 0);
    own.offer(1, i, 0);
    own.joining(0);
    own.state(b::state_mission, 100);
    own.host_result(1, 4, 1000);
    CHECK(own.result_due(1200, true) == 0 && own.result_done && own.result_due(9000, false) == 0);
    own.state(b::state_shell, 9500);
    own.returned();
    CHECK(own.offer(2, i, 10000) == GuestEvent::offered && !own.result_mask && !own.result_done);
    // The host's waves: never slower than quick_wave_s, and the quick window shorter than that.
    CHECK(b::quicker(15.0f, b::quick_wave_s) == b::quick_wave_s && b::quicker(0.2f, b::quick_wave_s) == 0.2f);
    CHECK(b::quick_window_s < b::quick_wave_s);
    // The winning-team mask as the scripts set it, and the host's choice: its game's mask whole, or one
    // that records the same winner.
    CHECK(b::victory_of(1u << 1) == 1 && b::victory_of(1u << 2) == 2);        // MissionVictory
    CHECK(b::victory_of(~(1u << 2)) == 1 && b::victory_of(~(1u << 1)) == 2);  // MissionDefeat
    CHECK(b::victory_of(0) == -1);
    CHECK(b::host_mask(1, 0xFFFFFFFBu) == 0xFFFFFFFBu && b::host_mask(2, 4) == 4);
    CHECK(b::victory_of(b::host_mask(1, 0)) == 1 && b::victory_of(b::host_mask(2, 0)) == 2);
    CHECK(b::host_mask(-1, 0) == 0 && b::host_mask(0, 0) == 0 && b::host_mask(3, 0) == 0);
    // A mask past one byte is handed over whole.
    b::Guest whole;
    whole.state(b::state_shell, 0);
    whole.offer(1, i, 0);
    whole.joining(0);
    whole.state(b::state_mission, 100);
    whole.host_result(1, 0xFFFFFFFBu, 1000);
    CHECK(whole.result_due(1000 + b::Guest::result_wait_ms, false) == 0xFFFFFFFBu);
    // The character table: Felucia's 64 and more (a hero battle's) are taken.
    CHECK(b::character_count_ok(64) && b::character_count_ok(65) && b::character_count_ok(b::max_characters));
    CHECK(!b::character_count_ok(0) && !b::character_count_ok(-1) && !b::character_count_ok(b::max_characters + 1));

    // Nothing invalid is offered. An announcement that does not decode (an empty Info) is turned down
    // once, not on every frame, and a newer valid one is still taken.
    b::Guest bad;
    CHECK(bad.offer(0, i, 0) == GuestEvent::none);
    CHECK(bad.offer(1, b::Info{"x", "pol1g_con", "", "", ""}, 0) == GuestEvent::ignored && !bad.busy());
    CHECK(bad.offer(1, b::Info{}, 16) == GuestEvent::none && !bad.busy() && bad.number == 0);
    CHECK(bad.offer(2, i, 33) == GuestEvent::offered);
}

// Battle edges: the friend's report to the host (frame J), the notes each galaxy shows, the engine's
// network errors, and the ways either side leaves a battle.
static void edges() {
    using Part = b::Part;
    using Note = b::Note;
    const b::Info i{"GCT-0a1b2c3d", "pol1g_con", "imp", "pol", "all=1111100,imp=1111111"};
    CHECK(!strcmp(b::part_name(Part::left), "left") && !strcmp(b::part_name(Part::none), "none"));
    CHECK(b::part_from(1) == Part::joined && b::part_from(4) == Part::left && b::part_from(0) == Part::none &&
          b::part_from(5) == Part::quit && b::part_from(6) == Part::none && !strcmp(b::part_name(Part::quit), "quit"));
    for (Note n : {Note::join_failed, Note::friend_failed, Note::friend_stayed, Note::friend_left, Note::friend_quit})
        CHECK(b::parse_note(b::note_name(n)) == n);
    CHECK(b::parse_note("none") == Note::none && b::parse_note("friend_left ") == Note::none &&
          b::parse_note(nullptr) == Note::none);
    // The engine's network error: a host that quit, or a lost host.
    CHECK(b::engine_error("ifs.mp.joinerrors.hostquit") == b::EngineError::host_quit);
    CHECK(b::engine_error("ifs.mp.joinerrors.connectlost") == b::EngineError::lost);
    CHECK(b::engine_error("ifs.mp.joinerrors.booted") == b::EngineError::lost);
    CHECK(b::engine_error("ifs.mp.joinerrors.full") == b::EngineError::other);
    CHECK(b::engine_error("") == b::EngineError::none && b::engine_error(nullptr) == b::EngineError::none);

    // The friend's game fights the battle to its end: joined, then nothing more.
    b::Guest both;
    both.state(b::state_shell, 0);
    both.offer(1, i, 0);
    both.joining(10);
    CHECK(both.state(b::state_mission, 100) == GuestEvent::launched && both.part(GuestEvent::launched) == Part::joined);
    both.match_over(200000);
    CHECK(both.state(b::state_shell, 204000) == GuestEvent::ended && !both.left_early());
    CHECK(both.part(GuestEvent::ended) == Part::none && !both.note());
    // Its player quits the battle (the pause menu's Quit), as its battle script reports once, only in
    // the battle (quit_own). The host hears quit; its battle and galaxy carry on.
    b::Guest quit;
    quit.state(b::state_shell, 0);
    quit.offer(1, i, 0);
    quit.joining(0);
    CHECK(quit.report("quit_own") == GuestEvent::none && !quit.own_quit);  // not in the battle yet
    quit.state(b::state_mission, 10);
    CHECK(quit.report("quit_own") == GuestEvent::own_quit && quit.report("quit_own") == GuestEvent::none);
    CHECK(quit.state(b::state_shell, 50000) == GuestEvent::ended && quit.left_early() && quit.quit_early());
    CHECK(quit.part(GuestEvent::ended) == Part::quit && !quit.note() && quit.returning_now(b::state_shell));
    quit.returned();
    CHECK(!quit.busy());
    // The host quits its battle and this game follows before the DLL reads the engine's hostquit: it
    // leaves early without its player quitting, so the host hears left, which costs no forfeit.
    b::Guest booted;
    booted.state(b::state_shell, 0);
    booted.offer(1, i, 0);
    booted.joining(0);
    booted.state(b::state_mission, 10);
    CHECK(booted.state(b::state_shell, 20000) == GuestEvent::ended && booted.left_early() && !booted.quit_early());
    CHECK(booted.part(GuestEvent::ended) == Part::left);
    // A new battle forgets the last one's quit.
    booted.returned();
    booted.offer(2, i, 30000);
    booted.joining(30000);
    CHECK(!booted.own_quit);
    // The DLL's own leave runs the same ScriptCB_QuitToShell: not the player's quit, and no quit for
    // the host.
    b::Guest led;
    led.state(b::state_shell, 0);
    led.offer(1, i, 0);
    led.joining(0);
    led.state(b::state_mission, 10);
    led.host_over(1, 1000);
    CHECK(led.quit_due(1000 + b::Guest::quit_after_ms) && led.report("quit_own") == GuestEvent::none && !led.own_quit);
    CHECK(led.state(b::state_shell, 3000) == GuestEvent::ended && !led.quit_early() &&
          led.part(GuestEvent::ended) == Part::none);
    // Its engine loses a host that is still there (15 s without a packet): it leaves 1 s later, and
    // the host hears that it left.
    b::Guest lost;
    lost.state(b::state_shell, 0);
    lost.offer(1, i, 0);
    lost.joining(0);
    lost.state(b::state_mission, 10);
    lost.engine_lost(false, 30000);
    CHECK(lost.lost_at == 30000 && !lost.host_quit && !lost.quit_due(30000 + b::Guest::quit_after_ms - 1));
    CHECK(lost.quit_due(30000 + b::Guest::quit_after_ms) && !lost.quit_due(40000));
    lost.engine_lost(true, 31000);  // the first loss counts
    CHECK(!lost.host_quit && lost.lost_at == 30000);
    CHECK(lost.state(b::state_shell, 33000) == GuestEvent::ended && lost.part(GuestEvent::ended) == Part::left);
    CHECK(!lost.quit_early());  // the DLL's own leave: no quit
    // The error popup's OK after the engine lost the host, before the DLL's own leave: still a lost
    // host, not a quit.
    b::Guest ok_left;
    ok_left.state(b::state_shell, 0);
    ok_left.offer(1, i, 0);
    ok_left.joining(0);
    ok_left.state(b::state_mission, 10);
    ok_left.engine_lost(false, 30000);
    CHECK(ok_left.state(b::state_shell, 30500) == GuestEvent::ended && !ok_left.quit_sent);
    CHECK(ok_left.part(GuestEvent::ended) == Part::left && !ok_left.quit_early());
    // The host quits the battle (pause menu): the engine records hostquit and quits too; this game
    // follows, and the host is not told it left.
    b::Guest hq;
    hq.state(b::state_shell, 0);
    hq.offer(1, i, 0);
    hq.joining(0);
    hq.state(b::state_mission, 10);
    hq.engine_lost(true, 60000);
    CHECK(hq.host_quit && hq.quit_due(60000 + b::Guest::quit_after_ms));
    CHECK(hq.state(b::state_shell, 62000) == GuestEvent::ended && !hq.left_early() &&
          hq.part(GuestEvent::ended) == Part::none);
    // Lost only counts in the battle.
    b::Guest early;
    early.offer(1, i, 0);
    early.engine_lost(false, 5);
    CHECK(!early.lost_at);
    // The host is gone mid-battle (its session ended): leave 1 s later, and report nothing to it.
    b::Guest gone;
    gone.state(b::state_shell, 0);
    gone.offer(1, i, 0);
    gone.joining(0);
    gone.state(b::state_mission, 10);
    gone.host_gone(70000);
    gone.host_gone(71000);  // once
    CHECK(gone.gone_at == 70000 && !gone.quit_due(70000 + b::Guest::quit_after_ms - 1));
    CHECK(gone.quit_due(70000 + b::Guest::quit_after_ms) && !gone.quit_due(80000));
    CHECK(gone.state(b::state_shell, 72000) == GuestEvent::ended && !gone.left_early());
    CHECK(gone.part(GuestEvent::ended) == Part::none && gone.part(GuestEvent::launched) == Part::none && !gone.note());
    CHECK(gone.returning_now(b::state_shell));  // back to its galaxy first, then the menus
    // Gone before the friend's game joined: the announcement expires at once, quietly.
    b::Guest gone_offer;
    gone_offer.offer(1, i, 0);
    gone_offer.host_gone(100);
    CHECK(gone_offer.state(0, 101) == GuestEvent::expired && !gone_offer.busy() && !gone_offer.note());
    CHECK(gone_offer.part(GuestEvent::expired) == Part::none);
    // Gone while joining: the popup stops (join_off), no lobby updates, no note.
    b::Guest gone_join;
    gone_join.offer(1, i, 0);
    gone_join.joining(0);
    gone_join.report("found");
    CHECK(!gone_join.join_off() && gone_join.lobby_due());
    gone_join.host_gone(500);
    CHECK(gone_join.join_off() && !gone_join.lobby_due());
    CHECK(gone_join.report("joinfail:the host's battle is over") == GuestEvent::gave_up && !gone_join.note());
    CHECK(gone_join.part(GuestEvent::gave_up) == Part::none);
    // The host's end also stops the popup.
    b::Guest over;
    over.offer(2, i, 0);
    over.joining(0);
    over.host_over(2, 100);
    CHECK(over.join_off());

    // The friend's join fails: the host hears it failed, and this galaxy shows a note once, until the
    // next announcement.
    b::Guest fail;
    fail.offer(1, i, 0);
    fail.joining(0);
    fail.report("found");
    CHECK(fail.report("joinfail:the host's game refused or lost the join") == GuestEvent::gave_up);
    CHECK(!fail.back && fail.part(GuestEvent::gave_up) == Part::failed && fail.note());
    fail.noted();
    CHECK(!fail.note());
    // Back on the joining popup: the friend chose to stay; no note here.
    b::Guest back;
    back.offer(1, i, 0);
    back.joining(0);
    CHECK(back.report("joinfail:the friend stayed in the galaxy") == GuestEvent::gave_up && back.back);
    CHECK(back.part(GuestEvent::gave_up) == Part::stayed && !back.note());
    // An announcement it could not start joining in time, a refused join and an abandoned one all fail,
    // with a note.
    b::Guest late;
    late.offer(1, i, 0);
    CHECK(late.state(0, b::Guest::offer_limit_ms + 1) == GuestEvent::expired && late.note());
    CHECK(late.part(GuestEvent::expired) == Part::failed);
    // A new battle drops the stale note.
    CHECK(late.offer(2, i, b::Guest::offer_limit_ms + 2) == GuestEvent::offered && !late.note());
    b::Guest refused;
    refused.offer(1, i, 0);
    CHECK(refused.refuse() == GuestEvent::refused && !refused.busy() && refused.note());
    CHECK(refused.part(GuestEvent::refused) == Part::failed && refused.refuse() == GuestEvent::none);
    b::Guest refused_join;
    refused_join.offer(1, i, 0);
    refused_join.joining(0);
    CHECK(refused_join.refuse() == GuestEvent::refused && !refused_join.sealed());
    b::Guest stuck;
    stuck.offer(1, i, 0);
    stuck.joining(0);
    CHECK(stuck.state(0, b::Guest::join_limit_ms + 1) == GuestEvent::abandoned && stuck.note());
    CHECK(stuck.part(GuestEvent::abandoned) == Part::failed);
    // A note waiting when the host goes is dropped: the co-op game is over.
    b::Guest dropped;
    dropped.offer(1, i, 0);
    dropped.refuse();
    CHECK(dropped.note());
    dropped.host_gone(10);
    CHECK(!dropped.note());
    // A later co-op game (the next session) starts afresh.
    CHECK(dropped.renumber(1) && !dropped.gone_at);
    CHECK(dropped.offer(1, i, 20) == GuestEvent::offered && !dropped.lost_at && !dropped.host_quit);

    // The host's notes: none without a paired friend, and none before the battle's network game is
    // closed.
    b::Tracker alone;
    alone.report("hosting:GCT-1:pol1g_con", 0);
    alone.state(b::state_mission, 10);
    alone.state(b::state_shell, 20);
    alone.cleaned();
    CHECK(!alone.with_friend && alone.note() == Note::none);
    b::Tracker t;
    t.report("hosting:GCT-1:pol1g_con", 0, true);
    CHECK(t.with_friend && t.note() == Note::none);
    t.friend_report(1, Part::joined);
    t.state(b::state_mission, 10);
    CHECK(t.state(b::state_shell, 20) == Event::ended && t.note() == Note::none);
    t.cleaned();
    CHECK(t.note() == Note::none);  // they fought it together
    // The friend's game did not join: no word, or failed, or Back.
    t.report("hosting:GCT-2:pol1g_con", 100, true);
    CHECK(t.parts[1] == Part::none);  // a new battle forgets the last
    t.state(b::state_mission, 110);
    t.state(b::state_shell, 120);
    CHECK(t.note() == Note::none);  // its network game is still open
    t.cleaned();
    CHECK(t.note() == Note::friend_failed);
    t.friend_report(1, Part::left);  // about an earlier battle: ignored
    CHECK(t.note() == Note::friend_failed);
    t.friend_report(2, Part::stayed);  // a late report still counts
    CHECK(t.note() == Note::friend_stayed);
    t.noted();
    CHECK(t.note() == Note::none);
    // Joined, then left before the end.
    t.report("hosting:GCT-3:pol1g_con", 200, true);
    t.friend_report(3, Part::joined);
    t.state(b::state_mission, 210);
    t.friend_report(3, Part::left);
    t.state(b::state_shell, 220);
    t.cleaned();
    CHECK(t.note() == Note::friend_left);
    // A fallback to the single-player battle has no note.
    t.report("hosting:GCT-4:pol1g_con", 300, true);
    t.report("fallback:netshell:boom", 310);
    t.friend_report(4, Part::failed);
    CHECK(t.note() == Note::none);
    // Co-op: a friend that quits the battle has left it, with no forfeit.
    t.report("hosting:GCT-5:pol1g_con", 400, true);
    t.state(b::state_mission, 410);
    t.friend_report(5, Part::joined);
    t.friend_report(5, Part::quit);
    CHECK(!t.forfeit_wanted && !t.forfeit_due());
    t.state(b::state_shell, 420);
    t.cleaned();
    CHECK(t.note() == Note::friend_left);
    t.noted();

    // Versus, hosted by the attacker's PC: a client whose player quits mid-battle loses it, and the
    // battle host ends the match for its own side, once.
    b::Tracker v;
    v.report("hosting:GCT-6:pol1g_con", 0, true, true);
    CHECK(v.versus && v.with_friend && !v.forfeit_wanted);
    v.friend_report(1, Part::quit);  // before the mission: no match to end
    CHECK(!v.forfeit_wanted);
    v.state(b::state_mission, 10);
    v.friend_report(1, Part::joined);
    CHECK(!v.forfeit_due());
    v.friend_report(1, Part::quit);
    CHECK(v.forfeit_wanted && v.forfeit_due() && !v.forfeit_due());  // once
    v.state(b::state_shell, 20);
    CHECK(!v.forfeit_due());
    v.cleaned();
    CHECK(v.note() == Note::friend_quit);
    v.noted();
    // A client that left (its game lost the host) has not quit, and a quit after the battle is over
    // changes nothing.
    v.report("hosting:GCT-7:pol1g_con", 100, true, true);
    CHECK(!v.forfeit_wanted && !v.forfeit_done);  // a new battle forgets the last
    v.state(b::state_mission, 110);
    v.friend_report(2, Part::left);
    CHECK(!v.forfeit_wanted);
    v.state(b::state_shell, 120);
    v.friend_report(2, Part::quit);
    CHECK(!v.forfeit_wanted && !v.forfeit_due());
    v.cleaned();
    CHECK(v.note() == Note::friend_quit);  // the latest report wins
    v.noted();
    // This PC's own player quits first (quit_own) and its client, dropped by that quit, reports a quit:
    // no forfeit (the game's own rule makes an attacker who quits lose) and no note.
    v.report("hosting:GCT-8:pol1g_con", 200, true, true);
    CHECK(v.report("quit_own", 205) == Event::none && !v.own_quit);  // no mission yet
    v.state(b::state_mission, 210);
    v.friend_report(3, Part::joined);
    CHECK(v.report("quit_own", 220) == Event::own_quit && v.own_quit);
    CHECK(v.report("quit_own", 221) == Event::none);  // once
    // A second after this PC's own quit, the DLL lets the quit go on, once.
    CHECK(!v.hurry_due(220 + b::Tracker::hurry_after_ms - 1));
    CHECK(v.hurry_due(220 + b::Tracker::hurry_after_ms) && v.hurried);
    CHECK(!v.hurry_due(220 + 2 * b::Tracker::hurry_after_ms));
    v.friend_report(3, Part::quit);
    CHECK(!v.forfeit_wanted && !v.forfeit_due());
    v.state(b::state_shell, 230);
    v.cleaned();
    CHECK(v.note() == Note::none);
    v.noted();
    // A client's quit comes first, then this PC's player quits before the forfeit runs: no forfeit,
    // but the friend did quit, so its note stays.
    v.report("hosting:GCT-9:pol1g_con", 300, true, true);
    CHECK(!v.own_quit && !v.dropped[1] && !v.hurried);  // a new battle forgets it
    v.state(b::state_mission, 310);
    CHECK(!v.hurry_due(310 + b::Tracker::hurry_after_ms));  // nobody quit here
    v.friend_report(4, Part::quit);
    CHECK(v.forfeit_wanted);
    v.report("quit_own", 311);
    CHECK(!v.forfeit_due() && !v.dropped[1]);
    v.state(b::state_shell, 320);
    CHECK(!v.hurry_due(311 + b::Tracker::hurry_after_ms));  // the mission is over
    v.cleaned();
    CHECK(v.note() == Note::friend_quit);
    v.noted();
    // Co-op: the friend's game leaves first, then this PC's player quits: its note stays. One that
    // never joined keeps its note too.
    b::Tracker c;
    c.report("hosting:GCT-10:pol1g_con", 400, true, false);
    c.state(b::state_mission, 410);
    c.friend_report(1, Part::joined);
    c.friend_report(1, Part::left);
    CHECK(c.report("quit_own", 420) == Event::own_quit && !c.dropped[1]);
    c.state(b::state_shell, 430);
    c.cleaned();
    CHECK(c.note() == Note::friend_left);
    c.noted();
    c.report("hosting:GCT-11:pol1g_con", 500, true, false);
    c.state(b::state_mission, 510);
    c.friend_report(2, Part::stayed);
    c.report("quit_own", 520);
    c.state(b::state_shell, 530);
    c.cleaned();
    CHECK(c.note() == Note::friend_stayed);
    c.noted();
    // Co-op, this PC's player quits first and the friend's game leaves after it: no note.
    c.report("hosting:GCT-12:pol1g_con", 600, true, false);
    c.state(b::state_mission, 610);
    c.friend_report(3, Part::joined);
    c.report("quit_own", 620);
    c.friend_report(3, Part::left);
    CHECK(c.dropped[1] && !c.forfeit_wanted);
    c.state(b::state_shell, 630);
    c.cleaned();
    CHECK(c.note() == Note::none);
    // The battle team of a side, for the forfeit (factions: all 1, imp 2).
    CHECK(b::side_team(1, 2, 1) == 1 && b::side_team(1, 2, 2) == 2 && b::side_team(2, 1, 1) == 2);
    CHECK(b::side_team(1, 2, 3) == 0 && b::side_team(1, 2, 0) == 0);
}

// Versus bots: the battle host's own team counts as the player's, so the engine's AI auto-balance
// runs. Co-op, a joined battle and an unknown team keep the network rule: no player's team.
static void bots() {
    CHECK(b::ai_player_team(true, true, 1) == 1 && b::ai_player_team(true, true, 2) == 2);
    CHECK(b::ai_player_team(false, true, 1) == 0);  // co-op: unchanged
    CHECK(b::ai_player_team(true, false, 2) == 0);  // the joining PC runs no AI
    CHECK(b::ai_player_team(true, true, 0) == 0 && b::ai_player_team(true, true, 3) == 0);
    // The allies' offset that gives them the enemy's skill, from the game's tables (allies 6, 11, 11;
    // enemy 4, 14, 17 for Easy to Hard).
    const int ally[4] = {0, 6, 11, 11}, enemy[4] = {0, 4, 14, 17};
    const int even[4] = {0, -2, 3, 6};
    for (int d = 1; d <= 3; ++d) {
        CHECK(b::even_ally_offset(0, enemy[d], ally[d]) == even[d]);
        CHECK(ally[d] + b::even_ally_offset(0, enemy[d], ally[d]) == enemy[d]);
    }
    CHECK(b::even_ally_offset(-1, 14, 11) == 2);  // a mission's own enemy offset is kept
}

// Each session numbers the host's battles afresh: a restarted or another host counts from 1 again.
// The friend's game takes a new session once idle, never mid-battle.
static void sessions() {
    const b::Info i{"GCT-0a1b2c3d", "pol1g_con", "imp", "pol", "all=1111100,imp=1111111"};
    b::Guest g;
    g.state(b::state_shell, 0);
    CHECK(g.renumber(1) && g.session == 1 && g.number == 0);
    CHECK(!g.renumber(1));  // once per session
    // Three battles with the first host's game; a repeat in the session is ignored.
    for (unsigned n = 1; n <= 3; ++n) {
        const uint64_t t = n * 100000;
        CHECK(g.offer(n, i, t) == GuestEvent::offered);
        g.joining(t + 10);
        CHECK(g.state(b::state_mission, t + 20) == GuestEvent::launched);
        CHECK(g.state(b::state_shell, t + 30) == GuestEvent::ended);
        g.returned();
        CHECK(!g.busy() && g.offer(n, i, t + 40) == GuestEvent::none);
    }
    // The next session (that host after a restart, or another host): even a battle numbered as the
    // last one is new.
    CHECK(g.renumber(2) && g.session == 2 && g.number == 0);
    CHECK(g.offer(3, i, 500000) == GuestEvent::offered);
    g.refuse();
    // And the one after: battle 1 again.
    CHECK(g.renumber(3) && g.number == 0);
    CHECK(g.offer(1, i, 600000) == GuestEvent::offered);
    // A new session waits while this game is busy: announced, joining, in the battle, returning or
    // closing. The battle keeps its number, so its E still counts.
    CHECK(!g.renumber(4) && g.session == 3 && g.number == 1);  // announced
    g.joining(600010);
    CHECK(!g.renumber(4));  // joining
    g.state(b::state_mission, 600020);
    CHECK(!g.renumber(4) && g.number == 1);  // in the battle
    g.host_over(1, 600030);
    CHECK(g.over_at == 600030 && g.quit_due(600030 + b::Guest::quit_after_ms));
    g.state(b::state_shell, 602000);
    CHECK(!g.renumber(4) && g.returning_now(b::state_shell));  // returning
    g.returned();
    CHECK(g.offer(1, i, 603000) == GuestEvent::none);  // still session 3: a repeat
    CHECK(g.renumber(4) && g.session == 4 && g.number == 0);
    CHECK(g.offer(1, i, 700000) == GuestEvent::offered);
    g.joining(700010);
    CHECK(g.state(b::state_shell, 700010 + b::Guest::join_limit_ms + 1) == GuestEvent::abandoned);
    CHECK(g.close_due() && !g.renumber(5));  // closing
    g.closed();
    CHECK(g.renumber(5) && g.offer(1, i, 800000) == GuestEvent::offered);
}

// An announcement that waited for the busy game expires quietly once taken if its host has gone:
// no join, no note, no J. That host stays gone until the next session, which starts clean.
static void stale() {
    using Part = b::Part;
    const b::Info i{"GCT-0a1b2c3d", "pol1g_con", "imp", "pol", "all=1111100,imp=1111111"};
    b::Guest g;
    g.state(b::state_shell, 0);
    CHECK(g.renumber(1));
    // Battle 1 is over and the friend's game is returning from it when the host announces battle 2:
    // turned down once, on whichever frame.
    g.offer(1, i, 1000);
    g.joining(1010);
    g.state(b::state_mission, 1020);
    g.host_over(1, 200000);
    CHECK(g.state(b::state_shell, 201000) == GuestEvent::ended && g.phase == b::Guest::Phase::returning);
    CHECK(g.offer(2, i, 202000) == GuestEvent::ignored);
    unsigned turned_down = 0;
    for (uint64_t t = 202016; t < 203000; t += 16)
        if (g.offer(2, i, t) != GuestEvent::none) ++turned_down;
    CHECK(turned_down == 0);
    // The host leaves before this game is back. Once back, battle 2 is taken and expires at once:
    // nothing to join, no note, nothing to tell.
    g.host_gone(203000);
    g.returned();
    CHECK(g.offer(2, i, 204000) == GuestEvent::offered && g.gone_at == 203000);
    CHECK(g.state(b::state_shell, 204000) == GuestEvent::expired && !g.busy() && !g.sealed());
    CHECK(!g.note() && !g.note_due && g.part(GuestEvent::expired) == Part::none);
    g.joining(204016);
    CHECK(g.phase == b::Guest::Phase::idle && !g.join_off() && !g.lobby_due());
    // Gone for the rest of that session: a later announcement of it expires quietly too.
    CHECK(!g.renumber(1) && g.gone_at == 203000);
    CHECK(g.offer(3, i, 205000) == GuestEvent::offered && g.state(0, 205000) == GuestEvent::expired);
    CHECK(!g.note() && g.part(GuestEvent::expired) == Part::none);
    // The next co-op game (a new session) starts clean: the host is there, and the numbers, the one
    // turned down included, count afresh.
    CHECK(g.renumber(2) && !g.gone_at && g.number == 0 && g.ignored == 0);
    CHECK(g.offer(1, i, 300000) == GuestEvent::offered);
    CHECK(g.state(b::state_shell, 300016) == GuestEvent::none && g.busy());
    g.joining(300100);
    CHECK(g.phase == b::Guest::Phase::joining && !g.join_off());
    CHECK(g.offer(2, i, 300200) == GuestEvent::ignored && g.offer(2, i, 300216) == GuestEvent::none);
    // A join that fails has its note, and the host hears it (J).
    CHECK(g.report("joinfail:the host's game refused or lost the join") == GuestEvent::gave_up);
    CHECK(g.note() && g.part(GuestEvent::gave_up) == Part::failed);
    CHECK(g.offer(2, i, 301000) == GuestEvent::offered && !g.note());

    // The host leaves mid-battle and the next session opens before this game is back: the battle is
    // still left, nothing is told, and the new session is taken, its host there, once this game is idle.
    b::Guest mid;
    mid.state(b::state_shell, 0);
    mid.renumber(1);
    mid.offer(4, i, 0);
    mid.joining(10);
    mid.state(b::state_mission, 20);
    mid.host_gone(1000);
    CHECK(!mid.renumber(2) && mid.gone_at == 1000);
    CHECK(mid.quit_due(1000 + b::Guest::quit_after_ms));
    CHECK(mid.state(b::state_shell, 3000) == GuestEvent::ended && mid.part(GuestEvent::ended) == Part::none);
    CHECK(!mid.renumber(2) && mid.gone_at == 1000);  // returning
    mid.returned();
    CHECK(mid.renumber(2) && !mid.gone_at);
    CHECK(mid.offer(1, i, 4000) == GuestEvent::offered && mid.state(b::state_shell, 4016) == GuestEvent::none);
}

// Exit to Windows in a Versus battle loses it: a closing battle host concedes with the other team's
// mask, and its client keeps that result, by its own team, after the host is gone.
static void kept() {
    using Part = b::Part;
    CHECK(b::concede_mask(1) == 4u && b::concede_mask(2) == 2u && b::concede_mask(0) == 0 && b::concede_mask(3) == 0);
    CHECK(b::victory_of(b::concede_mask(1)) == 2 && b::victory_of(b::concede_mask(2)) == 1);
    const b::Info i{"GCT-0a1b2c3d", "pol1g_con", "imp", "pol", "all=1111100,imp=1111111"};
    auto in_battle = [&](b::Guest& g) {
        g.state(b::state_shell, 0);
        g.offer(1, i, 0);
        g.joining(10);
        CHECK(g.state(b::state_mission, 20) == GuestEvent::launched);
    };
    // The host (team 1) closes: V with team 2's win, then B. This PC fights on team 2.
    b::Guest mine;
    in_battle(mine);
    mine.own_team = 2;
    CHECK(mine.kept_winner() == 0);  // nothing yet
    mine.host_result(1, b::concede_mask(1), 30000);
    CHECK(mine.kept_winner() == 0);  // the host is still there
    mine.host_gone(30010);
    CHECK(mine.kept_winner() == 1);
    CHECK(mine.quit_due(30010 + b::Guest::quit_after_ms) && mine.state(b::state_shell, 32000) == GuestEvent::ended);
    CHECK(mine.returning_now(b::state_shell) && mine.kept_winner() == 1 && mine.part(GuestEvent::ended) == Part::none);
    // The other team's win: the host's own, as its game recorded it before closing on its scoreboard.
    b::Guest theirs;
    in_battle(theirs);
    theirs.own_team = 2;
    theirs.host_result(1, 0xFFFFFFFBu, 40000);  // MissionDefeat(2): team 1, the host's, wins
    theirs.host_gone(41000);
    CHECK(theirs.kept_winner() == 2);
    // Nothing to keep: the battle ended on the host (E), no result came, or this PC's team is unknown.
    b::Guest ended;
    in_battle(ended);
    ended.own_team = 2;
    ended.host_result(1, 4u, 50000);
    ended.host_over(1, 50100);
    ended.host_gone(50200);
    CHECK(ended.kept_winner() == 0);
    b::Guest dropped;
    in_battle(dropped);
    dropped.own_team = 2;
    dropped.host_gone(60000);  // a drop: no V
    CHECK(dropped.kept_winner() == 0);
    b::Guest unknown;
    in_battle(unknown);
    unknown.host_result(1, 4u, 70000);
    unknown.host_gone(70100);
    CHECK(unknown.own_team == 0 && unknown.kept_winner() == 0);
    // A result for another battle, or one from before this battle ran, is not this one's.
    b::Guest other;
    other.state(b::state_shell, 0);
    other.offer(2, i, 0);
    other.joining(10);
    other.host_result(2, 4u, 15);  // still joining: not taken
    other.state(b::state_mission, 20);
    other.own_team = 2;
    other.host_result(1, 4u, 25);  // battle 1's
    other.host_gone(30);
    CHECK(other.kept_winner() == 0);
    // The next battle forgets this one's team and result.
    mine.returned();
    CHECK(mine.offer(2, i, 100000) == GuestEvent::offered && mine.own_team == 0 && mine.kept_winner() == 0);
}

// The host quits its battle. The friend's engine reads hostquit and would leave at its own 3 s
// deadline; the DLL's leave runs the battle's Quit at once and lets it go on a quarter second later.
static void quitting() {
    using Part = b::Part;
    const b::Info i{"GCT-0a1b2c3d", "tat2c_con", "cis", "tat", "cis=1100000,rep=1100000"};
    auto in_battle = [&](b::Guest& g, unsigned n, uint64_t at) {
        g.state(b::state_shell, at);
        g.offer(n, i, at);
        g.joining(at + 10);
        CHECK(g.state(b::state_mission, at + 20) == GuestEvent::launched);
    };
    // The leave is due as soon as hostquit is read, and it is the DLL's leave, not the player's quit
    // (the battle script's quit_own is not counted).
    b::Guest hq;
    in_battle(hq, 1, 0);
    CHECK(!hq.quit_due(24830) && !hq.hurry_due(24830));
    CHECK(hq.host_quitting(24830) && hq.quitting_at == 24830 && hq.host_quit);
    CHECK(!hq.host_quitting(24850) && hq.quitting_at == 24830);  // once
    CHECK(hq.quit_due(24830) && hq.quit_sent && hq.quit_at == 24830 && !hq.quit_due(24846));
    CHECK(hq.report("quit_own") == GuestEvent::none && !hq.own_quit);
    // A quarter second later the quit goes on, once.
    CHECK(!hq.hurry_due(24830 + b::Guest::hurry_after_ms - 1) && !hq.hurried);
    CHECK(hq.hurry_due(24830 + b::Guest::hurry_after_ms) && hq.hurried);
    CHECK(!hq.hurry_due(24830 + 2 * b::Guest::hurry_after_ms));
    // Out of the battle by the host's quit: the host is told nothing (no J), as after the host's end.
    CHECK(hq.state(b::state_shell, 26300) == GuestEvent::ended && !hq.left_early() && !hq.quit_early());
    CHECK(hq.part(GuestEvent::ended) == Part::none && hq.returning_now(b::state_shell));
    CHECK(!hq.host_quitting(26310) && !hq.hurry_due(26310));  // the battle is over
    hq.returned();
    // The next battle forgets it.
    CHECK(hq.offer(2, i, 30000) == GuestEvent::offered && !hq.quitting_at && !hq.quit_at && !hq.hurried &&
          !hq.host_quit);
    hq.joining(30010);
    CHECK(!hq.host_quitting(30015));  // not in the battle yet
    CHECK(hq.state(b::state_mission, 30020) == GuestEvent::launched && !hq.quit_due(30030) && !hq.hurry_due(40000));
    // Before the battle (announced or joining) nothing is taken.
    b::Guest early;
    early.offer(1, i, 0);
    CHECK(!early.host_quitting(5) && !early.quitting_at && !early.host_quit);
    early.joining(10);
    CHECK(!early.host_quitting(15) && !early.quit_due(15));
    // This PC's own player quits first: that Quit is under way, the engine sets no hostquit, and it
    // stays the player's quit.
    b::Guest own;
    in_battle(own, 1, 0);
    CHECK(own.report("quit_own") == GuestEvent::own_quit);
    CHECK(!own.host_quitting(1000) && !own.quitting_at && !own.host_quit && !own.quit_due(1000));
    CHECK(!own.hurry_due(1000 + b::Guest::hurry_after_ms));  // not the DLL's leave
    CHECK(own.state(b::state_shell, 2500) == GuestEvent::ended && own.part(GuestEvent::ended) == Part::quit);
    // The DLL's leave runs first (the host's end): nothing more to take, and that leave also goes on
    // a quarter second after it.
    b::Guest over;
    in_battle(over, 1, 0);
    over.host_over(1, 5000);
    CHECK(over.quit_due(5000 + b::Guest::quit_after_ms) && over.quit_at == 5000 + b::Guest::quit_after_ms);
    CHECK(!over.host_quitting(6100) && !over.quitting_at);
    CHECK(!over.hurry_due(6000 + b::Guest::hurry_after_ms - 1) && over.hurry_due(6000 + b::Guest::hurry_after_ms));
    // So do the leaves after the host has gone and after this game's match.
    b::Guest gone;
    in_battle(gone, 1, 0);
    gone.host_gone(7000);
    CHECK(gone.quit_due(7000 + b::Guest::quit_after_ms) && gone.hurry_due(8000 + b::Guest::hurry_after_ms));
    b::Guest board;
    in_battle(board, 1, 0);
    board.match_over(9000);
    CHECK(!board.hurry_due(20000));  // no leave yet
    CHECK(board.quit_due(9000 + b::Guest::quit_after_match_ms) && board.hurry_due(12000 + b::Guest::hurry_after_ms));
    // A battle the engine ended before the DLL's leave could run: no hurry.
    b::Guest fast;
    in_battle(fast, 1, 0);
    CHECK(fast.host_quitting(1000));
    CHECK(fast.state(b::state_shell, 1016) == GuestEvent::ended && !fast.quit_due(1016) && !fast.hurry_due(2000));
    CHECK(!fast.left_early() && fast.part(GuestEvent::ended) == Part::none);
}

// The friend's battle start: the side select's button, immediate spawns, the turret pool and the
// side choice's tries.
static void play() {
    // Faction IDs: all 1, imp 2, rep 3, cis 4. pol1g has the Empire as team 1.
    CHECK(!strcmp(b::side_button(2, 1, 1), "team2"));
    CHECK(!strcmp(b::side_button(2, 1, 2), "team1"));
    CHECK(!strcmp(b::side_button(3, 4, 4), "team2"));
    CHECK(!strcmp(b::side_button(2, 1, 3), "auto"));   // no team of that faction
    CHECK(!strcmp(b::side_button(0, 0, 1), "auto"));   // teams unreadable
    CHECK(!strcmp(b::side_button(2, 1, 0), "auto"));   // no galaxy side stored
    CHECK(!strcmp(b::side_button(1, 1, 1), "team1"));  // both match: the first
    // A human who asked to spawn and waits for a later wave spawns now.
    CHECK(b::spawn_now(7, 8, true));
    CHECK(!b::spawn_now(7, 7, true));   // already due
    CHECK(!b::spawn_now(7, 8, false));  // still choosing a unit
    CHECK(!b::spawn_now(-1, 8, true));  // no wave read
    CHECK(!b::spawn_now(9, 8, true));
    // The friend's turret pool is raised to the command post count, as the host's mission start does;
    // never lowered, never past what a network game sends, and left alone without command posts.
    CHECK(b::turret_limit(0, 5) == 5);
    CHECK(b::turret_limit(3, 5) == 5);
    CHECK(b::turret_limit(8, 5) == 8);  // a mission script's larger pool stays
    CHECK(b::turret_limit(5, 5) == 5);
    CHECK(b::turret_limit(0, 0) == 0);  // command posts not counted yet
    CHECK(b::turret_limit(0, -1) == 0);
    CHECK(b::turret_limit(0, 63) == 63);
    CHECK(b::turret_limit(0, 64) == 0);  // not a count this game can have
    // The side choice: tried up to max_tries, never again once done, and afresh after a reset.
    b::SideChoice side;
    CHECK(side.due());
    for (unsigned n = 0; n < b::SideChoice::max_tries; ++n) ++side.tries;
    CHECK(!side.due());
    side.reset();
    side.done = true;
    CHECK(!side.due());
    side.reset();
    CHECK(side.due() && side.tries == 0);
}

// The targeting controller linked to the player's own character table entry, and the AI census.
static void aim() {
    // The entry of slot 1 is the player's only once its own slot is 1.
    CHECK(b::character_entry_ok(1, 64, 1));
    CHECK(!b::character_entry_ok(1, 64, -1));  // still an AI's or a placeholder
    CHECK(!b::character_entry_ok(1, 64, 0));
    CHECK(!b::character_entry_ok(-1, 64, -1));  // no slot yet
    CHECK(!b::character_entry_ok(64, 64, 64));  // past the table
    CHECK(!b::character_entry_ok(0, 0, 0));     // no table
    // Linked when the controller holds none, or another character; never without a controller or
    // before the entry is the player's.
    const uint32_t controller = 0x1000, entry = 0x2000;
    CHECK(b::aim_relink_due(controller, 0, entry));
    CHECK(b::aim_relink_due(controller, 0x3000, entry));
    CHECK(!b::aim_relink_due(controller, entry, entry));
    CHECK(!b::aim_relink_due(0, 0, entry));
    CHECK(!b::aim_relink_due(controller, 0, 0));
    // What a character controls, in the targeting update's order.
    CHECK(b::seat_of(0, 0, 0) == b::Seat::none);
    CHECK(b::seat_of(1, 0, 0) == b::Seat::soldier);
    CHECK(b::seat_of(1, 1, 0) == b::Seat::vehicle);
    CHECK(b::seat_of(1, 1, 1) == b::Seat::seat);
    CHECK(b::seat_of(0, 0, 1) == b::Seat::seat);
    CHECK(!strcmp(b::seat_name(b::Seat::vehicle), "vehicle") && !strcmp(b::seat_name(b::Seat::none), "none"));
    // The AI census, by what each AI controls.
    b::Army army;
    army.add(false, b::Seat::vehicle);
    army.add(false, b::Seat::vehicle);
    army.add(false, b::Seat::soldier);
    army.add(false, b::Seat::none);
    army.add(false, b::Seat::seat);
    army.add(true, b::Seat::vehicle);  // a human counts as a human only
    CHECK(army.ai == 5 && army.vehicle == 2 && army.soldier == 1 && army.waiting == 1 && army.seat == 1 &&
          army.humans == 1);
}

// Tunnel frames: a header naming the target socket, the source and its port, then the packet.
static void tunnel() {
    const char packet[] = "\x01\x02\x03";
    const std::string f = b::encode_tunnel(b::To::listener, b::From::broadcast, 3659, packet, 3);
    b::To to = b::To::game;
    b::From from = b::From::game;
    uint16_t port = 0;
    CHECK(f.size() == b::tunnel_header + 3 && b::decode_tunnel(f.data(), f.size(), to, from, port));
    CHECK(to == b::To::listener && from == b::From::broadcast && port == 3659 &&
          !memcmp(f.data() + b::tunnel_header, packet, 3));
    CHECK(!b::decode_tunnel(f.data(), b::tunnel_header, to, from, port));  // no packet
    std::string wrong = f;
    wrong[0] = 'X';
    CHECK(!b::decode_tunnel(wrong.data(), wrong.size(), to, from, port));
    wrong = f;
    wrong[1] = 'Q';
    CHECK(!b::decode_tunnel(wrong.data(), wrong.size(), to, from, port));
    const std::string big(b::tunnel_header + b::tunnel_max_packet + 1, 'T');
    CHECK(!b::decode_tunnel(big.data(), big.size(), to, from, port));
    // Destination ports: 3656 reaches the listener, any other the game socket.
    CHECK(b::route(3656) == b::To::listener && b::route(3658) == b::To::game && b::route(3657) == b::To::game);

    // The mirror: the peer's game packets come from wherever this game sent.
    b::Mirror m;
    b::Address a = m.source(b::From::game, 0, b::host_address, b::host_game_port);
    CHECK(a.ip == b::host_address && a.port == b::host_game_port);
    a = m.source(b::From::game, 3660, b::host_address, b::host_game_port);
    CHECK(a.port == 3660);
    a = m.source(b::From::broadcast, 3660, b::host_address, b::host_game_port);
    CHECK(a.ip == b::host_address && a.port == b::broadcast_port);
    m.sent(b::host_address, b::broadcast_port, b::host_address);  // a reply to a broadcast: not the game socket
    m.sent(0xFFFFFFFF, b::listener_port, b::host_address);        // a discovery query
    CHECK(!m.set);
    m.sent(0xC0A80105, 3658, b::host_address);  // the host's address as this game read it
    a = m.source(b::From::game, 0, b::host_address, b::host_game_port);
    CHECK(m.set && a.ip == 0xC0A80105 && a.port == 3658);
    a = m.source(b::From::broadcast, 0, b::host_address, b::host_game_port);
    CHECK(a.ip == b::host_address && a.port == b::broadcast_port);
    m.clear();
    CHECK(!m.set);
}

// Battles with up to three friends: client addresses and routing, the character table's letters,
// and the host's notes per friend.
static void parties() {
    using Note = b::Note;
    using Part = b::Part;
    // Each client at its session slot's address; nothing else is a client.
    CHECK(b::client_address(1) == b::friend_address && b::client_address(3) == 0x0A470004u);
    CHECK(!b::client_address(0) && !b::client_address(4));
    CHECK(b::client_slot(0x0A470002) == 1 && b::client_slot(0x0A470004) == 3 && !b::client_slot(b::host_address));
    CHECK(!b::client_slot(0x0A470005) && !b::client_slot(0xC0A80105));
    // Where the battle host's packets go: the client at that address, or every client for a broadcast.
    // A stray address reaches the only client, and nobody when there are several.
    bool stray = false;
    const unsigned one_and_three = 0x2 | 0x8;
    CHECK(b::destination_slot(0x0A470002, one_and_three, stray) == 1 && !stray);
    CHECK(b::destination_slot(0x0A470004, one_and_three, stray) == 3 && !stray);
    CHECK(b::destination_slot(b::any_address, one_and_three, stray) == b::all_peers && !stray);
    CHECK(b::destination_slot(0x0A470003, one_and_three, stray) == b::no_peer && stray);  // slot 2 is not in it
    CHECK(b::destination_slot(0xC0A80105, one_and_three, stray) == b::no_peer && stray);
    CHECK(b::destination_slot(0xC0A80105, 0x4, stray) == 2 && stray);
    CHECK(b::destination_slot(0x0A470002, 0x4, stray) == 2 && stray);  // slot 1's address, but one client
    CHECK(b::destination_slot(0x0A470003, 0x4, stray) == 2 && !stray);
    CHECK(b::destination_slot(0x0A470002, 0, stray) == b::no_peer && stray);  // no client at all

    // The character table's letters: a player's own slot is its index, an AI's -1.
    b::Layout layout;
    layout.add(0, 0, 1);
    layout.add(1, -1, 2);
    layout.add(2, 2, 2);
    layout.add(3, -1, 1);
    layout.add(4, -1, 0);
    layout.add(5, 7, 1);  // another player's slot: not counted
    CHECK(layout.letters == "AbBa.." && layout.humans == 2 && layout.team1 == 2 && layout.team2 == 2);

    // The host's notes with several friends, told of the battle by slot.
    b::Tracker t;
    t.report("hosting:GCT-1:pol1g_con", 0, true);
    t.friends_told(1, 0x2 | 0x4 | 0x8);
    t.friend_report(1, 1, Part::joined);
    t.friend_report(3, 1, Part::joined);
    t.friend_report(4, 1, Part::left);  // no slot 4
    t.state(b::state_mission, 10);
    t.friend_report(3, 1, Part::left);  // slot 3 leaves before the end
    t.state(b::state_shell, 20);
    t.cleaned();
    CHECK(t.note() == Note::friends);
    CHECK(t.friend_note(1) == Note::none && t.friend_note(2) == Note::friend_failed &&
          t.friend_note(3) == Note::friend_left);
    CHECK(t.friend_note(0) == Note::none && t.friend_note(4) == Note::none);
    t.noted();
    CHECK(t.note() == Note::none && t.friend_note(2) == Note::none);
    // Everyone fought it: no note.
    t.report("hosting:GCT-2:pol1g_con", 100, true);
    t.friends_told(2, 0x2 | 0x4);
    t.friends_told(1, 0x8);  // about another battle: ignored
    t.friend_report(1, 2, Part::joined);
    t.friend_report(2, 2, Part::joined);
    t.state(b::state_mission, 110);
    t.state(b::state_shell, 120);
    t.cleaned();
    CHECK(t.note_slots() == 0x6 && t.note() == Note::none);
    // One friend told: the one-friend notes, from its own slot.
    t.report("hosting:GCT-3:pol1g_con", 200, true);
    t.friends_told(3, 0x4);
    t.friend_report(2, 3, Part::stayed);
    t.state(b::state_mission, 210);
    t.state(b::state_shell, 220);
    t.cleaned();
    CHECK(t.note() == Note::friend_stayed && t.friend_note(2) == Note::none);
    // Not told yet (0): slot 1 alone.
    t.report("hosting:GCT-4:pol1g_con", 300, true);
    CHECK(t.told == 0 && t.note_slots() == 0x2);
    t.friend_report(1, 4, Part::joined);
    t.state(b::state_mission, 310);
    t.state(b::state_shell, 320);
    t.cleaned();
    CHECK(t.note() == Note::none);
    // This PC's own quit drops the leaves that follow it, friend by friend.
    t.report("hosting:GCT-5:pol1g_con", 400, true);
    t.friends_told(5, 0x2 | 0x4);
    t.friend_report(1, 5, Part::joined);
    t.friend_report(2, 5, Part::joined);
    t.state(b::state_mission, 410);
    t.friend_report(1, 5, Part::left);  // before the host's own quit: it counts
    CHECK(t.report("quit_own", 420) == Event::own_quit);
    t.friend_report(2, 5, Part::left);  // after it: dropped
    t.state(b::state_shell, 430);
    t.cleaned();
    CHECK(t.note() == Note::friends && t.friend_note(1) == Note::friend_left && t.friend_note(2) == Note::none);
    CHECK(b::parse_note("friends") == Note::friends && !strcmp(b::note_name(Note::friends), "friends"));
}

// Which of the game's three LAN sockets a handle is.
static void sockets() {
    CHECK(b::classify(5, 5, 6, 7) == b::Socket::game);
    CHECK(b::classify(6, 5, 6, 7) == b::Socket::broadcast);
    CHECK(b::classify(7, 5, 6, 7) == b::Socket::listener);
    CHECK(b::classify(8, 5, 6, 7) == b::Socket::other);
    // Before the game makes its sockets every handle is 0.
    CHECK(b::classify(0, 0, 0, 0) == b::Socket::other);
    CHECK(b::classify(~uintptr_t(0), ~uintptr_t(0), 0, 0) == b::Socket::other);
}

// Stand-in Winsock: records sends; recvfrom hands out queued packets, then fails with
// WSAEWOULDBLOCK as a non-blocking socket does.
struct Fake {
    std::vector<std::pair<SOCKET, std::string>> sent;
    std::vector<std::pair<SOCKET, std::string>> incoming;
} fake;
static int WSAAPI fake_sendto(SOCKET s, const char* d, int n, int, const sockaddr*, int) {
    fake.sent.emplace_back(s, std::string(d, static_cast<size_t>(n)));
    return n;
}
static int WSAAPI fake_send(SOCKET s, const char* d, int n, int) {
    fake.sent.emplace_back(s, std::string(d, static_cast<size_t>(n)));
    return n;
}
static int WSAAPI fake_recvfrom(SOCKET s, char* d, int n, int, sockaddr* from, int* from_size) {
    for (size_t i = 0; i < fake.incoming.size(); ++i) {
        if (fake.incoming[i].first != s) continue;
        const std::string p = fake.incoming[i].second;
        fake.incoming.erase(fake.incoming.begin() + static_cast<long>(i));
        const int copy = static_cast<int>(p.size()) < n ? static_cast<int>(p.size()) : n;
        memcpy(d, p.data(), static_cast<size_t>(copy));
        if (from && from_size && *from_size >= static_cast<int>(sizeof(sockaddr_in))) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            memcpy(from, &a, sizeof a);
            *from_size = sizeof a;
        }
        return copy;
    }
    SetLastError(WSAEWOULDBLOCK);
    return SOCKET_ERROR;
}

// A sockaddr_in from a host-order address and port.
static sockaddr_in address(uint32_t ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = _byteswap_ushort(port);
    a.sin_addr.s_addr = _byteswap_ulong(ip);
    return a;
}

// The socket wrappers: straight through when unsealed; when sealed, the game's sockets reach only
// the tunnel.
static void wrappers() {
    DWORD globals[3] = {0, 0, 0};  // the game has not made its sockets yet
    gc::SendTo w_sendto = nullptr;
    gc::Send w_send = nullptr;
    gc::RecvFrom w_recvfrom = nullptr;
    gc::battle_net_test_setup(globals, &fake_sendto, &fake_send, &fake_recvfrom, w_sendto, w_send, w_recvfrom);
    char buffer[64];
    sockaddr_in from{};
    int from_size = sizeof from;
    const sockaddr_in to = address(0xC0A80105, 3658);

    // Unsealed: everything passes straight through.
    CHECK(w_sendto(11, "abc", 3, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 3 && fake.sent.size() == 1);
    globals[0] = 11;
    globals[1] = 12;
    globals[2] = 13;
    CHECK(w_send(12, "ping", 4, 0) == 4 && fake.sent.size() == 2);
    fake.incoming.emplace_back(11, "real");
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 4);
    CHECK(!memcmp(buffer, "real", 4));
    CHECK(!gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "x", 1).data(), b::tunnel_header + 1));

    // Sealed without a peer (the one-PC test): nothing is sent anywhere, but each call looks sent.
    gc::battle_tunnel_peer(0);
    gc::battle_seal(true);
    CHECK(gc::battle_sealed());
    fake.sent.clear();
    CHECK(w_sendto(11, "state", 5, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 5);
    CHECK(w_send(12, "announce", 8, 0) == 8);
    std::string frame;
    CHECK(fake.sent.empty() && !gc::battle_tunnel_take(frame));
    gc::BattleSealCounts counts = gc::battle_seal_counts();
    CHECK(counts.sent == 2 && counts.dropped == 2);
    // Another socket of the process still passes.
    CHECK(w_sendto(99, "other", 5, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 5 && fake.sent.size() == 1);
    // What really arrives is read and dropped; the game sees nothing.
    fake.incoming.emplace_back(11, "from a stranger");
    fake.incoming.emplace_back(13, "LAN browser query");
    fake.incoming.emplace_back(99, "for another socket");
    from_size = sizeof from;
    SetLastError(0);
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR);
    CHECK(GetLastError() == WSAEWOULDBLOCK);
    CHECK(w_recvfrom(13, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR);
    CHECK(fake.incoming.size() == 1 && fake.incoming[0].first == 99);
    from_size = sizeof from;
    CHECK(w_recvfrom(99, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 18);
    counts = gc::battle_seal_counts();
    CHECK(counts.discarded == 2);

    // The friend's PC (the peer is the host): what its game sends becomes tunnel frames for the Steam
    // thread.
    gc::battle_tunnel_peer(2);
    fake.sent.clear();
    CHECK(w_send(12, "query", 5, 0) == 5);  // discovery broadcast: the host's listener
    const sockaddr_in to_host = address(b::host_address, 3658);
    CHECK(w_sendto(11, "join", 4, 0, reinterpret_cast<const sockaddr*>(&to_host), sizeof to_host) == 4);
    const sockaddr_in to_listener = address(0xFFFFFFFF, 3656);
    CHECK(w_sendto(11, "probe", 5, 0, reinterpret_cast<const sockaddr*>(&to_listener), sizeof to_listener) == 5);
    CHECK(fake.sent.empty());
    b::To t = b::To::game;
    b::From f = b::From::game;
    uint16_t port = 0;
    CHECK(gc::battle_tunnel_take(frame) && b::decode_tunnel(frame.data(), frame.size(), t, f, port));
    CHECK(t == b::To::listener && f == b::From::broadcast && frame.substr(b::tunnel_header) == "query");
    CHECK(gc::battle_tunnel_take(frame) && b::decode_tunnel(frame.data(), frame.size(), t, f, port));
    CHECK(t == b::To::game && f == b::From::game && frame.substr(b::tunnel_header) == "join");
    CHECK(gc::battle_tunnel_take(frame) && b::decode_tunnel(frame.data(), frame.size(), t, f, port));
    CHECK(t == b::To::listener && f == b::From::game && frame.substr(b::tunnel_header) == "probe");
    CHECK(!gc::battle_tunnel_take(frame));

    // Frames from the host reach the socket they name, as from the host's address. Its game packets
    // mirror where this game sent ("join" above).
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::listener, b::From::broadcast, 0, "info", 4).data(),
                                b::tunnel_header + 4));
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "welcome", 7).data(),
                                b::tunnel_header + 7));
    from_size = sizeof from;
    // The broadcast socket only sends.
    CHECK(w_recvfrom(12, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR);
    from_size = sizeof from;
    CHECK(w_recvfrom(13, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 4 &&
          !memcmp(buffer, "info", 4));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == b::host_address &&
          _byteswap_ushort(from.sin_port) == b::broadcast_port);
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 7 &&
          !memcmp(buffer, "welcome", 7));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == b::host_address && _byteswap_ushort(from.sin_port) == 3658);
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR);
    // A packet larger than the game's buffer is cut, as UDP does.
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "0123456789", 10).data(),
                                b::tunnel_header + 10));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, 4, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR &&
          GetLastError() == WSAEMSGSIZE);
    CHECK(!gc::battle_tunnel_put("junk", 4));
    counts = gc::battle_seal_counts();
    CHECK(counts.delivered == 3);

    // The host's PC (the peer is the friend): until the host's game has sent to the friend, its
    // packets come from the friend's address and the client's first port.
    gc::battle_seal(false);
    gc::battle_seal(true);
    gc::battle_tunnel_peer(1);
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "hello", 5).data(),
                                b::tunnel_header + 5));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 5);
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == b::friend_address &&
          _byteswap_ushort(from.sin_port) == b::client_game_port);

    // Versus: the battle role, not the session's, gives the peer's address. The session host joins the
    // friend's battle (2: the peer hosts it), and the friend's PC hosts one (1: the peer is its client).
    CHECK(gc::battle_tunnel_peer_role() == 1);
    gc::battle_seal(false);
    gc::battle_seal(true);
    gc::battle_tunnel_role(2);
    CHECK(gc::battle_tunnel_peer_role() == 2);
    CHECK(
        gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "host", 4).data(), b::tunnel_header + 4));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 4);
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == b::host_address &&
          _byteswap_ushort(from.sin_port) == b::host_game_port);
    gc::battle_seal(false);
    gc::battle_seal(true);
    gc::battle_tunnel_peer(2);  // the friend's PC
    gc::battle_tunnel_role(1);
    CHECK(gc::battle_tunnel_peer_role() == 1);
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "client", 6).data(),
                                b::tunnel_header + 6));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 6);
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == b::friend_address &&
          _byteswap_ushort(from.sin_port) == b::client_game_port);
    gc::battle_tunnel_peer(0);  // no paired peer: no address, whatever the battle role
    CHECK(gc::battle_tunnel_peer_role() == 0);
    gc::battle_tunnel_peer(1);
    gc::battle_tunnel_role(0);

    // A co-op host's battle with friends in slots 1 and 3: each shows at its slot's address and is
    // mirrored on its own; packets go to the friend at that address, broadcasts to both, strays nowhere.
    gc::battle_seal(false);
    gc::battle_seal(true);
    gc::battle_tunnel_peers(0x2 | 0x8);
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::listener, b::From::broadcast, 0, "q1", 2).data(),
                                b::tunnel_header + 2, 1));
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::listener, b::From::broadcast, 0, "q3", 2).data(),
                                b::tunnel_header + 2, 3));
    CHECK(!gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "q0", 2).data(), b::tunnel_header + 2,
                                 0));
    CHECK(!gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "q4", 2).data(), b::tunnel_header + 2,
                                 4));
    CHECK(gc::battle_tunnel_waiting() == 2 && gc::battle_tunnel_waiting(1) == 1 && gc::battle_tunnel_waiting(3) == 1 &&
          !gc::battle_tunnel_waiting(2));
    from_size = sizeof from;
    CHECK(w_recvfrom(13, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 2 &&
          !memcmp(buffer, "q1", 2));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == 0x0A470002u && _byteswap_ushort(from.sin_port) == b::broadcast_port);
    from_size = sizeof from;
    CHECK(w_recvfrom(13, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 2 &&
          !memcmp(buffer, "q3", 2));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == 0x0A470004u && _byteswap_ushort(from.sin_port) == b::broadcast_port);
    const unsigned stray_before = gc::battle_seal_counts().stray;
    const sockaddr_in to_one = address(0x0A470002, 3659), to_three = address(0x0A470004, 3661);
    const sockaddr_in to_two = address(0x0A470003, 3659), to_lan = address(0xC0A80105, 3659);
    CHECK(w_sendto(11, "for 1", 5, 0, reinterpret_cast<const sockaddr*>(&to_one), sizeof to_one) == 5);
    CHECK(w_sendto(11, "for 3", 5, 0, reinterpret_cast<const sockaddr*>(&to_three), sizeof to_three) == 5);
    CHECK(w_send(12, "info", 4, 0) == 4);
    CHECK(w_sendto(11, "for 2", 5, 0, reinterpret_cast<const sockaddr*>(&to_two), sizeof to_two) == 5);  // not in it
    CHECK(w_sendto(11, "lan", 3, 0, reinterpret_cast<const sockaddr*>(&to_lan), sizeof to_lan) == 3);
    unsigned dest = 9;
    CHECK(gc::battle_tunnel_take(frame, &dest) && dest == 1 && frame.substr(b::tunnel_header) == "for 1");
    CHECK(gc::battle_tunnel_take(frame, &dest) && dest == 3 && frame.substr(b::tunnel_header) == "for 3");
    CHECK(gc::battle_tunnel_take(frame, &dest) && dest == b::all_peers && frame.substr(b::tunnel_header) == "info");
    CHECK(!gc::battle_tunnel_take(frame, &dest));
    CHECK(gc::battle_seal_counts().stray == stray_before + 2);
    // Each friend's game packets come from where this game sent to it.
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "s1", 2).data(), b::tunnel_header + 2,
                                1));
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "s3", 2).data(), b::tunnel_header + 2,
                                3));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 2 &&
          !memcmp(buffer, "s1", 2));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == 0x0A470002u && _byteswap_ushort(from.sin_port) == 3659);
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 2 &&
          !memcmp(buffer, "s3", 2));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == 0x0A470004u && _byteswap_ushort(from.sin_port) == 3661);
    // With one friend (slot 2), a stray address still reaches it and is mirrored.
    gc::battle_tunnel_peers(0x4);
    CHECK(w_sendto(11, "lan", 3, 0, reinterpret_cast<const sockaddr*>(&to_lan), sizeof to_lan) == 3);
    CHECK(gc::battle_tunnel_take(frame, &dest) && dest == 2 && frame.substr(b::tunnel_header) == "lan");
    CHECK(gc::battle_seal_counts().stray == stray_before + 3);
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "s2", 2).data(), b::tunnel_header + 2,
                                2));
    from_size = sizeof from;
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 2 &&
          !memcmp(buffer, "s2", 2));
    CHECK(_byteswap_ulong(from.sin_addr.s_addr) == 0xC0A80105u && _byteswap_ushort(from.sin_port) == 3659);
    // A friend's PC: its game's packets go to its one peer, the battle host; one to another address is
    // counted as stray.
    gc::battle_seal(false);
    gc::battle_seal(true);
    gc::battle_tunnel_peer(2);
    gc::battle_tunnel_peers(0x2);
    CHECK(w_sendto(11, "odd", 3, 0, reinterpret_cast<const sockaddr*>(&to_one), sizeof to_one) == 3);
    CHECK(gc::battle_tunnel_take(frame, &dest) && dest == 1 && frame.substr(b::tunnel_header) == "odd");
    CHECK(gc::battle_seal_counts().stray == stray_before + 4);
    gc::battle_tunnel_peer(1);
    gc::battle_seal(false);
    gc::battle_seal(true);

    // Unsealed again: queued frames are dropped and the same sockets work as before.
    CHECK(gc::battle_tunnel_put(b::encode_tunnel(b::To::game, b::From::game, 0, "stale", 5).data(),
                                b::tunnel_header + 5));
    CHECK(w_send(12, "late", 4, 0) == 4);
    gc::battle_seal(false);
    CHECK(!gc::battle_tunnel_take(frame));
    fake.sent.clear();
    CHECK(w_sendto(11, "after", 5, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 5 && fake.sent.size() == 1);
    CHECK(w_recvfrom(11, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_size) == SOCKET_ERROR);
}

int main() {
    modes();
    names();
    info();
    bonuses();
    tracker();
    guest();
    edges();
    bots();
    sessions();
    stale();
    kept();
    quitting();
    play();
    aim();
    tunnel();
    parties();
    sockets();
    wrappers();
    if (failures) {
        std::printf("battle_test FAILED %d\n", failures);
        return 1;
    }
    std::printf("battle_test PASS: modes, announcements, bonus cards, trackers, leaves, tunnels and socket wrappers\n");
    return 0;
}
