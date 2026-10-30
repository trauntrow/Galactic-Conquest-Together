#pragma once
// The session protocol between paired PCs over Steam P2P. Pure: no Steam, Windows or game calls.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace gc::session {
constexpr int channel = 8;                           // P2P channel; the developer relay uses 7, battles 9
constexpr uint8_t version = 1;                       // protocol version, carried by H
constexpr size_t header = 9, max_json = 512 * 1024;  // header: type, session, revision
// Intervals in ms: H while a session opens, and before an unconfirmed snapshot goes again.
constexpr unsigned long long hello_interval = 2000, resend_interval = 5000;
// A paired session lives on P2P, not on lobby membership. H doubles as the keepalive; a peer silent
// for silence_limit is gone, or for departed_limit once it has also left the lobby.
constexpr unsigned long long keepalive_interval = 5000, silence_limit = 30000, departed_limit = 10000;
using Digest = std::array<uint8_t, 32>;

// Host to friend: H, S, F, E, V; friend to host: R, K, L, J; either way: B. In Versus S and K go both
// ways, battle frames follow the battle's host, and P, G, C and D are Versus only.
enum class Type : char {
    hello = 'H',
    ready = 'R',
    snapshot = 'S',
    ack = 'K',
    loaded = 'L',
    bye = 'B',
    battle = 'F',
    battle_over = 'E',
    battle_part = 'J',
    battle_result = 'V',
    pick = 'P',
    game_over = 'G',
    card_ask = 'C',
    card_pick = 'D'
};
constexpr size_t max_battle = 600;  // F's text, battle_logic.h encode_info
constexpr uint8_t max_part = 5;     // J's byte, battle_logic.h Part
// C and D: a side's three bonus card slots (ifs_purchase_tech_using[team], each an
// ifs_purchase_tech_table index or 0) and the mission they are for.
constexpr size_t card_slots = 3, max_card_mission = 31;
// A mission name: 1 to 31 letters, digits or underscores.
inline bool card_mission_ok(const std::string& s) {
    if (s.empty() || s.size() > max_card_mission) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
// K's verdict on a revision, and what L says the friend's game shows.
enum class Ack : uint8_t { accepted = 1, duplicate = 2, stale = 3, rejected = 4 };
enum class Shows : uint8_t { applied = 1, stopped = 2 };

// Capture metadata sent with each snapshot (capture_live.h CaptureOffer).
struct Meta {
    uint32_t producer = 0, generation = 0, sequence = 0, source_table = 0, update_ticket = 0;
    int32_t unchanged_samples = -1;
};

// A decoded frame; which fields count depends on its type.
struct Frame {
    Type type = Type::hello;
    uint32_t session = 0, revision = 0;
    Meta meta;
    Ack ack = Ack::accepted;
    Shows shows = Shows::applied;
    uint8_t part = 0;
    uint32_t result = 0;  // V: the battle host's winning-team mask, never 0
    // P: this PC's side pick; the host's also carries the era and the start.
    uint8_t team = 0, era = 0;
    bool ready = false, started = false;
    uint8_t winner = 0;            // G: the winning side
    uint16_t cards[card_slots]{};  // C: the defender's card slots
    uint8_t slot = 0;              // D: the slot picked, 0 for none
    Digest digest{};
    std::string json;  // S: the snapshot; F: the battle info; C: the mission
};

// Little-endian u32 helpers.
inline void put32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}
inline uint32_t get32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

