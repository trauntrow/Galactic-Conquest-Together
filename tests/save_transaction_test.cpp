// Tests save_transaction and staged_save_transaction (src/save_transaction.h) on scripted storage,
// and RecoveryGate (src/recovery_gate.h).
#include "../src/save_transaction.h"
#include "../src/recovery_gate.h"
#include <cassert>
#include <cstdio>
#include <vector>
// Scripted save storage: counts reads and writes, fails the chosen one and records the write order.
struct IO {
    gc::SavedBuffer state{};
    int reads = 0, fail_read = 0, writes = 0, fail_write = 0;
    bool unstable = false;
    std::vector<int> order;
    bool read(gc::SavedBuffer& out) {
        ++reads;
        if (reads == fail_read) return false;
        out = state;
        if (unstable && reads == 2) out.bytes[511] ^= 1;  // the confirming read differs
        return true;
    }
    bool length(std::uint32_t n) {
        order.push_back(n ? 3 : 1);  // 1 clears the length, 3 restores it
        if (++writes == fail_write) return false;
        state.length = n;
        return true;
    }
    bool buffer(const std::array<unsigned char, 512>& bytes) {
        order.push_back(2);
        if (++writes == fail_write) return false;
        state.bytes = bytes;
        return true;
    }
};
int main() {
    {
        // RecoveryGate runs once: 20 s after arming, on a newer reload, after 120 unchanged frames.
        // Another thread or context, or a failed guard, restarts the count.
        gc::RecoveryGate gate;
        assert(gate.step(1, 1, 1, 3, true, 0) == gc::RecoveryGate::wait);
        gate.arm(7, 2, 1000);
        for (unsigned t = 1000; t < 21000; t += 100) assert(gate.step(7, 3, 11, 3, true, t) == gc::RecoveryGate::wait);
        for (unsigned i = 0; i < 130; ++i) assert(gate.step(7, 2, 11, 3, true, 21000) == gc::RecoveryGate::wait);
        for (unsigned i = 0; i < 119; ++i) assert(gate.step(7, 3, 11, 3, true, 21000) == gc::RecoveryGate::wait);
        assert(gate.step(8, 3, 11, 3, true, 21000) == gc::RecoveryGate::wait);
        for (unsigned i = 0; i < 119; ++i) assert(gate.step(7, 3, 11, 3, true, 21000) == gc::RecoveryGate::wait);
        assert(gate.step(7, 3, 12, 3, true, 21000) == gc::RecoveryGate::wait);
        assert(gate.step(7, 3, 12, 3, false, 21000) == gc::RecoveryGate::wait);
        for (unsigned i = 0; i < 119; ++i) assert(gate.step(7, 3, 12, 3, true, 21000) == gc::RecoveryGate::wait);
        assert(gate.step(7, 3, 12, 3, true, 21000) == gc::RecoveryGate::execute);
        assert(gate.step(7, 4, 12, 3, true, 21000) == gc::RecoveryGate::wait);
        // Its 120 s deadline ends it with a timeout, once.
        gc::RecoveryGate timeout;
        timeout.arm(7, 2, 0);
        assert(timeout.step(7, 3, 12, 3, true, 120000) == gc::RecoveryGate::timeout);
        assert(timeout.step(7, 3, 12, 3, true, 120001) == gc::RecoveryGate::wait);
    }
    // With retain_success, a successful operation is kept and a failed one undone.
    for (bool success : {false, true}) {
        IO io;
        const auto before = io.state;
        auto r = gc::save_transaction(
            io,
            [&] {
                io.state.length = 204;
                io.state.bytes.fill(3);
                return success;
            },
            true);
        assert(r.committed == success && r.operation_ok == success);
        if (success) assert(io.writes == 0 && io.state.length == 204 && !r.restored);
        else assert(io.state == before && r.restored && io.writes == 3);
    }
    // A failed seed write, a failed inner restore or a failed operation still ends with the original
    // storage, restored by the outer transaction.
    for (int fail = 0; fail <= 6; ++fail)
        for (bool success : {false, true}) {
            IO io;
            io.state.length = 17;
            io.state.bytes.fill(9);
            io.fail_write = fail;
            const auto original = io.state;
            gc::SavedBuffer baseline;
            baseline.length = 204;
            baseline.bytes.fill(3);
            bool entered = false;
            auto r = gc::staged_save_transaction(io, baseline, [&] {
                entered = true;
                assert(io.state == baseline);
                io.state.length = 99;
                io.state.bytes.fill(7);
                return success;
            });
            assert(r.outer.restored && r.outer.restore_writes_ok && io.state == original);
            if (fail >= 1 && fail <= 3) assert(!r.seeded && !entered && !r.outer.operation_ok);
            else {
                assert(r.seeded && entered);
                assert(r.inner.restore_writes_ok == (fail == 0));
                assert(r.outer.operation_ok == (success && fail == 0));
            }
        }
    // A failed outer restore write is reported; storage counts as restored only if it reads back intact.
    for (int fail = 7; fail <= 9; ++fail) {
        IO io;
        io.fail_write = fail;
        io.state.length = 17;
        gc::SavedBuffer baseline;
        baseline.length = 204;
        baseline.bytes.fill(3);
        auto r = gc::staged_save_transaction(io, baseline, [&] {
            io.state.bytes.fill(7);
            return true;
        });
        assert(r.inner.restored && !r.outer.restore_writes_ok);
        assert(r.outer.restored == (fail == 7));
    }
    // Lengths 0, 204 and 512 are restored whole: length cleared, bytes written, then the length.
    for (unsigned size : {0u, 204u, 512u})
        for (bool operation_ok : {false, true}) {
            IO io;
            io.state.length = size;
            for (unsigned i = 0; i < 512; ++i) io.state.bytes[i] = static_cast<unsigned char>(i);
            const auto original = io.state;
            auto result = gc::save_transaction(io, [&] {
                io.state.length = 33;
                io.state.bytes.fill(7);
                return operation_ok;
            });
            assert(result.backed_up && result.entered && result.operation_ok == operation_ok);
            assert(result.restore_writes_ok && result.restored && io.state == original);
            assert(io.order.size() == 3 && io.order[0] == 1 && io.order[1] == 2);
        }
    // No backup, no entry: a length over 512, a failed read, or two reads that differ.
    for (int mode = 0; mode < 4; ++mode) {
        IO io;
        io.state.length = mode == 0 ? 513 : 10;
        io.fail_read = mode == 1 ? 1 : mode == 2 ? 2 : 0;
        io.unstable = mode == 3;
        bool entered = false;
        auto r = gc::save_transaction(io, [&] {
            entered = true;
            return true;
        });
        assert(!r.backed_up && !entered && !r.entered && io.writes == 0);
    }
    // All three restore writes run even when one fails; losing only the length clear still restores.
    for (int fail = 1; fail <= 3; ++fail) {
        IO io;
        io.state.length = 204;
        io.fail_write = fail;
        auto r = gc::save_transaction(io, [&] {
            io.state.length = 20;
            io.state.bytes.fill(1);
            return true;
        });
        assert(!r.restore_writes_ok && io.writes == 3);
        assert(r.restored == (fail == 1));
    }
    // A failed read-back after the restore leaves it unconfirmed.
    IO io;
    io.fail_read = 3;
    auto r = gc::save_transaction(io, [] { return true; });
    assert(r.restore_writes_ok && !r.restored);
    std::puts(
        "save_transaction_test PASS backup guards, success/error recovery, full-buffer restore, write/read failures");
}
