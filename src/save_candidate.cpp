// Builds and encodes the save arguments the friend's game applies, from a host snapshot.
#include "save_candidate.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace gc::candidate {
namespace {
using capture::Data;
using capture::Entry;
using capture::Key;
using capture::Probed;
using capture::Row;
using capture::Value;

// Building stops at the first problem, thrown with its reason.
struct Refuse : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void refuse(const std::string& reason) { throw Refuse(reason); }

// A Lua value before encoding.
struct Node {
    enum Type : unsigned char { Nil, Boolean, Number, String, Table } type = Nil;
    bool flag = false;
    int number = 0;
    std::string text;
    std::vector<std::pair<Node, Node>> entries;
};

// Save arguments in order, by snapshot field. nullptr is argument 3, the profile table, which
// carries the host's hints.
constexpr const char* FIELDS[] = {"custom",
                                  "scenario",
                                  nullptr,
                                  "lastSelected",
                                  "lastFleet",
                                  "planetTeam",
                                  "planetFleet",
                                  "planetNext",
                                  "teamResources",
                                  "global.ifs_purchase_unit_owned",
                                  "global.ifs_purchase_tech_cards",
                                  "global.ifs_purchase_tech_using",
                                  "playerTeam",
                                  "turnNumber",
                                  "curScreen",
                                  "fleet.turnNumber",
                                  "fleet.planetStart",
                                  "fleet.planetNext",
                                  "launchMission",
                                  "activeBonus",
                                  "winnerTeam",
                                  "fleetBattle",
                                  "recentPlanets",
                                  "planetResources",
                                  "battleResources",
                                  "soakMode"};
// The host's own screens and battle result, sent as nil so the friend's game opens on the galaxy.
constexpr const char* GUEST_NIL[] = {"curScreen", "winnerTeam", "fleetBattle"};

Node primitive(const Value& v) {
    Node n;
    switch (v.kind) {
    case Value::Bool:
        n.type = Node::Boolean;
        n.flag = v.flag;
        break;
    case Value::Int:
        n.type = Node::Number;
        n.number = v.number;
        break;
    case Value::Text:
        n.type = Node::String;
        n.text = v.text;
        break;
    default: break;
    }
    return n;
}

Node key(const Key& k) {
    Node n;
    if (k.integer) {
        n.type = Node::Number;
        n.number = k.number;
    } else {
        n.type = Node::String;
        n.text = k.text;
    }
    return n;
}

Node table(const std::vector<Row>& rows) {
    Node n;
    n.type = Node::Table;
    for (const Row& r : rows) n.entries.emplace_back(key(r.key), primitive(r.value));
    return n;
}

// A probed map. Unsupported entries and nested table summaries leave the capture incomplete.
Node table(const std::vector<Entry>& entries, const std::string& field) {
    Node n;
    n.type = Node::Table;
    for (const Entry& e : entries) {
        if (e.kind == Entry::Plain) n.entries.emplace_back(key(e.key), primitive(e.value));
        else if (e.kind == Entry::Nested) n.entries.emplace_back(key(e.key), table(e.entries));
        else refuse(field + ": incomplete table contents");
    }
    return n;
}

// The Lua value of a snapshot field. A missing, unsupported or failed field refuses the snapshot.
Node captured(const Data& data, const std::string& field) {
    if (const auto s = data.scalars.find(field); s != data.scalars.end()) return primitive(s->second);
    if (const auto m = data.maps.find(field); m != data.maps.end()) return table(m->second);
    const auto p = data.probed.find(field);
    if (p == data.probed.end()) refuse(field + ": capture is incomplete (missing)");
    const Probed& row = p->second;
    if (row.kind == Probed::Scalar) return primitive(row.value);
    if (row.kind == Probed::Map) return table(row.entries, field);
    refuse(field + ": capture is incomplete (" + (row.kind == Probed::Error ? "error" : "unsupported") + ")");
}

Node number(int value) {
    Node n;
    n.type = Node::Number;
    n.number = value;
    return n;
}

// A probed field that must be present.
const Probed& probed(const Data& data, const std::string& field) {
    const auto p = data.probed.find(field);
    if (p == data.probed.end()) refuse(field + ": capture is incomplete (missing)");
    return p->second;
}

// Argument 3, which the friend's script replaces with its own profile table: [1] the host's human
// team (playerTeam is the AI's during its turn), from producer 7 [2] the camera planet and [3] zoom.
Node host_hints(const Data& data, int producer) {
    const auto s = data.scalars.find("playerTeam");
    if (s == data.scalars.end()) refuse("playerTeam: capture is incomplete (missing)");
    const Value& player = s->second;
    const Probed& row = probed(data, "controllerTeam");
    bool known = player.kind == Value::Int && (player.number == 1 || player.number == 2) && row.kind == Probed::Map &&
                 !row.entries.empty();
    bool plays = false;
    for (const Entry& e : row.entries) {
        if (e.kind != Entry::Plain || e.value.kind != Value::Int || (e.value.number != 1 && e.value.number != 2))
            known = false;
        else if (e.value.number == player.number) plays = true;
    }
    if (!known) refuse("the host's human team is unknown");
    Node hints;
    hints.type = Node::Table;
    hints.entries.emplace_back(number(1), number(plays ? player.number : 3 - player.number));
    if (producer == 7) {
        const Probed& camera = probed(data, "planetSelected");
        if (camera.kind == Probed::Scalar && camera.value.kind == Value::Text)
            hints.entries.emplace_back(number(2), primitive(camera.value));
        const Probed& zoom = probed(data, "fleet.zoomLevel");
        if (zoom.kind == Probed::Scalar && zoom.value.kind == Value::Int)
            hints.entries.emplace_back(number(3), primitive(zoom.value));
    }
    return hints;
}

// Type-tagged encoding: 0 nil, 1 boolean, 3 float, 4 NUL-terminated string, 5 table of key-value
// pairs ended by 0. Node count, depth and size are bounded.
struct Encoder {
    std::string out;
    unsigned nodes = 0;
    void write(const Node& n, unsigned depth = 0) {
        if (++nodes > 4096 || depth > 16) refuse("save structure exceeds bound");
        switch (n.type) {
        case Node::Nil: out += '\0'; break;
        case Node::Boolean:
            out += '\1';
            out += static_cast<char>(n.flag);
            break;
        case Node::Number: {
            const float f = static_cast<float>(n.number);
            char bytes[4];
            memcpy(bytes, &f, 4);
            out += '\3';
            out.append(bytes, 4);
            break;
        }
        case Node::String:
            if (n.text.size() > 4096 || n.text.find('\0') != std::string::npos) refuse("NUL or excessive string");
            out += '\4';
            out += n.text;
            out += '\0';
            break;
        case Node::Table:
            out += '\5';
            for (const auto& [k, v] : n.entries) {
                write(k, depth + 1);
                if (v.type == Node::Nil) refuse("invalid or nil table entry");
                write(v, depth + 1);
            }
            out += '\0';
            break;
        }
        if (out.size() > 65536) refuse("raw save exceeds native scratch space");
    }
};

// The most entries any table in n has.
size_t widest(const Node& n) {
    size_t most = n.type == Node::Table ? n.entries.size() : 0;
    for (const auto& e : n.entries) {
        const size_t w = widest(e.second);
        if (w > most) most = w;
    }
    return most;
}
}

