#pragma once
// RecoveryGate: holds a one-time step until the galaxy has settled on a rebuilt shell.
#include <cstdint>
namespace gc {
// Fires once a shell newer than the armed one shows the galaxy (flow 3) on the armed thread, with
// one Lua context and passing guards, for `frames` frames in a row. Times out 120 s after arming.
struct RecoveryGate {
    bool armed = false, finished = false;
    unsigned thread = 0, generation = 0, context = 0, stable = 0, observed_generation = 0, frames = 120;
    std::uint64_t not_before = 0, deadline = 0;
    enum Decision { wait, execute, timeout };
    // `reload` is the current shell rebuild count. No frame counts before `delay_ms` has passed.
    void arm(unsigned tid, unsigned reload, std::uint64_t now, unsigned delay_ms = 20000, unsigned needed = 120) {
        armed = true;
        thread = tid;
        generation = reload;
        not_before = now + delay_ms;
        deadline = now + 120000;
        frames = needed;
    }
    // Called every frame. A failed check resets the count; a new context or rebuild restarts it.
    Decision step(unsigned tid, unsigned reload, unsigned ctx, unsigned flow, bool guards, std::uint64_t now) {
        if (!armed || finished) return wait;
        if (now >= deadline) {
            finished = true;
            return timeout;
        }
        if (now < not_before || tid != thread || reload <= generation || !ctx || flow != 3 || !guards) {
            stable = 0;
            context = 0;
            return wait;
        }
        if (context != ctx || observed_generation != reload) {
            context = ctx;
            observed_generation = reload;
            stable = 0;
        }
        if (++stable < frames) return wait;
        finished = true;
        return execute;
    }
};
}