// Wire format: type, session (LE u32), revision (LE u32), payload. The revision field also numbers
// battles (F, E, V, J), picks (P), the final turn (G) and card requests (C, D).
inline std::string encode(const Frame& f) {
    std::string out(1, static_cast<char>(f.type));
    put32(out, f.session);
    put32(out, f.revision);
    switch (f.type) {
    case Type::hello: out += static_cast<char>(version); break;
    case Type::ready: break;
    case Type::snapshot:
        // Meta as six LE u32, the JSON's SHA-256, then the normalized JSON.
        for (uint32_t v : {f.meta.producer, f.meta.generation, f.meta.sequence, f.meta.source_table,
                           f.meta.update_ticket, static_cast<uint32_t>(f.meta.unchanged_samples)})
            put32(out, v);
        out.append(reinterpret_cast<const char*>(f.digest.data()), f.digest.size());
        out += f.json;
        break;
    case Type::ack:
        // The verdict, then the digest the receiver holds for that revision.
        out += static_cast<char>(f.ack);
        out.append(reinterpret_cast<const char*>(f.digest.data()), f.digest.size());
        break;
    case Type::loaded: out += static_cast<char>(f.shows); break;
    case Type::bye: break;
    case Type::battle: out += f.json; break;
    case Type::battle_over: break;
    case Type::battle_part: out += static_cast<char>(f.part); break;
    case Type::battle_result: put32(out, f.result); break;
    case Type::pick:
        // Side (0 to 2), ready, era (0 to 2), started.
        out += static_cast<char>(f.team);
        out += static_cast<char>(f.ready ? 1 : 0);
        out += static_cast<char>(f.era);
        out += static_cast<char>(f.started ? 1 : 0);
        break;
    case Type::game_over: out += static_cast<char>(f.winner); break;
    case Type::card_ask:
        // Three LE u16 card slots, then the mission.
        for (uint16_t c : f.cards) {
            out += static_cast<char>(c & 0xFF);
            out += static_cast<char>(c >> 8);
        }
        out += f.json;
        break;
    case Type::card_pick: out += static_cast<char>(f.slot); break;
    }
    return out;
}

// Parses and validates a frame; false for anything malformed or a zero session id.
inline bool decode(const char* p, size_t n, Frame& f) {
    if (n < header) return false;
    f = Frame{};
    f.type = static_cast<Type>(p[0]);
    f.session = get32(p + 1);
    f.revision = get32(p + 5);
    const char* body = p + header;
    const size_t size = n - header;
    if (!f.session) return false;
    switch (f.type) {
    case Type::hello: return size == 1 && static_cast<uint8_t>(body[0]) == version && f.revision == 0;
    case Type::ready: return size == 0;
    case Type::snapshot: {
        constexpr size_t fixed = 24 + 32;
        if (size <= fixed || size - fixed > max_json || !f.revision) return false;
        f.meta.producer = get32(body);
        f.meta.generation = get32(body + 4);
        f.meta.sequence = get32(body + 8);
        f.meta.source_table = get32(body + 12);
        f.meta.update_ticket = get32(body + 16);
        f.meta.unchanged_samples = static_cast<int32_t>(get32(body + 20));
        memcpy(f.digest.data(), body + 24, 32);
        f.json.assign(body + fixed, size - fixed);
        return f.meta.producer >= 1 && f.meta.producer <= 7;
    }
    case Type::ack:
        if (size != 33 || !f.revision) return false;
        f.ack = static_cast<Ack>(static_cast<uint8_t>(body[0]));
        memcpy(f.digest.data(), body + 1, 32);
        return f.ack >= Ack::accepted && f.ack <= Ack::rejected;
    case Type::loaded:
        if (size != 1) return false;
        f.shows = static_cast<Shows>(static_cast<uint8_t>(body[0]));
        return (f.shows == Shows::applied && f.revision) || f.shows == Shows::stopped;
    case Type::bye: return size == 0 && f.revision == 0;
    case Type::battle:
        if (!f.revision || !size || size > max_battle) return false;
        f.json.assign(body, size);
        return true;
    case Type::battle_over: return size == 0 && f.revision != 0;
    case Type::battle_part:
        if (size != 1 || !f.revision) return false;
        f.part = static_cast<uint8_t>(body[0]);
        return f.part >= 1 && f.part <= max_part;
    case Type::battle_result:
        if (size != 4 || !f.revision) return false;
        f.result = get32(body);
        return f.result != 0;
    case Type::pick: {
        if (size != 4 || !f.revision) return false;
        const auto byte = [&](size_t i) { return static_cast<uint8_t>(body[i]); };
        f.team = byte(0);
        f.era = byte(2);
        f.ready = byte(1) == 1;
        f.started = byte(3) == 1;
        return f.team <= 2 && byte(1) <= 1 && f.era <= 2 && byte(3) <= 1 && (f.team || !f.ready);
    }
    case Type::game_over:
        if (size != 1 || !f.revision) return false;
        f.winner = static_cast<uint8_t>(body[0]);
        return f.winner == 1 || f.winner == 2;
    case Type::card_ask: {
        constexpr size_t fixed = 2 * card_slots;
        if (size <= fixed || !f.revision) return false;
        for (size_t i = 0; i < card_slots; ++i)
            f.cards[i] = static_cast<uint16_t>(static_cast<unsigned char>(body[2 * i]) |
                                               (static_cast<unsigned char>(body[2 * i + 1]) << 8));
        f.json.assign(body + fixed, size - fixed);
        return card_mission_ok(f.json);
    }
    case Type::card_pick:
        if (size != 1 || !f.revision) return false;
        f.slot = static_cast<uint8_t>(body[0]);
        return f.slot <= card_slots;
    }
    return false;
}

