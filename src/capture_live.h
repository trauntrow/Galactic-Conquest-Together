#pragma once
// The newest host snapshot validated in this process, for the Steam session to send.
#include "capture_decode.h"
#include <string>

namespace gc {
// A validated snapshot ready to send. revision counts validated snapshots in this process, so it
// only moves forward, across producer generations too.
struct CaptureOffer {
    unsigned revision = 0;
    int producer = 0, generation = 0, sequence = 0, source_table = 0, update_ticket = 0, unchanged_samples = -1;
    std::string normalized_sha256;  // hex SHA-256 of json
    std::string json;               // canonical JSON of the normalized data
    // Side taking its turn, turn number (in half-turns) and the side this PC plays; 0 when unknown.
    int player_team = 0, turn_number = 0, local_team = 0;
};
// Reads playerTeam and turnNumber, and the one side every controller in controllerTeam plays
// (0 if there is none or they differ).
inline void turn_facts(const capture::Data& data, int& player_team, int& turn_number, int& local_team) {
    const auto number = [&](const char* field) {
        const auto s = data.scalars.find(field);
        return s != data.scalars.end() && s->second.kind == capture::Value::Int ? s->second.number : 0;
    };
    player_team = number("playerTeam");
    turn_number = number("turnNumber");
    local_team = 0;
    const auto p = data.probed.find("controllerTeam");
    if (p == data.probed.end() || p->second.kind != capture::Probed::Map) return;
    int team = 0;
    for (const capture::Entry& e : p->second.entries) {
        const bool side = e.kind == capture::Entry::Plain && e.value.kind == capture::Value::Int &&
                          (e.value.number == 1 || e.value.number == 2);
        if (!side || (team && team != e.value.number)) return;
        team = e.value.number;
    }
    local_team = team;
}
// Copies the newest snapshot into out if its revision is above `after`.
bool capture_latest(unsigned after, CaptureOffer& out);
}
