// Tests the friend's apply loop (src/apply_logic.h) frame by frame, with fake actions.
#include "../src/apply_logic.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace gc::apply;
#include "check.h"

// Fake apply actions: each call is recorded, and a test can make any of them fail.
struct Fake {
    unsigned latest = 0;
    bool ready = true, unchanged = true, prepare_ok = true, commit_ok = true, confirm_ok = true;
    bool prepare_refuses = false;  // refused with nothing changed, as on the AI's turn
    int confirm_failures = 0;      // confirmations that fail before the galaxy is set up
    int patch_result = 0;          // 1 patched in place, 0 declined (reload instead), -1 failed
    std::vector<std::string> calls;
    unsigned newest() { return latest; }
    bool menu_ready() { return ready; }
    int patch(unsigned& r) {
        calls.push_back((patch_result > 0   ? "patch "
                         : patch_result < 0 ? "patch-failed "
                                            : "decline ") +
                        std::to_string(latest));
        r = latest;
        return patch_result;
    }
    int prepare(bool menu, unsigned& r) {
        calls.push_back((prepare_refuses ? "pass " : menu ? "prepare-menu " : "prepare ") + std::to_string(latest));
        r = latest;
        return !prepare_ok ? -1 : prepare_refuses ? 0 : 1;
    }
    bool storage_unchanged() { return unchanged; }
    void withdraw(unsigned r) { calls.push_back("withdraw " + std::to_string(r)); }
    bool commit(bool menu, unsigned r) {
        calls.push_back((menu ? "commit-menu " : "commit ") + std::to_string(r));
        return commit_ok;
    }
    bool confirm(unsigned r) {
        calls.push_back("confirm " + std::to_string(r));
        if (confirm_failures > 0) {
            --confirm_failures;
            return false;
        }
        return confirm_ok;
    }
    void stopped(const char* why) { calls.push_back(std::string("stopped ") + why); }
};

// Frames from a received revision to the commit of a load from the menus.
constexpr unsigned first_cycle = first_settle_frames + first_commit_frames;

// The game's frames: thread 7, context 100, at the main menu, guards passing.
struct Game {
    Frame f{7, 100, menu_flow, 1, true, 0};
    void run(Loop<Fake>& loop, Fake& a, unsigned frames, unsigned ms = 16) {
        for (unsigned i = 0; i < frames; ++i) {
            f.now += ms;
            loop.step(f, a);
        }
    }
    // A load rebuilds the shell into the galaxy: a new generation and a new Lua context.
    void load() {
        ++f.generation;
        f.context += 1;
        f.flow = galaxy_flow;
    }
};

