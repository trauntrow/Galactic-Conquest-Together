#pragma once
// Shared battles: settings, battle info, bonus cards, host and friend trackers and the LAN tunnel
// format. Pure logic with no Windows, Steam or game calls, tested by tests/battle_test.cpp.
#include <cstdint>
#include <cstring>
#include <string>

namespace gc::battle {
// The battles= setting in gct.ini. shared: a co-op host's battles are network matches its friends
// join; single: the host fights alone; hosted: every battle is a network match, for one-PC tests.
enum class Mode { single, shared, hosted };
constexpr Mode default_mode = Mode::shared;

// Compares `a`, in any case, with the lowercase word `b`.
inline bool same_word(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
        if (x != *b) return false;
        if (!x) return true;
    }
}
// Parses battles=. Off, no, 0 and false also mean single; unknown text keeps the default, so a typo
// never turns the one-PC test on.
inline Mode parse_mode(const char* text) {
    if (!text) return default_mode;
    if (same_word(text, "shared")) return Mode::shared;
    if (same_word(text, "hosted")) return Mode::hosted;
    if (same_word(text, "single") || same_word(text, "off") || same_word(text, "no") || same_word(text, "0") ||
        same_word(text, "false"))
        return Mode::single;
    return default_mode;
}
inline const char* mode_name(Mode m) { return m == Mode::shared ? "shared" : m == Mode::hosted ? "hosted" : "single"; }

// Answers GCOBS:ask:battle_host, asked when the galaxy launches a battle: whether to host it as a
// network match instead of the single-player one.
inline bool host_battle(Mode mode, bool coop_host_paired) {
    return mode == Mode::hosted || (mode == Mode::shared && coop_host_paired);
}

// Engine state hashes (FNV-1a of the name passed to SetState).
constexpr uint32_t state_shell = 0x11E1FC01;    // SetState("shell"): menus and the galaxy
constexpr uint32_t state_mission = 0x8FF60339;  // SetState("mission"): any battle

