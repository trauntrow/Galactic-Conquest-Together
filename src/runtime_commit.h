#pragma once
// Guards for committing a prepared save to the running game: a token check and a frame gate.
#include <array>
#include <cstdint>
#include <cstring>
namespace gc {
// Validates a 96-byte commit token: "GCTC", version 1, process id, revision and nonce, then the
// SHA-256 of the request and of the exported save. A local handshake, not network authentication.
inline bool validate_commit(const unsigned char* bytes, unsigned size, std::uint32_t pid, std::uint32_t revision,
                            const std::array<unsigned char, 16>& nonce, const std::array<unsigned char, 32>& request,
                            const std::array<unsigned char, 32>& serialized) {
    if (size != 96 || std::memcmp(bytes, "GCTC", 4)) return false;
    std::uint32_t fields[3]{};
    std::memcpy(fields, bytes + 4, 12);
    return fields[0] == 1 && fields[1] == pid && fields[2] == revision && !std::memcmp(bytes + 16, nonce.data(), 16) &&
           !std::memcmp(bytes + 32, request.data(), 32) && !std::memcmp(bytes + 64, serialized.data(), 32);
}
// One-shot gate before a commit: attempt after `frames` authorized frames in a row. A failed guard,
// a thread or context change or the 2-minute deadline spends it; no authorization restarts the count.
struct RuntimeCommitGate {
    bool armed = false, spent = false;
    unsigned stable = 0, frames = 120;
    std::uint32_t thread = 0, context = 0;
    std::uint64_t deadline = 0;
    enum Decision { wait, attempt, reject, timeout };
    // Starts on this thread and Lua context; `needed` stable frames lead to the attempt.
    void arm(std::uint32_t t, std::uint32_t c, std::uint64_t now, unsigned needed = 120) {
        armed = true;
        thread = t;
        context = c;
        deadline = now + 120000;
        frames = needed;
    }
    // Called once per frame.
    Decision step(bool authorized, bool guards, std::uint32_t t, std::uint32_t c, std::uint64_t now) {
        if (!armed || spent) return wait;
        if (now > deadline) {
            spent = true;
            return timeout;
        }
        if (!guards || thread != t || context != c) {
            spent = true;
            return reject;
        }
        if (!authorized) {
            stable = 0;
            return wait;
        }
        if (++stable < frames) return wait;
        spent = true;
        return attempt;
    }
};
}