// Receiving side: the revision held and the verdict on each new one. Revisions only move forward.
struct Receiver {
    enum class Verdict { other_session, accept, duplicate, stale, conflict };
    uint32_t session = 0, revision = 0;
    Digest digest{};

    // A new session id starts empty; the current one keeps what it holds.
    bool open(uint32_t id) {
        if (id == session) return false;
        session = id;
        revision = 0;
        digest = Digest{};
        return true;
    }
    // Higher is accepted and lower stale; the same number must carry the same digest.
    Verdict classify(uint32_t id, uint32_t rev, const Digest& d) const {
        if (!session || id != session) return Verdict::other_session;
        if (rev > revision) return Verdict::accept;
        if (rev < revision) return Verdict::stale;
        return d == digest ? Verdict::duplicate : Verdict::conflict;
    }
    // Call only once the content itself has been validated.
    void commit(uint32_t rev, const Digest& d) {
        revision = rev;
        digest = d;
    }
};

// Sending side: the newest revision, the last one sent, and what the receiver has confirmed or refused.
struct Sender {
    uint32_t session = 0, latest = 0, sent = 0, acked = 0, refused = 0;
    uint32_t floor = 0;  // revisions up to it predate the last visit to the menus and are never sent
    bool ready = false;
    unsigned long long sent_at = 0;

    // A new session keeps latest and floor and resets the rest.
    void open(uint32_t id) {
        const uint32_t keep = latest, below = floor;
        *this = Sender{};
        latest = keep;
        floor = below;
        session = id;
    }
    // The receiver is ready and holds `held`; anything newer goes again.
    void on_ready(uint32_t held) {
        ready = true;
        if (held <= latest && held > acked) acked = held;
        sent = acked;
    }
    // True for a newer revision, neither confirmed nor refused, that is unsent or due for a resend.
    bool due(unsigned long long now) const {
        return ready && latest > floor && latest > acked && latest != refused &&
               (sent < latest || now - sent_at >= resend_interval);
    }
    void on_sent(unsigned long long now) {
        sent = latest;
        sent_at = now;
    }
    // False for an acknowledgement that cannot be about this session's revisions.
    bool on_ack(uint32_t rev, Ack status) {
        if (!rev || rev > latest) return false;
        if (status == Ack::accepted || status == Ack::duplicate) {
            if (rev > acked) acked = rev;
        } else if (status == Ack::rejected && rev == latest) refused = rev;
        return true;
    }
};

// Where this PC's game is, from the frame hook. The galaxy (flow 3) counts at once; the menus
// (flow 0 or 4) only after menu_frames guarded frames in a row, so a load passing through never does.
enum class Place : int { unknown = 0, menus = 1, galaxy = 2 };
struct PlaceWatch {
    static constexpr unsigned menu_frames = 30;
    Place place = Place::unknown;
    unsigned at_menus = 0;
    void step(bool flow_read, uint32_t flow, bool guards) {
        if (flow_read && flow == 3) {
            place = Place::galaxy;
            at_menus = 0;
            return;
        }
        if (guards && (flow == 0 || flow == 4)) {
            if (++at_menus >= menu_frames) place = Place::menus;
            return;
        }
        at_menus = 0;
    }
};

// A co-op game is one galaxy: once it has shown in a ready session, this PC returning to the menus
// ends the game for both (B, and both leave the lobby). The next game needs a new invite.
struct CoopGame {
    bool shown = false;
    // True once, when this PC has left the co-op galaxy for the menus.
    bool step(bool ready, bool galaxy_showing, Place place) {
        if (!ready) {
            shown = false;
            return false;
        }
        if (galaxy_showing) {
            shown = true;
            return false;
        }
        if (shown && place == Place::menus) {
            shown = false;
            return true;
        }
        return false;
    }
};

// Once the other PC has left: whether this game offers the stock save prompt. In co-op only the host
// saves (the friend shows the host's galaxy); in Versus each PC saves its own.
inline bool save_prompt_due(bool versus, bool session_host, bool galaxy_shows, bool game_over) {
    return galaxy_shows && !game_over && (versus || session_host);
}

