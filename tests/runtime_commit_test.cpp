// Tests the commit token check and RuntimeCommitGate (src/runtime_commit.h), and RecoveryGate's count.
#include "../src/runtime_commit.h"
#include "../src/recovery_gate.h"
#include <cassert>
#include <cstdio>
int main() {
    // A 96-byte token: "GCTC", version, process id and revision, then the nonce and both digests.
    std::array<unsigned char, 16> nonce{};
    nonce.fill(17);
    std::array<unsigned char, 32> request{}, serialized{};
    request.fill(29);
    serialized.fill(41);
    unsigned char token[97]{};
    std::memcpy(token, "GCTC", 4);
    const std::uint32_t fields[] = {1, 123, 2};
    std::memcpy(token + 4, fields, 12);
    std::memcpy(token + 16, nonce.data(), 16);
    std::memcpy(token + 32, request.data(), 32);
    std::memcpy(token + 64, serialized.data(), 32);
    auto valid = [&](unsigned size) { return gc::validate_commit(token, size, 123, 2, nonce, request, serialized); };
    // Exactly 96 bytes pass, and flipping any bit fails.
    assert(valid(96));
    assert(!valid(95));
    assert(!valid(97));
    for (unsigned i = 0; i < 96; ++i) {
        token[i] ^= 1;
        assert(!valid(96));
        token[i] ^= 1;
    }
    // The gate waits until armed, then attempts once after 120 authorized frames.
    using Gate = gc::RuntimeCommitGate;
    Gate gate;
    assert(gate.step(true, true, 1, 2, 0) == Gate::wait);
    gate.arm(1, 2, 100);
    for (unsigned i = 0; i < 200; ++i) assert(gate.step(false, true, 1, 2, 200) == Gate::wait);
    for (unsigned i = 0; i < 119; ++i) assert(gate.step(true, true, 1, 2, 200) == Gate::wait);
    assert(gate.step(true, true, 1, 2, 200) == Gate::attempt);
    assert(gate.step(true, true, 1, 2, 201) == Gate::wait);
    // A failed guard, another thread or context, or the 120 s deadline ends the gate for good.
    for (unsigned failure = 0; failure < 4; ++failure) {
        Gate invalid;
        invalid.arm(1, 2, 100);
        auto decision =
            invalid.step(false, failure != 0, failure == 1 ? 3 : 1, failure == 2 ? 3 : 2, failure == 3 ? 120101 : 200);
        assert(decision == (failure == 3 ? Gate::timeout : Gate::reject));
        assert(invalid.spent && invalid.step(true, true, 1, 2, 201) == Gate::wait);
    }
    // RecoveryGate: a newer reload at the same context restarts the 120-frame count.
    gc::RecoveryGate reload;
    reload.arm(7, 1, 0);
    for (unsigned i = 0; i < 119; ++i) assert(reload.step(7, 2, 3, 3, true, 21000) == gc::RecoveryGate::wait);
    for (unsigned i = 0; i < 119; ++i) assert(reload.step(7, 3, 3, 3, true, 21000) == gc::RecoveryGate::wait);
    assert(reload.step(7, 3, 3, 3, true, 21000) == gc::RecoveryGate::execute);
    puts("runtime_commit_test PASS exact authorization, one attempt, expiry, context/thread/menu departure");
}
