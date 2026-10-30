#pragma once
// Entry point of the Steam layer (steam_invite.cpp).
#include <windows.h>

namespace gc {
// Starts the Steam layer on its own thread. `self` locates the status folder next to this DLL; `game`
// is the module whose steam_api imports get wrapped. False if already running or it cannot start.
bool start_steam_invite(HMODULE self, HMODULE game);
}