int main() {
    {  // Nothing received: nothing happens.
        Fake a;
        Loop<Fake> loop;
        Game g;
        g.run(loop, a, 500);
        CHECK(a.calls.empty() && loop.phase == Loop<Fake>::waiting);
    }
    {  // A full first cycle from the main menu, then a second from the galaxy.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.run(loop, a, first_settle_frames - 1);  // the first load waits 15 + 15 frames at an idle menu
        CHECK(a.calls.empty());
        g.run(loop, a, 1);
        CHECK(a.calls.size() == 1 && a.calls[0] == "prepare-menu 1" && loop.phase == Loop<Fake>::committing);
        g.run(loop, a, first_commit_frames - 1);
        CHECK(a.calls.size() == 1);
        g.run(loop, a, 1);
        CHECK(a.calls.size() == 2 && a.calls[1] == "commit-menu 1" && loop.phase == Loop<Fake>::confirming);
        g.run(loop, a, 300);  // the old shell never confirms
        CHECK(a.calls.size() == 2);
        g.load();
        g.run(loop, a, confirm_frames - 1);  // 30 stable frames on the rebuilt shell
        CHECK(a.calls.size() == 2);
        g.run(loop, a, 1);
        CHECK(a.calls.size() == 3 && a.calls[2] == "confirm 1" && loop.applied == 1 && loop.in_galaxy);
        // Revisions 2 to 4 arrive and the next cycle takes the newest. In the galaxy the in-place
        // update declines here, so it reloads, settled like a load.
        a.latest = 4;
        g.run(loop, a, patch_settle_frames + settle_frames - 1);
        CHECK(a.calls.size() == 4 && a.calls[3] == "decline 4");
        g.run(loop, a, 1 + 120);
        CHECK(a.calls.size() == 6 && a.calls[4] == "prepare 4" && a.calls[5] == "commit 4");
        a.latest = 5;  // arrives mid-cycle: waits for the next one
        g.load();
        g.run(loop, a, confirm_frames + patch_settle_frames);  // confirmed, then 5 declines
        CHECK(a.calls.size() == 8 && a.calls[6] == "confirm 4" && a.calls[7] == "decline 5" && loop.applied == 4);
        g.run(loop, a, settle_frames);
        CHECK(a.calls.size() == 9 && a.calls[8] == "prepare 5");
        CHECK(loop.cycles == 3 && loop.patches == 0);
    }
    {  // Seamless: after the first load, later revisions are patched in place.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.run(loop, a, first_cycle);
        g.load();
        g.run(loop, a, 1400);
        CHECK(loop.applied == 1 && loop.in_galaxy);
        a.patch_result = 1;
        a.latest = 3;
        g.run(loop, a, patch_settle_frames - 1);
        CHECK(a.calls.back() == "confirm 1");
        g.run(loop, a, 1);  // patched after 6 frames, about 0.1 s
        CHECK(a.calls.back() == "patch 3" && loop.applied == 3 && loop.phase == Loop<Fake>::waiting);
        const size_t before = a.calls.size();
        a.latest = 4;
        g.run(loop, a, 3);
        a.latest = 5;
        g.run(loop, a, settle_frames);  // 4 is superseded while settling
        CHECK(a.calls.size() == before + 1 && a.calls.back() == "patch 5" && loop.applied == 5);
        CHECK(loop.patches == 2 && loop.phase == Loop<Fake>::waiting);  // no load, so no confirmation wait
        g.f.guards = false;
        a.latest = 6;  // never mid-frame or mid-transition
        g.run(loop, a, 500);
        CHECK(a.calls.back() == "patch 5");
        g.f.guards = true;
        a.patch_result = -1;  // the patch fails: the Lua stack is not restored
        g.run(loop, a, settle_frames);
        CHECK(loop.phase == Loop<Fake>::stopped && a.calls.back() == "stopped in-place update failed");
        CHECK(loop.applied == 5);
        // The galaxy still shows, so the host leaving still takes the friend's game out of it.
        CHECK(loop.in_galaxy && loop.can_leave());
    }
    {  // A stop belongs to its co-op game: when that game ends, the next one loads
        // from the menus as usual, and nothing the old host sent is loaded.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        a.patch_result = 1;
        g.run(loop, a, first_cycle);
        g.load();
        g.run(loop, a, confirm_frames);
        a.latest = 2;
        a.patch_result = -1;
        g.run(loop, a, patch_settle_frames);
        CHECK(loop.phase == Loop<Fake>::stopped && loop.can_leave());
        a.latest = 3;  // the old host's last revision
        loop.drop_pending(a.newest());
        CHECK(loop.phase == Loop<Fake>::waiting && loop.resumes == 1 && *loop.reason == 0);
        g.f.flow = shell_flow;
        ++g.f.generation;
        g.f.context += 1;  // its galaxy left for the menus
        const size_t before = a.calls.size();
        g.run(loop, a, 2000);
        CHECK(a.calls.size() == before && !loop.in_galaxy);
        a.latest = 4;
        a.patch_result = 1;  // the next co-op game
        g.run(loop, a, first_cycle);
        CHECK(a.calls.back() == "commit-menu 4");
        g.load();
        g.run(loop, a, confirm_frames);
        CHECK(loop.applied == 4 && loop.in_galaxy && loop.phase == Loop<Fake>::waiting);
        loop.drop_pending(a.newest());  // a game that ends without a stop
        CHECK(loop.resumes == 1);
    }
    {  // A load being confirmed lands in the galaxy and can be left once it has;
        // a confirmation that never matches has still shown that galaxy.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        a.confirm_ok = false;
        g.run(loop, a, first_cycle);
        CHECK(loop.loading() && !loop.in_galaxy && !loop.can_leave());
        g.load();
        g.run(loop, a, 2000);
        CHECK(!loop.loading() && loop.phase == Loop<Fake>::stopped && loop.in_galaxy && loop.can_leave());
        CHECK(a.calls.back() == "stopped confirmation failed");
    }
    {  // A revision that cannot load now (the AI's turn) is passed by and the next one
        // applied, from the menu and after a declined patch.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        a.prepare_refuses = true;
        g.run(loop, a, first_settle_frames);
        CHECK(a.calls.size() == 1 && a.calls[0] == "pass 1" && loop.phase == Loop<Fake>::waiting);
        g.run(loop, a, 2000);  // never retried on its own
        CHECK(a.calls.size() == 1 && loop.applied == 0 && loop.passed == 1);
        a.latest = 2;
        a.prepare_refuses = false;
        g.run(loop, a, first_cycle);
        g.load();
        g.run(loop, a, 1400);
        CHECK(a.calls[1] == "prepare-menu 2" && loop.applied == 2 && loop.in_galaxy);
        a.latest = 3;
        a.prepare_refuses = true;  // the patch declines and the reload refuses
        g.run(loop, a, patch_settle_frames + settle_frames);
        CHECK(a.calls.back() == "pass 3" && loop.phase == Loop<Fake>::waiting && loop.applied == 2);
        a.latest = 4;
        a.patch_result = 1;  // the next revision is patched as usual
        g.run(loop, a, patch_settle_frames);
        CHECK(a.calls.back() == "patch 4" && loop.applied == 4 && loop.refusals == 2);
    }
    {  // A confirmation that runs before the galaxy is set up is repeated, not fatal.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        a.confirm_failures = 2;
        g.run(loop, a, first_cycle);
        g.load();
        g.run(loop, a, confirm_frames);
        CHECK(a.calls.back() == "confirm 1" && loop.phase == Loop<Fake>::confirming && loop.confirm_retries == 1);
        g.run(loop, a, 1000);  // 250 ms and 30 more frames each time
        CHECK(loop.applied == 1 && loop.confirm_retries == 2 && loop.phase == Loop<Fake>::waiting);
        CHECK(a.calls.size() == 5);  // prepare, commit and three checks
    }
    {  // A load needs the menus with no story campaign: never the friend's own galaxy.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.f.flow = galaxy_flow;
        g.run(loop, a, 500);
        g.f.flow = menu_flow;
        a.ready = false;
        g.run(loop, a, 500);
        CHECK(a.calls.empty() && loop.phase == Loop<Fake>::settling);
        a.ready = true;
        g.run(loop, a, first_settle_frames);
        CHECK(a.calls.size() == 1);
    }
    {  // Any shell menu will do: flow 0 is every menu after the first one.
        Fake a;
        Loop<Fake> loop;
        Game g;
        g.f.flow = shell_flow;
        a.latest = 1;
        g.run(loop, a, first_cycle);
        CHECK(a.calls.size() == 2 && a.calls[0] == "prepare-menu 1" && a.calls[1] == "commit-menu 1");
    }
    {  // The friend's game leaves the galaxy for the menus: the next revision loads
        // from the menus again, and a galaxy that is not the host's is never touched.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        a.patch_result = 1;
        g.run(loop, a, first_cycle);
        g.load();
        g.run(loop, a, confirm_frames);
        a.latest = 7;
        g.run(loop, a, patch_settle_frames);
        CHECK(loop.applied == 7 && loop.in_galaxy && a.calls.back() == "patch 7");
        g.f.flow = shell_flow;
        ++g.f.generation;
        g.f.context += 1;  // back at the menus
        g.run(loop, a, 200);
        CHECK(!loop.in_galaxy && a.calls.back() == "patch 7");  // nothing newer yet
        a.latest = 8;
        g.run(loop, a, first_cycle);
        CHECK(a.calls.size() >= 2 && a.calls[a.calls.size() - 2] == "prepare-menu 8" &&
              a.calls.back() == "commit-menu 8" && loop.menu_loads == 2);
        g.load();
        g.run(loop, a, confirm_frames);
        CHECK(loop.applied == 8 && loop.in_galaxy && a.calls.back() == "confirm 8");
        // Back to the menus, then into a galaxy of the friend's own: not patched.
        g.f.flow = shell_flow;
        ++g.f.generation;
        g.run(loop, a, 5);
        g.f.flow = galaxy_flow;
        ++g.f.generation;
        g.f.context += 1;
        a.latest = 9;
        const size_t before = a.calls.size();
        g.run(loop, a, 2000);
        CHECK(a.calls.size() == before && !loop.in_galaxy && loop.phase == Loop<Fake>::settling);
    }
    {  // The host has gone: what it sent is never loaded; a new host's revision is.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 3;
        g.run(loop, a, first_settle_frames / 2);
        CHECK(loop.phase == Loop<Fake>::settling && !loop.can_leave());
        loop.drop_pending(a.newest());
        g.run(loop, a, 2000);
        CHECK(a.calls.empty() && loop.phase == Loop<Fake>::waiting);
        a.latest = 4;
        g.run(loop, a, first_cycle);
        CHECK(a.calls.size() == 2 && a.calls[1] == "commit-menu 4");
        g.load();
        g.run(loop, a, confirm_frames);
        CHECK(loop.in_galaxy && loop.can_leave());
        a.latest = 5;  // a committed load is never abandoned
        g.f.flow = galaxy_flow;
        g.run(loop, a, 1);
        CHECK(loop.phase == Loop<Fake>::settling && loop.can_leave());
    }
    {  // Any unguarded or changed frame restarts the settling count.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.run(loop, a, first_settle_frames - 1);
        g.f.guards = false;
        g.run(loop, a, 1);
        g.f.guards = true;
        g.run(loop, a, first_settle_frames - 1);
        g.f.context += 1;
        g.run(loop, a, first_settle_frames - 1);  // a new Lua context counts from zero
        CHECK(a.calls.empty());
        g.run(loop, a, 1);
        CHECK(a.calls.size() == 1);
    }
    {  // Leaving before the commit postpones it; nothing is written yet, so it is retried.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.run(loop, a, first_settle_frames + first_commit_frames / 2);
        g.f.flow = 2;
        g.run(loop, a, 1);
        CHECK(a.calls.size() == 2 && a.calls[1] == "withdraw 1" && loop.phase == Loop<Fake>::waiting);
        g.f.flow = menu_flow;
        a.unchanged = false;  // storage moved under the prepared candidate
        g.run(loop, a, first_settle_frames + 1);
        CHECK(a.calls.size() == 4 && a.calls[2] == "prepare-menu 1" && a.calls[3] == "withdraw 1");
        a.unchanged = true;
        g.run(loop, a, first_cycle);
        CHECK(a.calls.back() == "commit-menu 1");
    }
    {  // Failures stop applying for the rest of that co-op game.
        for (int which = 0; which < 3; ++which) {
            Fake a;
            Loop<Fake> loop;
            Game g;
            a.latest = 1;
            a.prepare_ok = which != 0;
            a.commit_ok = which != 1;
            a.confirm_ok = which != 2;
            g.run(loop, a, first_cycle);
            g.load();
            g.run(loop, a, 1400);
            CHECK(loop.phase == Loop<Fake>::stopped && loop.applied == 0);
            const size_t calls = a.calls.size();
            a.latest = 9;
            g.run(loop, a, 2000);
            CHECK(a.calls.size() == calls);
        }
    }
    {  // A load that never settles in the galaxy times out and stops.
        Fake a;
        Loop<Fake> loop;
        Game g;
        a.latest = 1;
        g.run(loop, a, first_cycle);
        g.load();
        g.f.flow = 2;
        g.run(loop, a, 9000);
        CHECK(loop.phase == Loop<Fake>::stopped && a.calls.back() == "stopped no stable galaxy after the load");
        CHECK(!loop.in_galaxy && !loop.can_leave());  // no galaxy of the host's has shown
    }
    {  // Versus: the session host's own galaxy takes the friend's turns in place, with
        // no revision applied before; at the menus it is its own again.
        Fake a;
        Loop<Fake> loop;
        Game g;
        g.f.flow = galaxy_flow;
        a.latest = 1;
        a.patch_result = 1;
        g.run(loop, a, 200);
        CHECK(a.calls.empty() && !loop.in_galaxy);  // not adopted: the host's galaxy is never touched
        loop.adopt();
        CHECK(loop.in_galaxy && loop.adopted);
        g.run(loop, a, patch_settle_frames);
        CHECK(a.calls.size() == 1 && a.calls[0] == "patch 1" && loop.applied == 1 && loop.patches == 1);
        a.latest = 2;
        g.run(loop, a, patch_settle_frames);
        CHECK(a.calls.back() == "patch 2" && loop.applied == 2);
        g.f.flow = shell_flow;
        g.run(loop, a, 1);
        CHECK(!loop.in_galaxy && !loop.adopted);  // back at the menus
        a.latest = 3;
        a.ready = false;  // a story campaign in memory: no load
        g.run(loop, a, 200);
        CHECK(a.calls.back() == "patch 2");
    }
    if (failures) {
        std::printf("%d apply check(s) failed\n", failures);
        return 1;
    }
    std::puts(
        "apply_test PASS: menu loads, in-place updates, passed revisions, retries, stops and the Versus host's galaxy");
    return 0;
}
