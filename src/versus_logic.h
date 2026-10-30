#pragma once
// Versus rules for the session: which turns go to the other PC, the side screen and the defender's
// bonus card. Pure logic with no Steam, Windows or game calls, tested by tests/session_test.cpp.
#include <cstdint>
#include <cstring>
#include <string>

namespace gc::versus {
// Co-op or Versus, chosen by the host's invite and read by the friend from the lobby data.
enum class Mode : uint8_t { coop = 0, versus = 1 };
constexpr char mode_key[] = "gct_mode";
inline const char* mode_name(Mode m) { return m == Mode::versus ? "versus" : "coop"; }
inline Mode parse_mode(const char* text) { return text && !strcmp(text, "versus") ? Mode::versus : Mode::coop; }

// A revision's turn facts, read from its snapshot when it is captured (capture_live.cpp) or received.
struct Turn {
    int player_team = 0;  // playerTeam: the side whose turn it is, 1 or 2
    int turn_number = 0;  // turnNumber: half-turns counted from 1
    int local_team = 0;   // controllerTeam: the side this PC plays, 0 if unknown
};

// How much of this game the other PC already holds.
struct Exchange {
    int peer_turn = 0;           // newest turnNumber the other PC has
    bool peer_has_game = false;  // it holds this game at all, received or loaded from this PC
    // The other PC sent this turn, so it holds it.
    void received(int turn) {
        peer_has_game = true;
        if (turn > peer_turn) peer_turn = turn;
    }
    // The other PC acknowledged one of this PC's revisions with this turn.
    void acknowledged(int turn) {
        peer_has_game = true;
        if (turn > peer_turn) peer_turn = turn;
    }
    void reset() { *this = Exchange{}; }
};

// Whether this PC may send a revision with these facts now. Each PC sends its own turn while it is
// played and when it hands over; what it only shows of the other PC's turn never goes back.
inline bool may_send(const Turn& t, const Exchange& x, bool session_host) {
    if ((t.player_team != 1 && t.player_team != 2) || t.turn_number < 1) return false;
    // A game's first state goes once, from the session host, so the friend's game can load it.
    if (!x.peer_has_game) return session_host;
    if (t.local_team != 1 && t.local_team != 2) return false;
    // This PC's own turn in progress, never older than what the other PC holds.
    if (t.player_team == t.local_team) return t.turn_number >= x.peer_turn;
    // A turn this PC has just handed over, which the other PC does not have yet.
    return t.turn_number > x.peer_turn;
}

// One PC's choice on the side screen shown before a Versus galaxy starts (player_menu.lua).
struct Pick {
    uint8_t team = 0;  // 0 for none, else 1 or 2
    bool ready = false;
};
// Both PCs' choices. Frame P carries this PC's pick; the host's also carries the era and the start.
struct Sides {
    Pick mine, theirs;
    uint8_t era = 0;                  // the host's Prefs.iEra: 1 Clone Wars, 2 Galactic Civil War; 0 until known
    bool started = false;             // the host started the galaxy, so the picks are final
    bool heard = false;               // the other PC's pick has arrived
    uint32_t sequence = 0, sent = 0;  // P frame numbers: this PC's newest change and the last one sent

    // A side can change until this PC is ready. Returns whether the pick changed.
    bool pick(uint8_t team) {
        if (started || mine.ready || (team != 1 && team != 2) || team == mine.team) return false;
        mine.team = team;
        ++sequence;
        return true;
    }
    // Ready needs a side, and not one the other player is already ready on.
    bool set_ready(bool ready) {
        if (started || ready == mine.ready) return false;
        if (ready && (!mine.team || (theirs.ready && theirs.team == mine.team))) return false;
        mine.ready = ready;
        ++sequence;
        return true;
    }
    // The other PC's pick. Only the host's frames carry an era or the start.
    void on_peer(uint8_t team, bool ready, uint8_t host_era, bool host_started) {
        heard = true;
        theirs.team = team == 1 || team == 2 ? team : 0;
        theirs.ready = ready && theirs.team != 0;
        if (host_era == 1 || host_era == 2) era = host_era;
        if (host_started) started = true;
    }
    // Both ready on different sides: the host may start the galaxy.
    bool both_ready() const {
        return mine.ready && theirs.ready && mine.team && theirs.team && mine.team != theirs.team;
    }
    // Both ready on the same side: one of them has to change.
    bool clash() const { return mine.team && mine.team == theirs.team && mine.ready && theirs.ready; }
    bool due() const { return sequence != sent; }
    void reset() { *this = Sides{}; }
};

// The defender's bonus card is picked on the defender's PC: the attacker's card screen asks with
// frame C and waits for frame D, which carries the slot picked.
constexpr unsigned card_slots = 3;
// The attacker's side of the exchange.
struct CardAsk {
    uint32_t number = 0, sent = 0;  // the newest request and the last one sent
    std::string mission;
    uint16_t cards[card_slots]{};
    uint32_t answered = 0;  // which request the other PC answered
    uint8_t slot = 0;       // the slot it picked, 0 for none
    // A new request for this mission's cards; any earlier answer no longer counts.
    void ask(const std::string& m, const uint16_t c[card_slots]) {
        ++number;
        mission = m;
        for (unsigned i = 0; i < card_slots; ++i) cards[i] = c[i];
        answered = 0;
        slot = 0;
    }
    bool due() const { return number && number != sent; }
    // Only the newest request's answer counts, and only once.
    bool on_answer(uint32_t n, uint8_t s) {
        if (!n || n != number || answered == n || s > card_slots) return false;
        answered = n;
        slot = s;
        return true;
    }
    bool answered_now() const { return number && answered == number; }
};
// The defender's side of the exchange, asked by the other PC.
struct CardPeer {
    uint32_t number = 0, taken = 0, answered = 0, answer_sent = 0;
    std::string mission;
    uint16_t cards[card_slots]{};
    uint8_t slot = 0;
    bool closed = false;  // the attacker moved on: its battle is announced or a newer request came
    // A newer request replaces the current one; a repeat changes nothing.
    bool on_ask(uint32_t n, const std::string& m, const uint16_t c[card_slots]) {
        if (!n || n <= number) return false;
        number = n;
        mission = m;
        for (unsigned i = 0; i < card_slots; ++i) cards[i] = c[i];
        taken = 0;
        slot = 0;
        closed = false;
        return true;
    }
    // Still open for the defender's answer.
    bool open() const { return number && answered != number && !closed; }
    // The defender's game has not shown it yet.
    bool pending() const { return open() && taken != number; }
    // Marks request n as shown by the defender's game.
    void take(uint32_t n) {
        if (n == number) taken = n;
    }
    // The defender's pick for request n, accepted while that request is open.
    bool answer(uint32_t n, uint8_t s) {
        if (n != number || !open() || s > card_slots) return false;
        answered = n;
        slot = s;
        return true;
    }
    bool answer_due() const { return answered && answered != answer_sent; }
    void close() {
        if (number) closed = true;
    }
    void reset() { *this = CardPeer{}; }
};
}
