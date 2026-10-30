// The apply mailbox: the newest offer behind a lock, and the progress block both threads share.
#include "apply_mailbox.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gc {
static SRWLOCK offer_lock = SRWLOCK_INIT;
static ApplyOffer* newest = nullptr;  // never freed, so no destructor runs under the loader lock
// The held offer's revisions, readable without taking the lock.
static std::atomic<unsigned> newest_revision{0}, newest_host_revision{0};
static ApplyProgress progress;

void apply_offer(ApplyOffer offer) {
    AcquireSRWLockExclusive(&offer_lock);
    if (!newest) newest = new ApplyOffer;
    if (offer.revision > newest->revision) {
        *newest = std::move(offer);
        newest_host_revision.store(newest->host_revision, std::memory_order_release);
        newest_revision.store(newest->revision, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&offer_lock);
}

bool apply_newest(unsigned after, ApplyOffer& out) {
    AcquireSRWLockShared(&offer_lock);
    const bool fresh = newest && newest->revision > after;
    if (fresh) out = *newest;
    ReleaseSRWLockShared(&offer_lock);
    return fresh;
}

unsigned apply_newest_revision() { return newest_revision.load(std::memory_order_acquire); }
unsigned apply_newest_host_revision() { return newest_host_revision.load(std::memory_order_acquire); }
ApplyProgress& apply_progress() { return progress; }
}