// Encodes all 26 values in argument order, then checks them against the apply script's limits.
std::string guest_wire(const Data& data, int producer, std::string& wire) {
    try {
        if (producer != 6 && producer != 7) refuse("requires producer 6 or 7");  // 7 also has the camera fields
        if (captured(data, "launchMission").type != Node::Nil)
            refuse("host is launching a battle; waiting for its return");
        const Node soak = captured(data, "soakMode");
        if (soak.type == Node::Boolean && soak.flag) refuse("soak mode would fabricate a battle result");
        std::vector<Node> values;
        for (const char* field : FIELDS) {
            Node n;
            if (!field) n = host_hints(data, producer);
            else if (std::find_if(std::begin(GUEST_NIL), std::end(GUEST_NIL),
                                  [&](const char* f) { return !strcmp(f, field); }) == std::end(GUEST_NIL))
                n = captured(data, field);
            values.push_back(std::move(n));
        }
        Encoder encoder;
        encoder.out += static_cast<char>(values.size());
        for (const Node& v : values) encoder.write(v);
        if (encoder.out.size() > 4096) refuse("candidate exceeds the 4096-byte apply bound");
        for (const Node& v : values)
            if (widest(v) > 128) refuse("a table exceeds 128 entries");
        wire = std::move(encoder.out);
        return {};
    } catch (const Refuse& error) { return error.what(); }
}
}
