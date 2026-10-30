// Tests src/session_logic.h and src/versus_logic.h: the session frame codec, revision and resend
// rules, the ready check, Versus turns, sides and cards, and who offers to save.
#include "../src/session_logic.h"
#include "../src/versus_logic.h"
#include <cstdio>

using namespace gc::session;
#include "check.h"

// A digest whose bytes count up from `seed`.
static Digest digest(uint8_t seed) {
    Digest d{};
    for (size_t i = 0; i < d.size(); ++i) d[i] = static_cast<uint8_t>(seed + i);
    return d;
}

// Encodes a frame and decodes it back.
static bool round_trip(const Frame& f, Frame& out) {
    const std::string bytes = encode(f);
    return decode(bytes.data(), bytes.size(), out);
}

int main() {
    // Codec: every frame type survives a round trip with its fields.
    Frame f, g;
    f.type = Type::hello;
    f.session = 0xA1B2C3D4;
    CHECK(round_trip(f, g) && g.type == Type::hello && g.session == 0xA1B2C3D4 && g.revision == 0);
    f = Frame{};
    f.type = Type::ready;
    f.session = 7;
    f.revision = 12;
    CHECK(round_trip(f, g) && g.type == Type::ready && g.revision == 12);
    f = Frame{};
    f.type = Type::snapshot;
    f.session = 7;
    f.revision = 3;
    f.meta = Meta{6, 2, 5, 107, 660, 2};
    f.digest = digest(9);
    f.json = "{\"maps\":{}}";
    CHECK(round_trip(f, g) && g.revision == 3 && g.meta.producer == 6 && g.meta.generation == 2 &&
          g.meta.sequence == 5 && g.meta.source_table == 107 && g.meta.update_ticket == 660 &&
          g.meta.unchanged_samples == 2 && g.digest == digest(9) && g.json == f.json);
    f.meta.unchanged_samples = -1;
    CHECK(round_trip(f, g) && g.meta.unchanged_samples == -1);
    f = Frame{};
    f.type = Type::ack;
    f.session = 7;
    f.revision = 3;
    f.ack = Ack::stale;
    f.digest = digest(1);
    CHECK(round_trip(f, g) && g.ack == Ack::stale && g.digest == digest(1));
    f = Frame{};
    f.type = Type::loaded;
    f.session = 7;
    f.revision = 5;
    f.shows = Shows::applied;
    CHECK(round_trip(f, g) && g.type == Type::loaded && g.revision == 5 && g.shows == Shows::applied);
    f.revision = 0;
    f.shows = Shows::stopped;  // stopped before anything is applied
    CHECK(round_trip(f, g) && g.shows == Shows::stopped && g.revision == 0);

    // Codec: malformed frames are refused.
    auto refused = [&](std::string bytes) {
        Frame out;
        return !decode(bytes.data(), bytes.size(), out);
    };
    f = Frame{};
    f.type = Type::hello;
    f.session = 7;
    std::string hello = encode(f);
    CHECK(refused(hello.substr(0, header)));  // missing version
    CHECK(refused(hello + "x"));              // trailing bytes
    std::string wrong_version = hello;
    wrong_version.back() = 2;
    CHECK(refused(wrong_version));
    std::string with_revision = hello;
    with_revision[5] = 1;
    CHECK(refused(with_revision));  // hello carries no revision
    std::string no_session = hello;
    no_session[1] = no_session[2] = no_session[3] = no_session[4] = 0;
    CHECK(refused(no_session));
    std::string unknown = hello;
    unknown[0] = 'X';
    CHECK(refused(unknown));
    CHECK(refused(""));
    f = Frame{};
    f.type = Type::snapshot;
    f.session = 7;
    f.revision = 1;
    f.meta.producer = 6;
    f.json = "{}";
    const std::string snapshot = encode(f);
    CHECK(!refused(snapshot));
    CHECK(refused(snapshot.substr(0, snapshot.size() - 2)));  // no JSON
    f.revision = 0;
    CHECK(refused(encode(f)));  // revisions start at 1
    f.revision = 1;
    f.meta.producer = 8;
    CHECK(refused(encode(f)));
    f.meta.producer = 6;
    f.json.assign(max_json + 1, ' ');
    CHECK(refused(encode(f)));
    f = Frame{};
    f.type = Type::ack;
    f.session = 7;
    f.revision = 1;
    std::string ack = encode(f);
    ack[header] = 5;
    CHECK(refused(ack));  // unknown status
    ack[header] = 0;
    CHECK(refused(ack));
    f = Frame{};
    f.type = Type::loaded;
    f.session = 7;
    f.revision = 2;
    std::string shows = encode(f);
    CHECK(!refused(shows));
    CHECK(refused(shows + "x") && refused(shows.substr(0, header)));
    shows[header] = 3;
    CHECK(refused(shows));  // unknown state
    f.revision = 0;
    CHECK(refused(encode(f)));  // applied names a revision

    // Friend: revisions only move forward within one session.
    Receiver in;
    CHECK(in.classify(7, 1, digest(1)) == Receiver::Verdict::other_session);  // nothing opened yet
    CHECK(in.open(7) && !in.open(7));
    CHECK(in.classify(7, 1, digest(1)) == Receiver::Verdict::accept);
    in.commit(1, digest(1));
    CHECK(in.classify(7, 3, digest(3)) == Receiver::Verdict::accept);  // gaps are fine: the newest wins
    in.commit(3, digest(3));
    CHECK(in.classify(7, 3, digest(3)) == Receiver::Verdict::duplicate);
    CHECK(in.classify(7, 3, digest(4)) == Receiver::Verdict::conflict);
    CHECK(in.classify(7, 2, digest(2)) == Receiver::Verdict::stale);
    CHECK(in.classify(8, 9, digest(9)) == Receiver::Verdict::other_session);
    CHECK(in.open(8) && in.revision == 0);  // a new session starts over
    CHECK(in.classify(8, 1, digest(1)) == Receiver::Verdict::accept);
    CHECK(in.classify(7, 4, digest(4)) == Receiver::Verdict::other_session);

    // Host: send the newest unconfirmed revision, resend when overdue, stop when confirmed.
    Sender out;
    out.latest = 2;
    out.open(5);
    CHECK(out.session == 5 && out.latest == 2 && !out.due(0));  // not until the friend's game is ready
    out.on_ready(0);
    CHECK(out.due(0));
    out.on_sent(1000);
    CHECK(!out.due(1000) && !out.due(1000 + resend_interval - 1) && out.due(1000 + resend_interval));
    out.latest = 3;  // a newer snapshot goes at once
    CHECK(out.due(1001));
    out.on_sent(1001);
    CHECK(out.on_ack(2, Ack::accepted) && out.acked == 2 && out.due(1001 + resend_interval));
    CHECK(out.on_ack(3, Ack::duplicate) && out.acked == 3 && !out.due(999999));
    CHECK(out.on_ack(1, Ack::accepted) && out.acked == 3);                  // acknowledgements never go back
    CHECK(!out.on_ack(4, Ack::accepted) && !out.on_ack(0, Ack::accepted));  // never sent in this session
    CHECK(out.on_ack(2, Ack::stale) && out.acked == 3);
    out.latest = 4;
    out.on_sent(2000);
    CHECK(out.on_ack(4, Ack::rejected) && out.refused == 4 && !out.due(999999));  // refused: no resend loop
    out.latest = 5;
    CHECK(out.due(2001));  // the next revision is tried
    // The friend reconnects holding revision 3: everything newer goes again.
    out.open(6);
    CHECK(out.latest == 5 && out.acked == 0 && !out.ready);
    out.on_ready(3);
    CHECK(out.acked == 3 && out.sent == 3 && out.due(0));
    Sender fresh;
    fresh.latest = 2;
    fresh.open(9);
    fresh.on_ready(7);  // a friend claiming more than exists
    CHECK(fresh.acked == 0 && fresh.due(0));

    // Ready check: a new session holds the host's galaxy until the friend's game reports it, the
    // limit counting from when the popup appears.
    Hold hold;
    CHECK(!hold.wanted(0) && !hold.ready(0));
    CHECK(hold.ready(5) && hold.wanted(0) && !hold.ready(5));  // R repeats as the keepalive answer
    CHECK(hold.wanted(10 * hold_limit));                       // still choosing a campaign: no limit yet
    hold.shown(1000);
    CHECK(hold.state == Hold::State::shown && hold.wanted(1000 + hold_limit - 1));
    CHECK(!hold.on_loaded(4, Shows::applied) && hold.wanted(1001));  // another session's report
    CHECK(hold.on_loaded(5, Shows::applied) && !hold.wanted(1002) && hold.state == Hold::State::done);
    hold.shown(2000);
    CHECK(!hold.wanted(2000) && !hold.release("again"));  // done stays done for this session
    CHECK(hold.ready(6) && hold.wanted(0));               // the friend's game rejoined
    hold.shown(100);
    CHECK(!hold.wanted(100 + hold_limit) && std::string(hold.reason) == "the friend's game took too long");
    CHECK(hold.ready(7) && hold.on_loaded(7, Shows::stopped) && !hold.wanted(0) &&
          std::string(hold.reason) == "the friend's game stopped applying");
    CHECK(hold.ready(8) && hold.release("the host cancelled the wait") && !hold.wanted(0));
    // Another co-op game with the friend already paired holds again.
    CHECK(!hold.ready(8) && hold.again(8) && hold.wanted(0) && hold.session == 8);
    CHECK(hold.on_loaded(8, Shows::applied) && !hold.wanted(0));
    CHECK(!hold.again(0));
    {  // Start holds for every friend ready at that moment, each until its game
        // shows the galaxy, stops applying or leaves.
        Hold party;
        const uint32_t none[] = {0, 0};
        CHECK(!party.start(none, 2) && !party.wanted(0));  // nobody ready
        const uint32_t three[] = {11, 0, 12, 13, 14};      // one slot empty; three at most
        CHECK(party.start(three, 5) && party.wanted(0) && party.session == 11 && party.friends == 3 &&
              party.loading() == 3);
        CHECK(!party.on_loaded(14, Shows::applied) && party.loading() == 3);  // not one it waits for
        CHECK(!party.on_loaded(12, Shows::applied) && party.loading() == 2 && party.wanted(0));
        CHECK(!party.on_loaded(12, Shows::applied) && party.loading() == 2);  // a repeat
        CHECK(!party.on_left(11) && party.loading() == 1 && party.wanted(0));
        party.shown(500);
        CHECK(party.on_loaded(13, Shows::applied) && !party.wanted(501) &&
              std::string(party.reason) == "the last friend's game shows the galaxy");
        CHECK(party.start(three, 2) && party.friends == 1 && party.on_left(11) &&
              std::string(party.reason) == "the friend left");  // the zero is skipped: one friend
        CHECK(party.start(three, 3) && !party.on_loaded(11, Shows::stopped) && party.on_left(12) &&
              std::string(party.reason) == "the last friend still loading left");
        CHECK(party.start(three, 3) && !party.on_left(12) && party.on_loaded(11, Shows::stopped) &&
              std::string(party.reason) == "the last friend's game stopped applying");
        CHECK(party.start(three, 3));
        party.shown(0);
        CHECK(!party.wanted(hold_limit) && std::string(party.reason) == "the friends' games took too long");
        CHECK(!party.on_loaded(11, Shows::applied) && !party.on_left(12));  // released: nothing more ends it
    }
    {  // A sender per friend, each under its own session, offered the same newest
        // revision. A friend that joins mid-game gets the newest alone, never older ones.
        Sender ben, cai;
        ben.open(21);
        ben.on_ready(0);
        for (uint32_t r = 1; r <= 3; ++r) {
            ben.latest = cai.latest = r;
            CHECK(ben.due(r) && !cai.due(r));  // no session with Cai yet
            ben.on_sent(r);
            CHECK(ben.on_ack(r, Ack::accepted));
        }
        CHECK(ben.acked == 3 && !ben.due(999999));
        cai.open(22);
        CHECK(cai.session == 22 && cai.latest == 3 && !cai.due(0));  // the newest kept, not ready yet
        cai.on_ready(0);
        CHECK(cai.due(0) && cai.sent == 0);
        cai.on_sent(10);
        CHECK(cai.sent == 3 && !cai.due(11));  // only revision 3 goes
        CHECK(!cai.on_ack(4, Ack::accepted) && cai.on_ack(3, Ack::accepted) && cai.acked == 3);
        CHECK(!ben.on_ack(4, Ack::accepted) && ben.acked == 3);  // each acknowledges its own
        ben.latest = cai.latest = 4;
        CHECK(ben.due(20) && cai.due(20));  // the next goes to both
        ben.on_sent(20);
        CHECK(!ben.due(21) && cai.due(21));
        // Revisions captured before the host last stood at the menus are never sent.
        Sender late;
        late.latest = 4;
        late.floor = 4;
        late.open(23);
        late.on_ready(0);
        CHECK(late.floor == 4 && !late.due(999999));
    }
    // Two keepalives fit in departed_limit, which is shorter than silence_limit.
    CHECK(keepalive_interval * 2 <= departed_limit && departed_limit < silence_limit);
    {  // The goodbye, the sender's floor, where the game is, and the co-op game's end.
        Frame bye;
        bye.type = Type::bye;
        bye.session = 9;
        const std::string b = encode(bye);
        Frame back;
        CHECK(b.size() == header && decode(b.data(), b.size(), back) && back.type == Type::bye && back.session == 9);
        std::string bad = b;
        bad += 'x';
        CHECK(!decode(bad.data(), bad.size(), back));
        bad = b;
        bad[5] = 1;  // a goodbye carries no revision
        CHECK(!decode(bad.data(), bad.size(), back));
        // A battle begins (F, with what joining needs) and ends (E).
        Frame battle;
        battle.type = Type::battle;
        battle.session = 9;
        battle.revision = 3;
        battle.json = "GCT-0a1b2c3d\npol1g_con\nimp\npol\nall=1111111\n1=sensor_array";
        const std::string fb = encode(battle);
        CHECK(fb.size() == header + battle.json.size() && decode(fb.data(), fb.size(), back) &&
              back.type == Type::battle && back.revision == 3 && back.json == battle.json);
        bad = fb.substr(0, header);  // no text
        CHECK(!decode(bad.data(), bad.size(), back));
        bad = fb;
        bad[5] = 0;  // battle number 0
        CHECK(!decode(bad.data(), bad.size(), back));
        battle.json.assign(max_battle + 1, 'x');
        bad = encode(battle);
        CHECK(!decode(bad.data(), bad.size(), back));
        Frame over;
        over.type = Type::battle_over;
        over.session = 9;
        over.revision = 3;
        const std::string fe = encode(over);
        CHECK(fe.size() == header && decode(fe.data(), fe.size(), back) && back.type == Type::battle_over &&
              back.revision == 3);
        bad = fe;
        bad += 'x';
        CHECK(!decode(bad.data(), bad.size(), back));
        over.revision = 0;
        bad = encode(over);
        CHECK(!decode(bad.data(), bad.size(), back));
        // The host's match result (V): the winning-team mask in all 32 bits, as MissionDefeat(2) sets
        // every bit but team 2's.
        Frame result;
        result.type = Type::battle_result;
        result.session = 9;
        result.revision = 3;
        result.result = 0xFFFFFFFBu;
        const std::string fv = encode(result);
        CHECK(fv.size() == header + 4 && decode(fv.data(), fv.size(), back) && back.type == Type::battle_result &&
              back.revision == 3 && back.result == 0xFFFFFFFBu);
        result.result = 4;
        bad = encode(result);
        CHECK(decode(bad.data(), bad.size(), back) && back.result == 4);  // MissionVictory(2)
        result.result = 0;
        bad = encode(result);
        CHECK(!decode(bad.data(), bad.size(), back));  // no team won
        result.result = 2;
        bad = encode(result) + "x";
        CHECK(!decode(bad.data(), bad.size(), back));  // trailing bytes
        bad = encode(result).substr(0, header + 1);
        CHECK(!decode(bad.data(), bad.size(), back));  // a one-byte mask
        bad = encode(result).substr(0, header);
        CHECK(!decode(bad.data(), bad.size(), back));  // missing mask
        result.revision = 0;
        bad = encode(result);
        CHECK(!decode(bad.data(), bad.size(), back));  // battle number 0
        // The friend's part in the host's battle (J), one byte.
        Frame part;
        part.type = Type::battle_part;
        part.session = 9;
        part.revision = 3;
        part.part = 4;
        const std::string fj = encode(part);
        CHECK(fj.size() == header + 1 && decode(fj.data(), fj.size(), back) && back.type == Type::battle_part &&
              back.revision == 3 && back.part == 4);
        for (uint8_t p = 1; p <= max_part; ++p) {
            part.part = p;
            bad = encode(part);
            CHECK(decode(bad.data(), bad.size(), back) && back.part == p);
        }
        part.part = 0;
        bad = encode(part);
        CHECK(!decode(bad.data(), bad.size(), back));  // no part
        part.part = static_cast<uint8_t>(max_part + 1);
        bad = encode(part);
        CHECK(!decode(bad.data(), bad.size(), back));  // unknown part
        part.part = 2;
        bad = encode(part) + "x";
        CHECK(!decode(bad.data(), bad.size(), back));  // trailing bytes
        bad = encode(part).substr(0, header);
        CHECK(!decode(bad.data(), bad.size(), back));  // missing byte
        part.revision = 0;
        bad = encode(part);
        CHECK(!decode(bad.data(), bad.size(), back));  // battle number 0
        // A sender's floor: the newest revision of an earlier galaxy, never sent.
        Sender s;
        s.open(3);
        s.latest = 7;
        s.floor = 7;
        s.on_ready(0);
        CHECK(!s.due(0));  // revision 7 is an earlier galaxy
        s.latest = 8;
        CHECK(s.due(0));
        s.open(4);
        CHECK(s.floor == 7 && s.latest == 8);  // a new session keeps both
        // Where the game is: the galaxy at once, the menus after menu_frames guarded shell frames in a row.
        PlaceWatch w;
        CHECK(w.place == Place::unknown);
        w.step(true, 3, false);
        CHECK(w.place == Place::galaxy);  // loading into the galaxy counts at once
        for (unsigned i = 0; i < 100; ++i) w.step(true, 0, false);
        CHECK(w.place == Place::galaxy);  // a suspended shell is not the menus
        for (unsigned i = 0; i + 1 < PlaceWatch::menu_frames; ++i) w.step(true, 0, true);
        CHECK(w.place == Place::galaxy);
        w.step(true, 0, false);  // one unguarded frame starts the count again
        for (unsigned i = 0; i + 1 < PlaceWatch::menu_frames; ++i) w.step(true, 4, true);
        CHECK(w.place == Place::galaxy);
        w.step(true, 4, true);
        CHECK(w.place == Place::menus);
        w.step(false, 3, false);
        CHECK(w.place == Place::menus);  // an unread flow changes nothing
        // The co-op game ends once, when a paired PC is back at the menus after its galaxy showed.
        CoopGame game;
        CHECK(!game.step(true, false, Place::menus));   // paired at the menus: waiting to start
        CHECK(!game.step(true, true, Place::galaxy));   // the co-op galaxy shows
        CHECK(!game.step(true, true, Place::menus));    // still showing (the host's place lags)
        CHECK(!game.step(true, false, Place::galaxy));  // between screens
        CHECK(game.step(true, false, Place::menus));    // back at the menus: the game is over
        CHECK(!game.step(true, false, Place::menus));   // once
        CHECK(!game.step(true, true, Place::galaxy) && !game.step(false, false, Place::menus));  // unpaired: nothing
        CHECK(!game.step(true, false, Place::menus));
    }
    {  // L only counts what this game applied in the current session.
        ShowsReport r;
        uint32_t rev = 99;
        CHECK(!r.due(3, 3, false, rev));  // no session yet
        r.open(5, 1);                     // first session: offers from 1 count
        CHECK(!r.due(0, 0, false, rev));  // nothing applied yet
        CHECK(r.due(1, 4, false, rev) && rev == 4);
        r.on_sent(4, false);
        CHECK(!r.due(1, 4, false, rev));  // sent once per change
        CHECK(r.due(2, 6, false, rev) && rev == 6);
        r.on_sent(6, false);
        // The next game's session opens after offers 1 and 2: the last game's revision 6 releases nothing.
        r.open(9, 3);
        CHECK(!r.due(2, 6, false, rev));
        CHECK(r.due(3, 8, false, rev) && rev == 8);  // the new game's galaxy shows
        r.on_sent(8, false);
        CHECK(r.due(3, 8, true, rev) && rev == 8);  // applying stopped: reported too
        r.on_sent(8, true);
        r.open(10, 4);
        CHECK(r.due(3, 8, true, rev) && rev == 0);  // stopped, nothing from this session
        r.on_sent(0, true);
        CHECK(!r.due(3, 8, true, rev));
        Frame stop;
        stop.type = Type::loaded;
        stop.session = 10;
        stop.revision = 0;
        stop.shows = Shows::stopped;
        const std::string w = encode(stop);
        Frame back;
        CHECK(decode(w.data(), w.size(), back) && back.shows == Shows::stopped && back.revision == 0);
    }

    {  // Versus frames P (a side pick) and G (the game's end).
        Frame p;
        p.type = Type::pick;
        p.session = 4;
        p.revision = 3;
        p.team = 2;
        p.ready = true;
        p.era = 1;
        p.started = true;
        Frame back;
        std::string w = encode(p);
        CHECK(w.size() == header + 4 && decode(w.data(), w.size(), back) && back.type == Type::pick &&
              back.revision == 3 && back.team == 2 && back.ready && back.era == 1 && back.started);
        p.team = 0;
        p.ready = false;
        p.era = 0;
        p.started = false;
        w = encode(p);
        CHECK(decode(w.data(), w.size(), back) && back.team == 0 && !back.ready && !back.started);  // no pick yet
        p.ready = true;
        w = encode(p);
        CHECK(!decode(w.data(), w.size(), back));  // ready without a side
        p.team = 3;
        p.ready = false;
        w = encode(p);
        CHECK(!decode(w.data(), w.size(), back));  // no third side
        p.team = 1;
        p.era = 3;
        w = encode(p);
        CHECK(!decode(w.data(), w.size(), back));  // unknown era
        p.era = 2;
        w = encode(p);
        w[header + 1] = 2;
        CHECK(!decode(w.data(), w.size(), back));  // ready is 0 or 1
        w = encode(p);
        w[header + 3] = 7;
        CHECK(!decode(w.data(), w.size(), back));  // started is 0 or 1
        w = encode(p) + "x";
        CHECK(!decode(w.data(), w.size(), back));  // trailing bytes
        p.revision = 0;
        w = encode(p);
        CHECK(!decode(w.data(), w.size(), back));  // picks are numbered from 1
        Frame over;
        over.type = Type::game_over;
        over.session = 4;
        over.revision = 21;
        over.winner = 1;
        w = encode(over);
        CHECK(w.size() == header + 1 && decode(w.data(), w.size(), back) && back.type == Type::game_over &&
              back.revision == 21 && back.winner == 1);
        over.winner = 0;
        w = encode(over);
        CHECK(!decode(w.data(), w.size(), back));  // a winner is side 1 or 2
        over.winner = 3;
        w = encode(over);
        CHECK(!decode(w.data(), w.size(), back));
        over.winner = 2;
        over.revision = 0;
        w = encode(over);
        CHECK(!decode(w.data(), w.size(), back));  // the turn it ended on
    }
    {  // Versus: each PC sends its own turn as it is played and a turn it has just
        // handed over; the host also sends the game's first state once.
        using namespace gc::versus;
        CHECK(parse_mode("versus") == Mode::versus && parse_mode("coop") == Mode::coop &&
              parse_mode(nullptr) == Mode::coop);
        CHECK(parse_mode("Versus") == Mode::coop && !strcmp(mode_name(Mode::versus), "versus"));
        Exchange x;
        // The game's first state: the host sends it once to a friend holding nothing.
        CHECK(may_send(Turn{2, 1, 2}, x, true));
        CHECK(may_send(Turn{1, 1, 2}, x, true));    // a loaded game may start on the friend's turn
        CHECK(!may_send(Turn{2, 1, 1}, x, false));  // the friend never sends the first state
        CHECK(!may_send(Turn{0, 1, 2}, x, true) && !may_send(Turn{2, 0, 2}, x, true));  // facts unknown
        x.acknowledged(1);
        // The host plays its turn 1 (side 2): it goes as it is played, so the friend watches it.
        CHECK(may_send(Turn{2, 1, 2}, x, true));
        // Its NextTurn: turn 2 is side 1's, and the friend does not have it.
        CHECK(may_send(Turn{1, 2, 2}, x, true));
        x.acknowledged(2);
        CHECK(!may_send(Turn{1, 2, 2}, x, true));  // held now: no echo
        // The friend's PC (side 1) after applying turn 2: its own turn, as it is played.
        Exchange peer;
        peer.received(2);
        CHECK(may_send(Turn{1, 2, 1}, peer, false));
        CHECK(!may_send(Turn{1, 1, 1}, peer, false));  // its own turn, but older than the host holds
        CHECK(!may_send(Turn{2, 2, 1}, peer, false));  // an older galaxy showing turn 2: not new
        CHECK(may_send(Turn{2, 3, 1}, peer, false));   // its NextTurn hands turn 3 back
        CHECK(!may_send(Turn{2, 3, 0}, peer, false));  // this PC's side unknown
        peer.acknowledged(3);
        CHECK(!may_send(Turn{2, 3, 1}, peer, false) && peer.peer_turn == 3);
        peer.received(2);
        CHECK(peer.peer_turn == 3);  // never goes back
        peer.reset();
        CHECK(!peer.peer_has_game && peer.peer_turn == 0);
        // The side screen: picks, Ready, and the host's era and start.
        Sides s;
        CHECK(!s.set_ready(true));  // Ready needs a side
        CHECK(s.pick(1) && s.mine.team == 1 && s.due() && !s.pick(1) && !s.pick(3) && !s.pick(0));
        s.sent = s.sequence;
        CHECK(!s.due());
        s.on_peer(1, true, 2, false);  // the host is ready on side 1, era GCW
        CHECK(s.heard && s.era == 2 && s.theirs.team == 1 && s.theirs.ready);
        CHECK(!s.set_ready(true));  // side 1 is taken
        CHECK(s.pick(2) && s.set_ready(true) && s.both_ready() && !s.clash());
        CHECK(!s.pick(1));  // ready: un-ready first
        CHECK(s.set_ready(false) && !s.both_ready() && s.pick(1));
        s.on_peer(1, false, 0, false);
        CHECK(s.era == 2 && !s.theirs.ready);  // an era once known stays
        CHECK(s.set_ready(true));
        s.on_peer(1, true, 2, false);  // both pressed Ready on side 1 at once
        CHECK(s.clash() && !s.both_ready());
        CHECK(s.set_ready(false) && s.pick(2) && s.set_ready(true) && s.both_ready());
        s.on_peer(1, true, 2, true);                            // the host started the galaxy
        CHECK(s.started && !s.set_ready(false) && !s.pick(1));  // final
        s.on_peer(3, true, 9, false);
        CHECK(s.theirs.team == 0 && !s.theirs.ready && s.era == 2 && s.started);
        s.reset();
        CHECK(!s.started && !s.heard && s.mine.team == 0 && s.era == 0);
    }
    {  // Frame J's quit, and frames C (the attacker asks for the defender's card) and D (the answer).
        Frame j;
        j.type = Type::battle_part;
        j.session = 4;
        j.revision = 2;
        j.part = 5;
        Frame back;
        std::string w = encode(j);
        CHECK(decode(w.data(), w.size(), back) && back.part == 5);
        j.part = 6;
        w = encode(j);
        CHECK(!decode(w.data(), w.size(), back));
        Frame c;
        c.type = Type::card_ask;
        c.session = 4;
        c.revision = 1;
        c.json = "pol1g_con";
        c.cards[0] = 3;
        c.cards[1] = 0;
        c.cards[2] = 0x1234;
        w = encode(c);
        CHECK(w.size() == header + 6 + 9 && decode(w.data(), w.size(), back) && back.type == Type::card_ask &&
              back.revision == 1 && back.json == "pol1g_con" && back.cards[0] == 3 && back.cards[1] == 0 &&
              back.cards[2] == 0x1234);
        c.json = "";
        w = encode(c);
        CHECK(!decode(w.data(), w.size(), back));  // a mission is needed
        c.json = "pol1g con";
        w = encode(c);
        CHECK(!decode(w.data(), w.size(), back));  // letters, digits and '_' only
        c.json = std::string(32, 'a');
        w = encode(c);
        CHECK(!decode(w.data(), w.size(), back));  // at most 31
        c.json = "tat2g_eli";
        c.revision = 0;
        w = encode(c);
        CHECK(!decode(w.data(), w.size(), back));  // requests are numbered from 1
        Frame d;
        d.type = Type::card_pick;
        d.session = 4;
        d.revision = 1;
        d.slot = 2;
        w = encode(d);
        CHECK(w.size() == header + 1 && decode(w.data(), w.size(), back) && back.type == Type::card_pick &&
              back.revision == 1 && back.slot == 2);
        d.slot = 0;
        w = encode(d);
        CHECK(decode(w.data(), w.size(), back) && back.slot == 0);  // no card
        d.slot = 4;
        w = encode(d);
        CHECK(!decode(w.data(), w.size(), back));  // three slots
        d.slot = 1;
        d.revision = 0;
        w = encode(d);
        CHECK(!decode(w.data(), w.size(), back));
    }
    {  // The card exchange: the attacker's PC asks; the defender's shows its pass once
        // and answers; only the newest request counts.
        using namespace gc::versus;
        CardAsk a;
        CHECK(!a.due() && !a.answered_now());
        const uint16_t cards[3] = {4, 0, 7};
        a.ask("pol1g_con", cards);
        CHECK(a.number == 1 && a.due() && a.cards[2] == 7 && !a.answered_now());
        a.sent = a.number;
        CHECK(!a.due());
        CHECK(!a.on_answer(2, 1) && !a.on_answer(1, 4) && !a.answered_now());  // another request, a fourth slot
        CHECK(a.on_answer(1, 3) && a.answered_now() && a.slot == 3 && !a.on_answer(1, 1) && a.slot == 3);
        a.ask("pol1g_con", cards);  // the ground battle after a won space battle
        CHECK(a.number == 2 && !a.answered_now() && a.due() && !a.on_answer(1, 2));
        CardPeer p;
        CHECK(!p.open() && !p.pending());
        CHECK(p.on_ask(1, "pol1g_con", cards) && p.open() && p.pending() && p.cards[0] == 4);
        CHECK(!p.on_ask(1, "pol1g_con", cards));  // a repeat
        p.take(1);
        CHECK(p.open() && !p.pending());  // shown, not answered
        CHECK(!p.answer(2, 1) && !p.answer(1, 4));
        CHECK(p.answer(1, 3) && !p.open() && p.answer_due() && p.slot == 3);
        p.answer_sent = p.answered;
        CHECK(!p.answer_due() && !p.answer(1, 2));
        CHECK(p.on_ask(2, "tat2g_eli", cards) && p.pending() && !p.answer_due());
        p.close();  // its battle begins without an answer
        CHECK(!p.open() && !p.pending() && !p.answer(2, 1));
        CHECK(p.on_ask(3, "tat2g_eli", cards) && p.open());  // a newer request opens again
        p.reset();
        CHECK(!p.open() && p.number == 0);
    }

    // The one left offers to save while its galaxy shows and the game goes on: in co-op only the
    // host, in Versus either PC.
    CHECK(save_prompt_due(false, true, true, false));    // co-op: the friend left the host
    CHECK(!save_prompt_due(false, false, true, false));  // co-op: the host left; its galaxy goes
    CHECK(save_prompt_due(true, true, true, false) && save_prompt_due(true, false, true, false));
    CHECK(!save_prompt_due(true, false, false, false) && !save_prompt_due(false, true, false, false));  // at the menus
    CHECK(!save_prompt_due(true, true, true, true) && !save_prompt_due(true, false, true, true));  // the game ended

    if (failures) {
        std::printf("%d session check(s) failed\n", failures);
        return 1;
    }
    std::puts(
        "session_test PASS: frame codec, revision and resend rules, ready check, Versus turns, sides and cards, save prompt");
    return 0;
}
