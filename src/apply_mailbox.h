#pragma once
// Hand-off from the Steam thread to the game thread's apply loop: received revisions in, progress out.
#include <atomic>
#include <string>

namespace gc {
// A revision received from the host, ready for the game's native loader (save_candidate.cpp).
struct ApplyOffer {
    unsigned revision = 0;          // this PC's offer count, which orders the apply loop
    unsigned host_revision = 0;     // the revision number on the host's session, for logs and checks
    std::string wire;               // the 26 native values, argument 3 empty
    std::string normalized_sha256;  // of the snapshot it is built from
};
// Steam thread: keeps the offer if its revision is newer than the one held.
void apply_offer(ApplyOffer offer);
// Game thread: copies the newest offer when its revision is greater than `after`.
bool apply_newest(unsigned after, ApplyOffer& out);
unsigned apply_newest_revision();
unsigned apply_newest_host_revision();

// The apply loop's state for the status file. Written by the game thread except where noted.
struct ApplyProgress {
    std::atomic<int> phase{-1};  // the apply::Loop phase, or -1 until the loop starts
    std::atomic<unsigned> applied{0}, cycles{0};
    std::atomic<unsigned> applied_offer{0};  // offer number of the applied revision
    std::atomic<const char*> reason{""};     // the reason applying stopped, if it has
    std::atomic<bool> in_galaxy{false};      // the host's galaxy, applied by the loop, is showing
    std::atomic<bool> pending{false};        // a newer revision is waiting to load
    // Steam thread: counts the times the host left. The game thread then drops that host's
    // revisions and quits its galaxy to the menus if it shows.
    std::atomic<unsigned> host_left{0};
    // Steam thread, host's PC: counts friends leaving while the galaxy shows. The game thread then
    // shows the save prompt and returns to the menus, as the galaxy menu's Quit does.
    std::atomic<unsigned> friend_left{0};
};
ApplyProgress& apply_progress();
}
