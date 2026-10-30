#pragma once
// Hook on the game's per-frame function: one sample per frame, and a step of the Lua bridge.
#include "diagnostics.h"
#include <cstdint>

namespace gc {
// One frame as the hook saw it: thread, return address, the current Lua state (context) and
// whether its header could be read.
struct FrameSample {
    DWORD thread, caller, context, readable;
    DWORD header[12];  // start of the Lua state: [2] is its stack top, [3] the base
    ULONGLONG tick;
};
// Hooks the frame function after checking its code and its caller. Call after verify_image.
bool install_frame_probe(HMODULE game);
// Worker thread: empties the sample ring and logs what it shows.
void drain_frame_probe();
#if GC_FRAME_TEST
// Test builds: hook any function and read the ring directly.
bool test_install_frame_probe(void* target, DWORD caller, DWORD* context_slot);
bool take_frame_sample(FrameSample& sample);
uint32_t frame_dropped();
#endif
}