// Friend side: what L has told the host's ready check. Only offers applied in this session count
// (offers use the friend's own numbering); applying having stopped always counts.
struct ShowsReport {
    uint32_t session = 0, first_offer = 0;
    bool sent = false, sent_stopped = false;
    uint32_t sent_revision = 0;

    void open(uint32_t id, uint32_t next_offer) {
        *this = ShowsReport{};
        session = id;
        first_offer = next_offer;
    }
    // True when an L is due; `revision` is the host revision shown from this session, or 0 if none.
    bool due(uint32_t applied_offer, uint32_t applied_revision, bool stopped, uint32_t& revision) const {
        const bool current = session && first_offer && applied_offer >= first_offer && applied_revision;
        if (!session || (!current && !stopped)) return false;
        revision = current ? applied_revision : 0;
        return !sent || sent_revision != revision || sent_stopped != stopped;
    }
    void on_sent(uint32_t revision, bool stopped) {
        sent = true;
        sent_revision = revision;
        sent_stopped = stopped;
    }
};

// Host side: the ready check. The galaxy waits under the busy popup (player_hold.lua) until each
// awaited friend's game shows it, stops applying or leaves, the host cancels, or hold_limit passes.
constexpr unsigned long long hold_limit = 60000;  // ms from when the popup appears, not from the join
constexpr size_t hold_friends = 3;
struct Hold {
    enum class State : uint8_t { idle, waiting, shown, done };
    State state = State::idle;
    uint32_t session = 0;              // the first friend's, for the HOLD_* log lines
    uint32_t awaited[hold_friends]{};  // sessions still loading; 0 once done
    unsigned friends = 0;              // how many it started with
    unsigned long long shown_at = 0;
    const char* reason = "";  // why it ended

    // A session it has not seen yet is ready: hold the next galaxy frames.
    bool ready(uint32_t id) {
        if (!id || id == session) return false;
        return again(id);
    }
    // Holds again for the friend already paired.
    bool again(uint32_t id) { return start(&id, 1); }
    // Waits for these sessions (the first hold_friends nonzero ids); false if there are none.
    bool start(const uint32_t* ids, size_t n) {
        Hold next;
        for (size_t i = 0; i < n && next.friends < hold_friends; ++i)
            if (ids[i]) next.awaited[next.friends++] = ids[i];
        if (!next.friends) return false;
        next.session = next.awaited[0];
        next.state = State::waiting;
        *this = next;
        return true;
    }
    // How many friends' games it still waits for.
    unsigned loading() const {
        unsigned n = 0;
        for (uint32_t id : awaited) n += id ? 1 : 0;
        return n;
    }
    // The popup has appeared: hold_limit runs from now.
    void shown(unsigned long long now) {
        if (state == State::waiting) {
            state = State::shown;
            shown_at = now;
        }
    }
    // Ends a hold in progress with `why` as its reason; false if there is none.
    bool release(const char* why) {
        if (state != State::waiting && state != State::shown) return false;
        state = State::done;
        reason = why;
        return true;
    }
    // A friend's game shows the galaxy or has stopped applying. True when that ends the hold.
    bool on_loaded(uint32_t id, Shows shows) {
        if (!drop(id) || loading()) return false;
        if (friends == 1)
            return release(shows == Shows::applied ? "the friend's game shows the galaxy"
                                                   : "the friend's game stopped applying");
        return release(shows == Shows::applied ? "the last friend's game shows the galaxy"
                                               : "the last friend's game stopped applying");
    }
    // A friend has left and is no longer awaited. True when that ends the hold.
    bool on_left(uint32_t id) {
        if (!drop(id) || loading()) return false;
        return release(friends == 1 ? "the friend left" : "the last friend still loading left");
    }
    // Whether the galaxy should be held now; releases it once hold_limit has passed.
    bool wanted(unsigned long long now) {
        if (state == State::shown && now - shown_at >= hold_limit)
            release(friends > 1 ? "the friends' games took too long" : "the friend's game took too long");
        return state == State::waiting || state == State::shown;
    }

private:
    // Stops waiting for `id`; false if it is not awaited.
    bool drop(uint32_t id) {
        for (uint32_t& awaited_id : awaited)
            if (id && awaited_id == id) {
                awaited_id = 0;
                return true;
            }
        return false;
    }
};
}