// The session name the host advertises and the friend's game searches for: "GCT" and 1 to 28
// letters, digits or dashes.
inline bool valid_token(const std::string& s) {
    if (s.size() < 4 || s.size() > 31 || s.compare(0, 3, "GCT")) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
// Non-empty, at most `most` characters, each a letter, digit or '_'.
inline bool word_chars(const std::string& s, size_t most) {
    if (s.empty() || s.size() > most) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
// Mission names are the game's own, such as pol1g_con.
inline bool valid_mission(const std::string& s) { return word_chars(s, 31); }
// The four team codes a side can be (ifs_freeform_main.teamCode).
inline bool valid_side(const std::string& s) { return s == "all" || s == "imp" || s == "rep" || s == "cis"; }
// The mission setup's world: the planet the battle is on, or "spa". Space battles load that
// planet's sky.
inline bool valid_world(const std::string& s) { return word_chars(s, 16); }
// Unit classes bought per side, as SaveMissionSetup records them: "all=1111111,imp=1111100", one
// digit each for soldier, pilot, assault, sniper, engineer, officer, special. Unlisted sides get all.
inline bool valid_units(const std::string& s) {
    if (s.empty()) return false;
    size_t at = 0;
    for (unsigned sides = 0; sides < 4; ++sides) {
        if (s.size() - at < 11 || !valid_side(s.substr(at, 3)) || s[at + 3] != '=') return false;
        for (size_t i = at + 4; i < at + 11; ++i)
            if (s[i] != '0' && s[i] != '1') return false;
        at += 11;
        if (at == s.size()) return true;
        if (s[at] != ',') return false;
        ++at;
    }
    return false;
}

// Team bonuses as "1=<name>,2=<name>" by galaxy team (1 all or rep, 2 imp or cis): each side's tech
// card, whose AcceptBonus activates team_bonus_<name>. A side without a card is left out.
inline bool parse_bonus(const std::string& s, std::string names[2]) {
    names[0].clear();
    names[1].clear();
    if (s.empty()) return true;
    int last = 0;
    for (size_t at = 0;;) {
        const size_t end = s.find(',', at);
        const std::string entry = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (entry.size() < 3 || (entry[0] != '1' && entry[0] != '2') || entry[1] != '=') return false;
        const int team = entry[0] - '0';
        if (team <= last || !word_chars(entry.substr(2), 31)) return false;  // teams in order, once each
        names[team - 1] = entry.substr(2);
        last = team;
        if (end == std::string::npos) return true;
        at = end + 1;
    }
}
inline bool valid_bonus(const std::string& s) {
    std::string names[2];
    return parse_bonus(s, names);
}
// The engine's name hash: FNV-1a over the bytes, each OR'd with 0x20 first, so '_' counts as 0x7F.
// ActivateBonus stores a bonus as the hash of its whole name; team_bonus_autoturrets is DE68CB69.
inline uint32_t engine_hash(const std::string& s) {
    uint32_t h = 0x811C9DC5;
    for (unsigned char c : s) h = (h ^ (c | 0x20u)) * 0x01000193;
    return h;
}
inline uint32_t bonus_hash(const std::string& name) { return name.empty() ? 0 : engine_hash("team_bonus_" + name); }
// The Leader card gives its side a hero and turns heroes on in the host's game, which changes the
// host's network messages.
inline bool has_leader(const std::string& bonus) {
    std::string names[2];
    return parse_bonus(bonus, names) && (names[0] == "leader" || names[1] == "leader");
}
// The bonus manager's slot for a battle team: team - 1, swapped under metagame rules when team 2's
// faction is lower. The friend's game copies the session's metagame flag, 0, so it never swaps.
inline int bonus_slot(int team, bool metagame, int faction1, int faction2) {
    return metagame && faction2 < faction1 ? 2 - team : team - 1;
}
// Fills the friend's slots with the card each battle team gets on the host. galaxy[] holds the
// announced cards' hashes by galaxy team, whose team 1 (all or rep) always has the lower faction.
inline bool friend_bonus_slots(const uint32_t galaxy[2], int faction1, int faction2, bool friend_metagame,
                               uint32_t out[2]) {
    if (faction1 <= 0 || faction2 <= 0) return false;
    for (int team = 1; team <= 2; ++team)
        out[bonus_slot(team, friend_metagame, faction1, faction2)] = galaxy[bonus_slot(team, true, faction1, faction2)];
    return true;
}

// What the friend's game needs to join the host's battle. Side, world and units may be empty when
// the host's script could not read them; the bonus is empty when neither side has a card.
struct Info {
    std::string token, mission, side, world, units, bonus;
    bool valid() const {
        return valid_token(token) && valid_mission(mission) && (side.empty() || valid_side(side)) &&
               (world.empty() || valid_world(world)) && (units.empty() || valid_units(units)) && valid_bonus(bonus);
    }
    // Side, world and units all present; without them the friend's game sets up its own.
    bool complete() const { return valid() && !side.empty() && !world.empty() && !units.empty(); }
};
// The script's report after "hosting:": token:mission[:side:world:units[:bonus]]. The battle is
// hosted whatever the optional fields say, so one that does not check out is dropped.
inline bool parse_hosting(const char* text, Info& out) {
    if (!text) return false;
    Info i;
    std::string* parts[] = {&i.token, &i.mission, &i.side, &i.world, &i.units, &i.bonus};
    size_t count = 0;
    for (const char* p = text;; ++count) {
        const char* end = strchr(p, ':');
        if (count == 6) return false;  // too many fields
        parts[count]->assign(p, end ? static_cast<size_t>(end - p) : strlen(p));
        if (!end) {
            ++count;
            break;
        }
        p = end + 1;
    }
    if ((count != 2 && count != 5 && count != 6) || !valid_token(i.token) || !valid_mission(i.mission)) return false;
    if (!i.side.empty() && !valid_side(i.side)) i.side.clear();
    if (!i.world.empty() && !valid_world(i.world)) i.world.clear();
    if (!i.units.empty() && !valid_units(i.units)) i.units.clear();
    if (!valid_bonus(i.bonus)) i.bonus.clear();
    out = i;
    return true;
}
// Frame F's payload: the six Info fields, one per line.
inline std::string encode_info(const Info& i) {
    return i.token + "\n" + i.mission + "\n" + i.side + "\n" + i.world + "\n" + i.units + "\n" + i.bonus;
}
inline bool decode_info(const std::string& text, Info& out) {
    Info i;
    std::string* parts[] = {&i.token, &i.mission, &i.side, &i.world, &i.units, &i.bonus};
    size_t start = 0;
    for (size_t k = 0; k < 6; ++k) {
        const size_t end = text.find('\n', start);
        if (k < 5 && end == std::string::npos) return false;
        if (k == 5 && end != std::string::npos) return false;
        parts[k]->assign(text, start, (k < 5 ? end : text.size()) - start);
        start = end + 1;
    }
    if (!i.valid()) return false;
    out = i;
    return true;
}

// What the friend's game did in the host's battle, sent with frame J. stayed: Back on the joining
// popup; left: out before the end; quit: its player quit, which loses a Versus battle.
enum class Part : uint8_t { none = 0, joined = 1, failed = 2, stayed = 3, left = 4, quit = 5 };
inline const char* part_name(Part p) {
    switch (p) {
    case Part::joined: return "joined";
    case Part::failed: return "failed";
    case Part::stayed: return "stayed";
    case Part::left: return "left";
    case Part::quit: return "quit";
    default: return "none";
    }
}
inline Part part_from(uint8_t v) { return v >= 1 && v <= 5 ? static_cast<Part>(v) : Part::none; }

// The note a galaxy shows after a battle not everyone fought (player_menu.lua). The script asks
// GCOBS:ask:battle_note_<name> and reports GCOBS:battle:noted:<name> once it is shown.
enum class Note { none, join_failed, friend_failed, friend_stayed, friend_left, friend_quit, friends };
inline const char* note_name(Note n) {
    switch (n) {
    case Note::join_failed: return "join_failed";
    case Note::friend_failed: return "friend_failed";
    case Note::friend_stayed: return "friend_stayed";
    case Note::friend_left: return "friend_left";
    case Note::friend_quit: return "friend_quit";
    case Note::friends: return "friends";
    default: return "none";
    }
}
inline Note parse_note(const char* text) {
    if (!text) return Note::none;
    for (Note n : {Note::join_failed, Note::friend_failed, Note::friend_stayed, Note::friend_left, Note::friend_quit,
                   Note::friends})
        if (!strcmp(text, note_name(n))) return n;
    return Note::none;
}

// The engine's network error keys: connectlost after 15 s without the host, hostquit when the host
// quits, booted on a kick. The error popup's OK clears them.
enum class EngineError { none, host_quit, lost, other };
// Inside a mission the engine clears these as it records them, so Guest::host_quitting reads the
// host's quit from the engine's flag instead.
inline EngineError engine_error(const char* key) {
    if (!key || !*key) return EngineError::none;
    if (!strcmp(key, "ifs.mp.joinerrors.hostquit")) return EngineError::host_quit;
    if (!strcmp(key, "ifs.mp.joinerrors.connectlost") || !strcmp(key, "ifs.mp.joinerrors.booted"))
        return EngineError::lost;
    return EngineError::other;
}

// The winning-team mask: MissionVictory(team) sets 1 << team, MissionDefeat(team) every bit but the
// loser's. The engine records team 1 as the winner when bit 1 is set, otherwise team 2.
inline int victory_of(uint32_t mask) { return !mask ? -1 : (mask >> 1 & 1) ? 1 : 2; }
// The mask the host sends: its game's own, or one recording the same winner when none is left.
inline uint32_t host_mask(int victory, uint32_t mask) {
    if (mask) return mask;
    return victory == 1 || victory == 2 ? 1u << victory : 0;
}
// In a Versus battle the quitter's side loses, Exit to Windows included. A battle host that closes
// its game concedes with the other team's victory mask, or 0 when its own team is unknown.
inline uint32_t concede_mask(int own_team) { return own_team == 1 || own_team == 2 ? 1u << (3 - own_team) : 0; }

// Follows the host's battle from the script's hosting report to the mission and back to the shell.
// Game thread only: reports come through the FindFirstFileA observer, states from the frame hook.
struct Tracker {
    enum class Phase { idle, hosting, hosted, ended };
    enum class Event { none, hosting, fallback, launched, ended, abandoned, own_quit };
    // The script's popup gives up after 20 s; this is the backstop if its report never comes.
    static constexpr uint64_t launch_limit_ms = 60000;
    Phase phase = Phase::idle;
    Info info;
    uint32_t state_seen = 0;
    uint64_t hosting_since = 0;
    unsigned number = 0;    // counts hosting reports; friends are told of each battle by this number
    unsigned launched = 0;  // hosted battles that got as far as the mission
    // Friends paired at hosting (with_friend) are told with frame F and report with J, by session
    // slot 1 to 3. told: the slots told, bit k for slot k; until known, slot 1.
    static constexpr unsigned max_slots = 3;
    bool with_friend = false;
    unsigned told = 0;
    Part parts[max_slots + 1]{};
    bool note_due = false;
    // Versus: a client that quits the battle loses it, so this game ends the match for its own side
    // once (forfeit_due), unless the match is already over.
    bool versus = false, forfeit_wanted = false, forfeit_done = false;
    // This PC's own player quit the battle (ScriptCB_QuitToShell). A client dropped by that quit has
    // not quit itself: no forfeit, and no note for the leave it reports (dropped, by slot).
    bool own_quit = false, dropped[max_slots + 1]{};
    // A network host's quit waits up to 6 s for every client to answer. Its packets already tell the
    // clients, so a second later the DLL lets the quit go on (hurry_due).
    static constexpr uint64_t hurry_after_ms = 1000;
    uint64_t own_quit_at = 0;
    bool hurried = false;

    // The text after "GCOBS:battle:". Only hosting, quit_own and fallback change anything; the other
    // reports are only logged. paired: the other PC is paired, so the battle is announced to it.
    Event report(const char* text, uint64_t now, bool paired = false, bool in_versus = false) {
        if (!text) return Event::none;
        if (!strncmp(text, "hosting:", 8)) {
            if (!parse_hosting(text + 8, info)) return Event::none;
            phase = Phase::hosting;
            hosting_since = now;
            ++number;
            with_friend = paired;
            told = 0;
            for (unsigned k = 0; k <= max_slots; ++k) {
                parts[k] = Part::none;
                dropped[k] = false;
            }
            note_due = false;
            versus = in_versus;
            forfeit_wanted = forfeit_done = own_quit = hurried = false;
            own_quit_at = 0;
            return Event::hosting;
        }
        if (!strcmp(text, "quit_own") && phase == Phase::hosted && !own_quit) {
            own_quit = true;
            own_quit_at = now;
            return Event::own_quit;
        }
        // The script closed the network game and started the single-player battle instead.
        if (!strncmp(text, "fallback:", 9) && phase == Phase::hosting) {
            phase = Phase::idle;
            return Event::fallback;
        }
        return Event::none;
    }
    // Each frame's engine state hash (0 when unread) and the time.
    Event state(uint32_t hash, uint64_t now) {
        if (phase == Phase::hosting && now - hosting_since > launch_limit_ms) {
            phase = Phase::idle;
            return Event::abandoned;
        }
        if (!hash || hash == state_seen) return Event::none;
        state_seen = hash;
        if (hash == state_mission && phase == Phase::hosting) {
            phase = Phase::hosted;
            ++launched;
            return Event::launched;
        }
        if (hash == state_shell && phase == Phase::hosted) {
            phase = Phase::ended;
            note_due = with_friend;
            return Event::ended;
        }
        return Event::none;
    }
    // After a hosted battle the shell still has the network game open; the stock menus close it when
    // the player goes to Single Player (ifs_main).
    bool cleanup_due() const { return phase == Phase::ended; }
    void cleaned() { phase = Phase::idle; }
    // From the hosting report until the network game closes, the LAN sockets carry only this battle.
    bool sealed() const { return phase != Phase::idle; }
    // A friend's report (frame J) on battle n, by slot; the latest wins. A Versus client's quit while
    // the battle runs asks for the forfeit, unless this PC's own player quit first.
    void friend_report(unsigned slot, unsigned n, Part p) {
        if (!slot || slot > max_slots || !n || n != number || !with_friend || p == Part::none) return;
        parts[slot] = p;
        dropped[slot] = own_quit && (p == Part::left || p == Part::quit);
        if (p == Part::quit && versus && phase == Phase::hosted && !own_quit) forfeit_wanted = true;
    }
    void friend_report(unsigned n, Part p) { friend_report(1, n, p); }
    // The slots told of battle n, bit k for slot k.
    void friends_told(unsigned n, unsigned slots) {
        if (n && n == number && with_friend) told = slots & 0xE;
    }
    // The slots this battle's note covers: those told, else slot 1 when a friend is paired.
    unsigned note_slots() const { return told ? told : with_friend ? 2u : 0u; }
    // Once, while the mission runs: ends the match for this game's own side because the other PC's
    // player quit. Never after this PC's own quit.
    bool forfeit_due() {
        if (!forfeit_wanted || forfeit_done || own_quit || phase != Phase::hosted) return false;
        forfeit_done = true;
        return true;
    }
    // Once, while the mission runs, hurry_after_ms after this PC's own quit.
    bool hurry_due(uint64_t now) {
        if (!own_quit || hurried || phase != Phase::hosted || now - own_quit_at < hurry_after_ms) return false;
        hurried = true;
        return true;
    }
    // The note for the host's galaxy once the network game closes: none when everyone fought, else
    // the one friend's note, or `friends` for several. No word from a friend counts as not joining.
    Note note() const {
        if (!note_due || phase != Phase::idle) return Note::none;
        const unsigned slots = note_slots();
        unsigned count = 0, only = 0;
        for (unsigned k = 1; k <= max_slots; ++k)
            if (slots & (1u << k)) {
                ++count;
                only = k;
            }
        if (count == 1) return slot_note(only);
        for (unsigned k = 1; k <= max_slots; ++k)
            if ((slots & (1u << k)) && slot_note(k) != Note::none) return Note::friends;
        return Note::none;
    }
    // What the `friends` note says of one slot: none when that friend fought.
    Note friend_note(unsigned slot) const {
        if (note() != Note::friends || !slot || slot > max_slots || !(note_slots() & (1u << slot))) return Note::none;
        return slot_note(slot);
    }
    void noted() { note_due = false; }

private:
    // One friend's note; none for a leave caused by this PC's own quit.
    Note slot_note(unsigned slot) const {
        if (dropped[slot]) return Note::none;
        switch (parts[slot]) {
        case Part::joined: return Note::none;
        case Part::left: return Note::friend_left;
        case Part::quit: return versus ? Note::friend_quit : Note::friend_left;
        case Part::stayed: return Note::friend_stayed;
        default: return Note::friend_failed;
        }
    }
};

// The friend's side of a shared battle. The host's session announces each battle (frame F); the
// friend's game joins it from the host's galaxy, fights and returns there. Game thread only.
struct Guest {
    enum class Phase { idle, offered, joining, battle, returning, closing };
    enum class Event {
        none,
        offered,
        ignored,
        expired,
        joining,
        found,
        gave_up,
        refused,
        launched,
        ended,
        abandoned,
        own_quit
    };
    // How soon after an announcement the game must start joining, from the host's galaxy.
    static constexpr uint64_t offer_limit_ms = 30000;
    // Backstop for the join popup, which gives up after 150 s. The host's game answers "try again"
    // until its own map has loaded, which can take more than a minute.
    static constexpr uint64_t join_limit_ms = 180000;
    // A game still in the battle this long after the host's battle ends or the host goes leaves it,
    // as the pause menu's Quit does; its scoreboard would otherwise wait for a host that is gone.
    static constexpr uint64_t quit_after_ms = 1000;
    static constexpr uint64_t quit_after_match_ms = 3000;  // after this game's own match ends
    // The host's match result (frame V). A client's engine ends its match when the mask arrives with
    // the host's own messages; if that has not happened this long after V, the DLL hands it over.
    static constexpr uint64_t result_wait_ms = 1500;
    // The join script's text for Back on the joining popup (player_menu.lua close_join).
    static constexpr const char* join_back = "the friend stayed in the galaxy";
    Phase phase = Phase::idle;
    Info info;
    // Battle numbers count within `session`, this PC's count of sessions opened. ignored: the newest
    // announcement turned down, which stays the host's newest until taken or replaced.
    unsigned session = 0, number = 0, joined = 0, ignored = 0;
    uint64_t since = 0, over_at = 0, match_over_at = 0;
    uint32_t state_seen = 0;
    bool quit_sent = false;
    // The join found the host's session; from then on the game thread updates the lobby every
    // frame, as the stock lobby screen does.
    bool found = false;
    // gone_at: the host's session ended. lost_at: the engine lost the host in battle (timeout, kick
    // or host_quit). back: the join gave up on Back. note_due: a "could not join" note is pending.
    uint64_t gone_at = 0, lost_at = 0;
    bool host_quit = false, back = false, note_due = false;
    // The host's result for this battle (frame V).
    unsigned result_number = 0;
    uint32_t result_mask = 0;
    uint64_t result_at = 0;
    bool result_done = false;
    // This PC's own battle team, as the game thread reads it in the battle: 1 or 2, 0 until known.
    int own_team = 0;
    // This PC's own player quit the battle (ScriptCB_QuitToShell). Leaving early without it is the
    // engine's doing: the host quit or the connection dropped.
    bool own_quit = false;
    // When the engine hears the host quit. Its own quit skips the Remaster's ScriptCB_QuitToShell,
    // which restores settings in the galaxy save, so quit_due runs that Quit at once instead.
    uint64_t quitting_at = 0;
    // The DLL's leave waits for the host's answer or 3 s. A quarter second in, once a few packets
    // have carried the quit, the DLL lets it go on (hurry_due).
    static constexpr uint64_t hurry_after_ms = 250;
    uint64_t quit_at = 0;
    bool hurried = false;

    // A battle the host announced, taken only when idle, so a busy game takes it later if no newer one
    // comes. Turned down once per announcement (ignored); taken after the host has gone, it expires.
    Event offer(unsigned n, const Info& i, uint64_t now) {
        if (!n || n == number) return Event::none;
        if (phase != Phase::idle || !i.valid()) {
            if (n == ignored) return Event::none;
            ignored = n;
            return Event::ignored;
        }
        number = n;
        info = i;
        phase = Phase::offered;
        since = now;
        over_at = 0;
        match_over_at = 0;
        quit_sent = false;
        found = false;
        lost_at = 0;
        host_quit = back = note_due = false;
        result_number = 0;
        result_mask = 0;
        result_at = 0;
        result_done = false;
        own_team = 0;
        own_quit = false;
        quitting_at = quit_at = 0;
        hurried = false;
        return Event::offered;
    }
    // This PC opened session s, whose host numbers its battles from 1. Taken only when idle, so a
    // running battle keeps its number; true when taken. The new session's host is present.
    bool renumber(unsigned s) {
        if (s == session || phase != Phase::idle) return false;
        session = s;
        number = ignored = 0;
        gone_at = 0;
        return true;
    }
    // The host's battle with this number is over (frame E).
    void host_over(unsigned n, uint64_t now) {
        if (n == number && !over_at) over_at = now;
    }
    // The host's game recorded this battle's winner (frame V). Taken once, in the battle; a result
    // that arrives while loading is taken on the first battle frame, and the wait starts there.
    void host_result(unsigned n, uint32_t mask, uint64_t now) {
        if (phase != Phase::battle || !n || n != number || !mask || result_number == n) return;
        result_number = n;
        result_mask = mask;
        result_at = now;
        result_done = false;
    }
    // The mask to hand the engine now, or 0. Once per battle, when the host's result is in and this
    // game has not ended its match within result_wait_ms; `ended`: it has a winner or mask already.
    uint32_t result_due(uint64_t now, bool ended) {
        if (phase != Phase::battle || result_number != number || !result_mask || result_done) return 0;
        if (ended) {
            result_done = true;
            return 0;
        }
        if (now - result_at < result_wait_ms) return 0;
        result_done = true;
        return result_mask;
    }
    // The host's session ended: it left the co-op game, closed or lost its game, or left Steam. Until
    // the next session an announcement expires, a join gives up and a battle is left, with no note.
    void host_gone(uint64_t now) {
        if (!gone_at) gone_at = now;
        note_due = false;
    }
    // The engine lost the host in the battle (EngineError lost or host_quit). Rather than wait for the
    // error popup's OK, the game leaves the battle.
    void engine_lost(bool quit, uint64_t now) {
        if (phase == Phase::battle && !lost_at) {
            lost_at = now;
            host_quit = quit;
        }
    }
    // The engine's hostquit flag, seen in the battle. Taken once, unless a Quit is already under way,
    // by this PC's player or the DLL's leave. True when taken.
    bool host_quitting(uint64_t now) {
        if (phase != Phase::battle || quitting_at || own_quit || quit_sent) return false;
        quitting_at = now;
        host_quit = true;
        return true;
    }
    // Called as the game thread starts the join script.
    void joining(uint64_t now) {
        if (phase == Phase::offered) {
            phase = Phase::joining;
            since = now;
        }
    }
    // The join script's reports after "GCOBS:battle:": found (the session), joinfail:<why> (it gave
    // up and closed the network game), and quit_own in the battle (this PC's player quit).
    Event report(const char* text) {
        if (!text) return Event::none;
        if (!strcmp(text, "quit_own")) {
            if (phase != Phase::battle || own_quit || quit_sent) return Event::none;
            own_quit = true;
            return Event::own_quit;
        }
        if (phase != Phase::joining) return Event::none;
        if (!strncmp(text, "joinfail:", 9)) {
            phase = Phase::idle;
            back = !strcmp(text + 9, join_back);
            note_due = !back && !gone_at;
            return Event::gave_up;
        }
        if (!strcmp(text, "found") && !found) {
            found = true;
            return Event::found;
        }
        return Event::none;
    }
    // The game thread could not start the join: no script, no name buffer, or the script refused.
    Event refuse() {
        if (phase != Phase::offered && phase != Phase::joining) return Event::none;
        phase = Phase::idle;
        note_due = !gone_at;
        return Event::refused;
    }
    // Each frame's engine state hash (0 when unread) and the time. A join that neither launches nor
    // reports in time is left for the game thread to close (closing).
    Event state(uint32_t hash, uint64_t now) {
        if (phase == Phase::offered && (now - since > offer_limit_ms || over_at || gone_at)) {
            phase = Phase::idle;
            note_due = !gone_at;
            return Event::expired;
        }
        if (phase == Phase::joining && now - since > join_limit_ms) {
            phase = Phase::closing;
            note_due = !gone_at;
            return Event::abandoned;
        }
        if (!hash || hash == state_seen) return Event::none;
        state_seen = hash;
        if (hash == state_mission && phase == Phase::joining) {
            phase = Phase::battle;
            since = now;
            ++joined;
            return Event::launched;
        }
        if (hash == state_shell && phase == Phase::battle) {
            phase = Phase::returning;
            since = now;
            return Event::ended;
        }
        return Event::none;
    }
    // Asked when the shell builds after the battle, before the frame hook has seen the new state: the
    // galaxy is about to load this PC's own save.
    bool returning_now(uint32_t hash) const {
        return phase == Phase::returning || (phase == Phase::battle && hash == state_shell);
    }
    // With the host gone after its result (V) but before the battle's end (E), this galaxy keeps that
    // result: 1 if this PC's team won, 2 if the other did, 0 when there is nothing to keep.
    int kept_winner() const {
        if (!gone_at || over_at || !result_number || result_number != number || !result_mask) return 0;
        if (own_team != 1 && own_team != 2) return 0;
        return victory_of(result_mask) == own_team ? 1 : 2;
    }
    // The engine recorded a winner: this game's own match is over.
    void match_over(uint64_t now) {
        if (phase == Phase::battle && !match_over_at) match_over_at = now;
    }
    // Once, when it is time to leave: the host's battle is over, the host is gone or lost, or this
    // game's match ended, each after a short delay; at once when the host is quitting.
    bool quit_due(uint64_t now) {
        if (phase != Phase::battle || quit_sent) return false;
        const bool host_done = over_at && now - over_at >= quit_after_ms;
        const bool gone_done = gone_at && now - gone_at >= quit_after_ms;
        const bool lost_done = lost_at && now - lost_at >= quit_after_ms;
        const bool match_done = match_over_at && now - match_over_at >= quit_after_match_ms;
        if (!host_done && !gone_done && !lost_done && !match_done && !quitting_at) return false;
        quit_sent = true;
        quit_at = now;
        return true;
    }
    // Once, while still in the battle, hurry_after_ms after the DLL's own leave.
    bool hurry_due(uint64_t now) {
        if (phase != Phase::battle || !quit_sent || hurried || now - quit_at < hurry_after_ms) return false;
        hurried = true;
        return true;
    }
    // At the battle's end: the game left before the host's battle or its own match ended, and not
    // because the host had gone or quit as far as the DLL knows.
    bool left_early() const { return !over_at && !match_over_at && !gone_at && !host_quit; }
    // Of those, the player's own Quit: not the DLL's leave and not a lost host.
    bool quit_early() const { return left_early() && own_quit && !quit_sent && !lost_at; }
    // What the host is told (frame J) at each event; none when there is nothing to tell or no host.
    Part part(Event e) const {
        if (gone_at) return Part::none;
        switch (e) {
        case Event::launched: return Part::joined;
        case Event::gave_up: return back ? Part::stayed : Part::failed;
        case Event::expired:
        case Event::refused:
        case Event::abandoned: return Part::failed;
        case Event::ended: return quit_early() ? Part::quit : left_early() ? Part::left : Part::none;
        default: return Part::none;
        }
    }
    // The friend's own "could not join" note, shown while the co-op game goes on.
    bool note() const { return note_due && !gone_at; }
    void noted() { note_due = false; }
    void returned() {
        if (phase == Phase::returning) phase = Phase::idle;
    }
    // The stock lobby update (UpdateLobby, LaunchLobby), run each frame from finding the session until
    // the mission loads, unless the host's battle is over or the host has gone.
    bool lobby_due() const { return phase == Phase::joining && found && !over_at && !gone_at; }
    // The joining popup closes: the host's battle is over or the host has gone.
    bool join_off() const { return phase == Phase::joining && (over_at || gone_at); }
    // An abandoned join's network game is still open and needs closing; closed() then idles.
    bool close_due() const { return phase == Phase::closing; }
    void closed() {
        if (phase == Phase::closing) phase = Phase::idle;
    }
    // While busy, the apply loop waits and the co-op game stays open.
    bool busy() const { return phase != Phase::idle; }
    // The LAN sockets carry this battle from the join until the network game closes.
    bool sealed() const {
        return phase == Phase::joining || phase == Phase::battle || phase == Phase::returning ||
               phase == Phase::closing;
    }
};

// The side select button to press: the team whose faction is the galaxy side's. Auto is only the
// fallback, since on the friend's PC, without metagame rules, it balances the teams instead.
inline const char* side_button(int team1_faction, int team2_faction, int side_faction) {
    if (side_faction > 0 && side_faction == team1_faction) return "team1";
    if (side_faction > 0 && side_faction == team2_faction) return "team2";
    return "auto";
}
// The battle team of the galaxy side, 1 or 2, or 0 when neither matches. A Versus battle host ends a
// forfeited match for this team (MissionVictory).
inline int side_team(int team1_faction, int team2_faction, int side_faction) {
    if (side_faction > 0 && side_faction == team1_faction) return 1;
    if (side_faction > 0 && side_faction == team2_faction) return 2;
    return 0;
}
// The team the AI counts as the player's (bots_hook.h); none in a network game. A Versus battle host
// counts its own, so the AI auto-balance runs and gives that team the enemy's skill.
inline int ai_player_team(bool versus, bool hosting, int own_team) {
    return versus && hosting && (own_team == 1 || own_team == 2) ? own_team : 0;
}
// The allies' skill offset that matches the enemy's skill at the same difficulty.
inline int even_ally_offset(int enemy_offset, int enemy_base, int ally_base) {
    return enemy_offset + enemy_base - ally_base;
}
// In a network game every character waits for its team's next spawn wave, 15 s apart. A human who
// asked to spawn (+0x164) gets its own wave (+0x15C) set back to the current one and spawns now.
inline bool spawn_now(int team_wave, int own_wave, bool requested) {
    return requested && team_wave >= 0 && own_wave > team_wave;
}
// The character table (01E30334, count at 01E30330) holds every net slot and each team's units up to
// its cap, so a big map fills 64 or more. It is read whole, up to this many.
constexpr int max_characters = 256;
inline bool character_count_ok(int count) { return count >= 1 && count <= max_characters; }
// The spawn manager starts a team's next wave every delay (+0x5C) and spawns it once its window
// (+0x7C) passes. The host caps the delay at the engine's 0.5 s warm-up rate and the window below it.
constexpr float quick_wave_s = 0.5f, quick_window_s = 0.25f;
inline float quicker(float value, float limit) { return value > limit ? limit : value; }
// The auto-turret bonus builds turrets at command posts. A network client's turret pool limit stays 0,
// so the friend's game raises it to the post count, up to the 63 objects one network game can send.
inline unsigned turret_limit(unsigned limit, int command_posts) {
    if (command_posts <= 0 || command_posts > 63 || static_cast<unsigned>(command_posts) <= limit) return limit;
    return static_cast<unsigned>(command_posts);
}
// Enemy markers and lock-on need the targeting controller to hold the player's character table entry.
// It links that entry only at mission start, and only if the entry's own slot (+0x130) is the player's.
inline bool character_entry_ok(int slot, int count, int entry_slot) {
    return slot >= 0 && slot < count && entry_slot == slot;
}
// The DLL links the entry itself once it is the player's, whenever the controller holds another or none.
inline bool aim_relink_due(uint32_t controller, uint32_t linked, uint32_t expected) {
    return controller && expected && linked != expected;
}
// What a character controls, in the targeting update's order: a vehicle seat (+0x150), a vehicle it
// drives (+0x14C), then its soldier (+0x148, none while it waits to spawn).
enum class Seat { none, soldier, vehicle, seat };
inline Seat seat_of(uint32_t soldier, uint32_t vehicle, uint32_t seat) {
    return seat ? Seat::seat : vehicle ? Seat::vehicle : soldier ? Seat::soldier : Seat::none;
}
inline const char* seat_name(Seat s) {
    switch (s) {
    case Seat::soldier: return "soldier";
    case Seat::vehicle: return "vehicle";
    case Seat::seat: return "seat";
    default: return "none";
    }
}
// Each army's AI counted by what they control, for the BATTLE_AI log line on both PCs. In space,
// pilots without a vehicle stand in a hangar or wait to spawn.
struct Army {
    unsigned ai = 0, vehicle = 0, seat = 0, soldier = 0, waiting = 0, humans = 0;
    void add(bool human, Seat s) {
        if (human) {
            ++humans;
            return;
        }
        ++ai;
        switch (s) {
        case Seat::vehicle: ++vehicle; break;
        case Seat::seat: ++seat; break;
        case Seat::soldier: ++soldier; break;
        default: ++waiting; break;
        }
    }
};

// The character table as one letter each, to compare teams across the PCs' logs: a player's character
// (own slot equals its index) is 'A' or 'B' by team, an AI's (own slot -1) 'a' or 'b'.
struct Layout {
    std::string letters;
    unsigned humans = 0, team1 = 0, team2 = 0;
    void add(int index, int own, int team) {
        const bool human = own == index;
        char c = '.';
        if ((human || own == -1) && (team == 1 || team == 2)) {
            c = static_cast<char>((team == 1 ? 'a' : 'b') - (human ? 'a' - 'A' : 0));
            humans += human ? 1 : 0;
            (team == 1 ? team1 : team2) += 1;
        }
        letters += c;
    }
};

// The automatic side choice, tried at most max_tries times per battle.
struct SideChoice {
    static constexpr unsigned max_tries = 8;
    unsigned tries = 0;
    bool done = false;
    void reset() {
        tries = 0;
        done = false;
    }
    bool due() const { return !done && tries < max_tries; }
};

// The game's three LAN sockets: the game socket, the discovery broadcast (connected to
// 255.255.255.255:3656) and the discovery listener on 3656.
enum class Socket { other, game, broadcast, listener };
inline Socket classify(uintptr_t s, uintptr_t game, uintptr_t broadcast, uintptr_t listener) {
    // Unset handles are 0 or INVALID_SOCKET (all bits set), which never match.
    if (!s || s == ~uintptr_t(0)) return Socket::other;
    if (s == game) return Socket::game;
    if (s == broadcast) return Socket::broadcast;
    if (s == listener) return Socket::listener;
    return Socket::other;
}

// Tunnel frames on Steam P2P channel 9 (the session uses 8): 'T', destination, source, the sender's
// port (LE u16, 0 if unknown), then the packet. Game packets fit one unreliable Steam send.
constexpr int tunnel_channel = 9;
constexpr size_t tunnel_header = 5, tunnel_max_packet = 2048;
enum class To : char { game = 'G', listener = 'L' };
enum class From : char { game = 'G', broadcast = 'B' };
inline std::string encode_tunnel(To to, From from, uint16_t port, const char* data, size_t n) {
    std::string f(tunnel_header + n, '\0');
    f[0] = 'T';
    f[1] = static_cast<char>(to);
    f[2] = static_cast<char>(from);
    f[3] = static_cast<char>(port & 0xFF);
    f[4] = static_cast<char>(port >> 8);
    if (n) memcpy(&f[tunnel_header], data, n);
    return f;
}
// Checks a frame's header; the packet follows it at tunnel_header.
inline bool decode_tunnel(const char* p, size_t n, To& to, From& from, uint16_t& port) {
    if (n <= tunnel_header || n > tunnel_header + tunnel_max_packet || p[0] != 'T') return false;
    if (p[1] != 'G' && p[1] != 'L') return false;
    if (p[2] != 'G' && p[2] != 'B') return false;
    to = static_cast<To>(p[1]);
    from = static_cast<From>(p[2]);
    port = static_cast<uint16_t>(static_cast<unsigned char>(p[3]) | (static_cast<unsigned char>(p[4]) << 8));
    return true;
}

// Made-up peer addresses in host byte order. A client's game sees only its battle host, 10.71.0.1;
// the battle host sees the client in session slot k at 10.71.0.1 + k.
constexpr uint32_t host_address = 0x0A470001, friend_address = 0x0A470002, any_address = 0xFFFFFFFF;
constexpr unsigned max_peers = 3;
// Broadcasts appear to come from 3657 and game sockets from 3658 (host) or 3659 (client), the ports
// the game binds first.
constexpr uint16_t listener_port = 3656, broadcast_port = 3657, host_game_port = 3658, client_game_port = 3659;
struct Address {
    uint32_t ip = 0;
    uint16_t port = 0;
};
inline uint32_t client_address(unsigned slot) { return slot >= 1 && slot <= max_peers ? host_address + slot : 0; }
// The session slot of a client address, or 0.
inline unsigned client_slot(uint32_t ip) {
    return ip > host_address && ip <= host_address + max_peers ? static_cast<unsigned>(ip - host_address) : 0;
}
// Where the battle host's game sends a packet for `ip`: all_peers for a broadcast, the client at
// that address if in `peers` (bit k for slot k), else the only client or no_peer, flagged stray.
constexpr unsigned all_peers = 0, no_peer = ~0u;
inline unsigned destination_slot(uint32_t ip, unsigned peers, bool& stray) {
    stray = false;
    if (ip == any_address) return all_peers;
    const unsigned slot = client_slot(ip);
    if (slot && (peers & (1u << slot))) return slot;
    stray = true;
    unsigned only = 0, count = 0;
    for (unsigned k = 1; k <= max_peers; ++k)
        if (peers & (1u << k)) {
            only = k;
            ++count;
        }
    return count == 1 ? only : no_peer;
}
// The peer socket a sent packet lands on: the listener for the discovery port, else the game socket.
inline To route(uint16_t destination_port) { return destination_port == listener_port ? To::listener : To::game; }
// The game identifies a sender by IP and port, so each peer must keep one address. Once the local game
// has sent to a peer, that peer's game packets appear to come from exactly where it sent them.
struct Mirror {
    Address peer;
    bool set = false;
    void sent(uint32_t ip, uint16_t port, uint32_t peer_ip) {
        if (port == listener_port || (ip == peer_ip && port == broadcast_port)) return;
        peer = {ip, port};
        set = true;
    }
    Address source(From from, uint16_t port, uint32_t peer_ip, uint16_t peer_game_port) const {
        if (from == From::broadcast) return {peer_ip, broadcast_port};
        if (set) return peer;
        return {peer_ip, port ? port : peer_game_port};
    }
    void clear() { *this = Mirror{}; }
};
}
