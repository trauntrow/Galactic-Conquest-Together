#pragma once
// Runs an operation against the game's native save buffer with a backup that is restored afterwards.
#include <array>
#include <cstdint>

namespace gc {
// The native save buffer: its length and its 512 bytes.
struct SavedBuffer {
    std::uint32_t length = 0;
    std::array<unsigned char, 512> bytes{};
    bool operator==(const SavedBuffer& b) const { return length == b.length && bytes == b.bytes; }
};
// How far a transaction got. committed: the operation's result stays in the buffer.
struct SaveTransactionResult {
    bool backed_up = false, entered = false, operation_ok = false;
    bool restore_writes_ok = false, restored = false;
    bool committed = false;
};
// Backs up the buffer, runs operation and restores the backup, unless retain_success is set and the
// operation succeeded. IO reads and writes the buffer; tests pass one that fails on purpose.
template <class IO, class Operation>
SaveTransactionResult save_transaction(IO& io, Operation operation, bool retain_success = false) {
    SaveTransactionResult result;
    SavedBuffer original{}, confirm{}, restored{};
    if (!io.read(original) || original.length > 512 || !io.read(confirm) || !(original == confirm)) return result;
    result.backed_up = true;
    result.entered = true;
    // A protected Lua call: failure comes back as false, never as an exception.
    result.operation_ok = operation();
    if (retain_success && result.operation_ok) {
        result.committed = true;
        return result;
    }
    // Zero the length first and restore it last, so the buffer never looks valid half-written.
    // Every step runs even if an earlier write fails.
    const bool invalidated = io.length(0);
    const bool bytes = io.buffer(original.bytes);
    const bool length = io.length(original.length);
    result.restore_writes_ok = invalidated && bytes && length;
    result.restored = io.read(restored) && restored == original;
    return result;
}
struct StagedTransactionResult {
    bool seeded = false;
    SaveTransactionResult inner{}, outer{};
};
// Runs operation with the buffer seeded from baseline: the inner transaction restores the baseline,
// the outer one the original contents.
template <class IO, class Operation>
StagedTransactionResult staged_save_transaction(IO& io, const SavedBuffer& baseline, Operation operation) {
    StagedTransactionResult result;
    if (!baseline.length || baseline.length > 512) return result;
    result.outer = save_transaction(io, [&] {
        SavedBuffer check{};
        if (!io.length(0) || !io.buffer(baseline.bytes) || !io.length(baseline.length) || !io.read(check) ||
            !(check == baseline))
            return false;
        result.seeded = true;
        result.inner = save_transaction(io, operation);
        return result.inner.operation_ok && result.inner.restore_writes_ok && result.inner.restored;
    });
    return result;
}
}
