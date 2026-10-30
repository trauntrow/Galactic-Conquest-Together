#pragma once
// The apply loop on a friend's PC, which brings each revision from the host into the running game.
// Pure logic, driven frame by frame with fake actions by tests/apply_test.cpp.
#include "recovery_gate.h"
#include "runtime_commit.h"
#include <cstdint>

namespace gc::apply {
// Engine flow values: 4 the main menu at startup, 0 any later shell menu, 3 the galaxy.
constexpr std::uint32_t menu_flow = 4, shell_flow = 0, galaxy_flow = 3;
inline bool at_menus(std::uint32_t flow) { return flow == menu_flow || flow == shell_flow; }
// A load needs the shell settled; an in-place patch is one protected call on an idle, guarded
// frame, so a few frames of the same context are enough.
constexpr unsigned settle_frames = 120, patch_settle_frames = 6;
// A load from the menus waits at an idle menu while the host holds its galaxy (session_logic.h
// Hold), so it can be quick.
constexpr unsigned first_settle_frames = 15, first_commit_frames = 15;
// Confirmation needs a later shell and confirm_frames stable galaxy frames. A failed check repeats
// every confirm_retry_ms, up to confirm_window_ms after the commit, as the galaxy may be setting up.
constexpr unsigned confirm_frames = 30, confirm_retry_ms = 250;
constexpr unsigned long long confirm_window_ms = 20000;

// What the game thread reads each frame.
struct Frame {
    std::uint32_t thread = 0, context = 0, flow = 0;
    unsigned generation = 0;  // how many times the shell has been rebuilt
    bool guards = false;      // caller, transition, suspend, context and idle Lua stack checks pass
    std::uint64_t now = 0;
};

// Applies the host's newest revision, patching the running galaxy in place when it can and loading
// it through the native loader otherwise. Actions supplies every game call.
template <class Actions> class Loop {
public:
    // A reload settles, prepares, commits once storage has stayed unchanged, then confirms the
    // galaxy on the next shell. A patch needs only a short settle.
    enum Phase { waiting, settling, committing, confirming, stopped };
    Phase phase = waiting;
    unsigned applied = 0, target = 0, passed = 0, cycles = 0, patches = 0, refusals = 0, menu_loads = 0, resumes = 0;
    bool in_galaxy = false;   // the galaxy on screen is one this loop applied
    bool adopted = false;     // in_galaxy because adopt() took this PC's own galaxy
    const char* reason = "";  // why the loop stopped

    // Versus: the session host's own galaxy counts as applied, so the friend's turns are patched into
    // it. It lasts until the menus.
    void adopt() {
        in_galaxy = true;
        adopted = true;
    }

    // The co-op game is over: nothing it sent may load later and an uncommitted cycle is dropped.
    // A stop ends with its game, so a newer game's revisions apply again.
    void drop_pending(unsigned newest) {
        if (newest > passed) passed = newest;
        if (phase == settling || phase == committing) phase = waiting;
        if (phase == stopped) {
            phase = waiting;
            reason = "";
            ++resumes;
        }
    }
    // The applied galaxy may be left between cycles or after a stop, never with a load in flight.
    bool can_leave() const { return in_galaxy && (phase == waiting || phase == settling || phase == stopped); }
    // A committed load not yet confirmed, on its way into the galaxy.
    bool loading() const { return phase == confirming; }

    // Called once per frame. A guarded frame at the menus means the applied galaxy is gone.
    void step(const Frame& f, Actions& a) {
        if (f.guards && at_menus(f.flow)) in_galaxy = adopted = false;
        switch (phase) {
        case stopped: return;
        case waiting:
            if (a.newest() <= (passed > applied ? passed : applied)) return;
            phase = settling;
            stable = 0;
            thread = context = 0;
            [[fallthrough]];
        case settling: {
            // At the menus no story campaign may be loaded; otherwise only the applied galaxy counts.
            const bool menu = at_menus(f.flow);
            if (!f.guards || (menu ? !a.menu_ready() : f.flow != galaxy_flow || !in_galaxy)) {
                stable = 0;
                return;
            }
            if (thread != f.thread || context != f.context || menu != menu_cycle) {
                stable = 0;
                thread = f.thread;
                context = f.context;
                menu_cycle = menu;
            }
            const bool reload = menu || reload_next;
            if (++stable < (menu ? first_settle_frames : reload ? settle_frames : patch_settle_frames)) return;
            if (!reload) {
                unsigned revision = 0;
                // 1: patched in place; 0: declined with nothing changed; -1: failed.
                const int patched = a.patch(revision);
                if (patched < 0) {
                    stop(a, "in-place update failed");
                    return;
                }
                if (patched > 0) {
                    ++cycles;
                    ++patches;
                    applied = target = revision;
                    phase = waiting;
                    return;
                }
                // Declined: settle again and reload this revision instead.
                reload_next = true;
                stable = 0;
                return;
            }
            ++cycles;
            reload_next = false;
            unsigned revision = 0;
            // 1: staged; -1: failed; 0: refused with storage restored, for example on the AI's turn.
            // A refused revision is passed by until a newer one arrives.
            const int prepared = a.prepare(menu, revision);
            if (prepared < 0) {
                stop(a, "preparation failed");
                return;
            }
            if (prepared == 0) {
                ++refusals;
                passed = revision;
                phase = waiting;
                return;
            }
            target = revision;
            commit = RuntimeCommitGate{};
            commit.arm(f.thread, f.context, f.now, menu ? first_commit_frames : 120);
            generation = f.generation;
            phase = committing;
            return;
        }
        case committing: {
            // Commits once the shell, storage and screen have held still long enough. Any change
            // withdraws the staged revision, and the next cycle tries again.
            const bool unchanged = f.guards && f.generation == generation && a.storage_unchanged() &&
                                   (menu_cycle ? at_menus(f.flow) && a.menu_ready() : f.flow == galaxy_flow);
            const auto decision = commit.step(true, unchanged, f.thread, f.context, f.now);
            if (decision == RuntimeCommitGate::wait) return;
            if (decision != RuntimeCommitGate::attempt) {
                a.withdraw(target);
                phase = waiting;
                return;
            }
            if (!a.commit(menu_cycle, target)) {
                stop(a, "commit failed");
                return;
            }
            if (menu_cycle) ++menu_loads;
            commit_generation = f.generation;
            confirm_until = f.now + confirm_window_ms;
            confirm = RecoveryGate{};
            confirm.arm(f.thread, commit_generation, f.now, 0, confirm_frames);
            phase = confirming;
            return;
        }
        case confirming: {
            // Checks the live galaxy once it has settled on a later shell.
            const auto decision = confirm.step(f.thread, f.generation, f.context, f.flow, f.guards, f.now);
            if (decision == RecoveryGate::wait) return;
            if (decision == RecoveryGate::timeout) {
                stop(a, "no stable galaxy after the load");
                return;
            }
            if (!a.confirm(target)) {
                // The loaded galaxy shows even if it does not match, so it stays the loop's to leave.
                if (f.now >= confirm_until) {
                    in_galaxy = true;
                    stop(a, "confirmation failed");
                    return;
                }
                ++confirm_retries;  // the galaxy may still be setting up
                confirm = RecoveryGate{};
                confirm.arm(f.thread, commit_generation, f.now, confirm_retry_ms, confirm_frames);
                return;
            }
            applied = target;
            in_galaxy = true;
            phase = waiting;
            return;
        }
        }
    }

    unsigned confirm_retries = 0;

private:
    unsigned stable = 0, generation = 0, commit_generation = 0;
    std::uint64_t confirm_until = 0;
    bool reload_next = false;  // a patch declined, so the next attempt reloads
    bool menu_cycle = false;   // the current cycle is a load from the menus
    std::uint32_t thread = 0, context = 0;
    RuntimeCommitGate commit;
    RecoveryGate confirm;
    void stop(Actions& a, const char* why) {
        phase = stopped;
        reason = why;
        a.stopped(why);
    }
};
}
