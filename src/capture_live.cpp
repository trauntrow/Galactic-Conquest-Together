// In-process validation of the host's campaign capture, and the newest result for the Steam thread.
#include "diagnostics.h"
#include "capture_decode.h"
#include "capture_live.h"

namespace gc {
// Tags come straight from find_observer rather than from the log, which can drop lines under
// contention. Fails closed: after the first rejection nothing more from this session is trusted.
static SRWLOCK capture_lock = SRWLOCK_INIT;
static capture::Stream* capture_stream = nullptr;
static bool capture_failed = false;
static CaptureOffer* capture_newest = nullptr;  // never freed, like capture_stream

// Read by the Steam thread; tags arrive on the game thread.
bool capture_latest(unsigned after, CaptureOffer& out) {
    AcquireSRWLockShared(&capture_lock);
    const bool fresh = capture_newest && capture_newest->revision > after;
    if (fresh) out = *capture_newest;
    ReleaseSRWLockShared(&capture_lock);
    return fresh;
}

// Validates one GCOBS tag and publishes each snapshot it completes as the next revision.
void capture_channel(const char* tag) {
    AcquireSRWLockExclusive(&capture_lock);
    if (!capture_failed) {
        try {
            // Never freed: a destructor at process exit would run under the loader lock.
            if (!capture_stream) capture_stream = new capture::Stream;
            std::vector<capture::Validated> fresh;
            capture_stream->tag(tag, fresh);
            for (const capture::Validated& s : fresh) {
                if (!capture_newest) capture_newest = new CaptureOffer;
                CaptureOffer next;
                next.revision = capture_newest->revision + 1;
                next.producer = capture_stream->version();
                next.generation = s.generation;
                next.sequence = s.sequence;
                next.source_table = s.source_table;
                next.update_ticket = s.update_ticket;
                next.unchanged_samples = s.unchanged_samples;
                next.normalized_sha256 = s.normalized_sha256;
                next.json = capture::canonical_json(s.data);
                turn_facts(s.data, next.player_team, next.turn_number, next.local_team);
                *capture_newest = std::move(next);
                log("CAPTURE_SNAPSHOT generation=%d sequence=%d source=%d ticket=%d unchanged=%d observations=%u revision=%u normalized_sha256=%s turn=%d active=%d own=%d",
                    s.generation, s.sequence, s.source_table, s.update_ticket, s.unchanged_samples,
                    static_cast<unsigned>(s.model_observations.size()), capture_newest->revision,
                    s.normalized_sha256.c_str(), capture_newest->turn_number, capture_newest->player_team,
                    capture_newest->local_team);
            }
            if (capture_stream->stopped()) {
                capture_failed = true;
                log("CAPTURE_REJECTED %.300s; in-process validation stopped for this session",
                    capture_stream->error().c_str());
            }
        } catch (...) {
            capture_failed = true;
            log("CAPTURE_REJECTED internal error; in-process validation stopped for this session");
        }
    }
    ReleaseSRWLockExclusive(&capture_lock);
}
}
