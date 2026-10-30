#pragma once
// RearmGate: when the Lua query may be installed again after a shell rebuild.
#include <cstdint>

namespace gc {
// Allows one attempt per shell rebuild, after 120 stable guarded frames. The context address is
// only a filter, never an identity: each attempt still runs the full stack and Lua screen guards.
struct RearmGate {
    enum Decision { wait, execute, context_mismatch, attempt_limit };
#if GC_PLAYER
    // The Player re-arms after every battle. With the first install this fills the capture
    // validator's 64 generations.
    static constexpr unsigned max_attempts = 63;
#else
    static constexpr unsigned max_attempts = 4;
#endif
    unsigned generation = 0, stable = 0, attempts = 0;
    uint32_t installed_context = 0, installed_thread = 0;
    uint32_t previous_context = 0, previous_thread = 0, previous_flow = 0;
    bool enabled = false, settled = true;

    // Records an install made at shell rebuild `reload`. A failed install disables the gate.
    void arm(unsigned reload, uint32_t context, uint32_t thread, bool success) {
        generation = reload;
        installed_context = context;
        installed_thread = thread;
        enabled = success;
        stable = 0;
        settled = true;  // the next attempt needs a new rebuild
    }
    // Called every frame. Counts frames in the galaxy (flow 3) or main menu (flow 4) on the
    // installing thread; a change of context, thread or flow restarts the count.
    Decision step(unsigned reload, uint32_t context, uint32_t thread, uint32_t flow, bool guards) {
        if (!enabled) return wait;
        if (reload != generation) {
            generation = reload;
            stable = 0;
            settled = false;
        }
        if (settled) return wait;
        if (!guards || thread != installed_thread || (flow != 3 && flow != 4)) {
            stable = 0;
            return wait;
        }
        if (context != previous_context || thread != previous_thread || flow != previous_flow) stable = 0;
        previous_context = context;
        previous_thread = thread;
        previous_flow = flow;
        if (++stable < 120) return wait;
        settled = true;  // this rebuild is spent before any Lua entry, whatever the outcome
        if (context != installed_context) return context_mismatch;
        if (attempts >= max_attempts) return attempt_limit;
        ++attempts;
        return execute;
    }
};
}
