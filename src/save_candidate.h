#pragma once
// Turns a host snapshot into the 26 arguments the friend's game passes to ScriptCB_SaveMetagameState,
// encoded for the Lua script that applies them.
#include "capture_decode.h"
#include <string>

namespace gc::candidate {
// Fills wire (the argument count, then each value) and returns "", or returns why this snapshot
// cannot be applied on the friend's PC.
std::string guest_wire(const capture::Data& normalized, int producer, std::string& wire);
}
