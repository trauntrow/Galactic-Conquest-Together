#pragma once
// Detour for the engine's player-team test, on which each team's AI skill depends.
#include <atomic>
#include <cstdint>

namespace gc::bots {
// __fastcall with the team in ECX; the game's callers read the answer from AL.
using PlayerTeamFn = uint32_t(__fastcall*)(uint32_t);
inline PlayerTeamFn original = nullptr;
// The team to count as the player's, or 0 to leave the answer to the game.
inline std::atomic<int> chosen{0};
// lua_query.cpp installs this with MinHook and sets `chosen` in the Versus battles it hosts.
inline uint32_t __fastcall player_team(uint32_t team) {
    const int c = chosen.load(std::memory_order_relaxed);
    if (!c) return original(team);
    return team == static_cast<uint32_t>(c) ? 1u : 0u;
}
}
