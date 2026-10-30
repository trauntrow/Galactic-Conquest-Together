#pragma once
// Reads the fault behind the game's silent exit. Its top-level __try handler calls _exit(0), and
// the DLL's _exit observer finds the exception record still on that handler's stack.
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace gc::fault {
static_assert(sizeof(void*) == 4, "the game's frames are x86");
// Where the handler's call to _exit returns: 0x00617B4E at the preferred base 0x00400000.
constexpr uint32_t handler_return_rva = 0x217B4E;
// Offsets from that call's return slot: the wrapper's saved esp at +18h must point 8 bytes above
// the slot, and the filter's EXCEPTION_POINTERS are at +1Ch.
constexpr uint32_t saved_esp_offset = 0x18, saved_esp_value = 0x08, pointers_offset = 0x1C;
constexpr DWORD access_violation = 0xC0000005, float_divide_by_zero = 0xC000008E;

// What the observer logs about a fault.
struct Fault {
    DWORD code = 0, address = 0;
    // For an access violation: the access (0 read, 1 write, 8 execute) and the address touched.
    bool access_known = false;
    DWORD access = 0, target = 0;
    // The faulting thread's registers, if its context could be read.
    bool context_ok = false;
    DWORD eip = 0, esp = 0, ebp = 0, eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0;
};

// Fills `out` when `return_slot` is the handler's frame and its record is one the filter takes.
// read(address, out, size) copies `size` bytes or fails without faulting; registers are optional.
template <class Read> bool read_fault(uint32_t return_slot, Read&& read, Fault& out) {
    uint32_t saved_esp = 0, pointers = 0, both[2]{};
    if (!read(return_slot + saved_esp_offset, &saved_esp, sizeof saved_esp) ||
        saved_esp != return_slot + saved_esp_value)
        return false;
    if (!read(return_slot + pointers_offset, &pointers, sizeof pointers) || !pointers ||
        !read(pointers, both, sizeof both) || !both[0])
        return false;
    // The first seven words of EXCEPTION_RECORD: code, flags, next record, address, parameter count
    // and the two access violation parameters.
    DWORD record[7]{};
    static_assert(offsetof(EXCEPTION_RECORD, ExceptionCode) == 0 &&
                      offsetof(EXCEPTION_RECORD, ExceptionAddress) == 12 &&
                      offsetof(EXCEPTION_RECORD, NumberParameters) == 16 &&
                      offsetof(EXCEPTION_RECORD, ExceptionInformation) == 20,
                  "EXCEPTION_RECORD layout");
    if (!read(both[0], record, sizeof record)) return false;
    if (record[0] != access_violation && record[0] != float_divide_by_zero) return false;
    Fault f;
    f.code = record[0];
    f.address = record[3];
    if (record[0] == access_violation && record[4] >= 2) {
        f.access_known = true;
        f.access = record[5];
        f.target = record[6];
    }
    // CONTEXT keeps Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags and Esp in one block.
    static_assert(offsetof(CONTEXT, Esi) == offsetof(CONTEXT, Edi) + 4 &&
                      offsetof(CONTEXT, Eax) == offsetof(CONTEXT, Edi) + 20 &&
                      offsetof(CONTEXT, Ebp) == offsetof(CONTEXT, Edi) + 24 &&
                      offsetof(CONTEXT, Eip) == offsetof(CONTEXT, Edi) + 28 &&
                      offsetof(CONTEXT, Esp) == offsetof(CONTEXT, Edi) + 40,
                  "CONTEXT layout");
    DWORD flags = 0, regs[11]{};
    const DWORD needed = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (both[1] && read(both[1] + offsetof(CONTEXT, ContextFlags), &flags, sizeof flags) &&
        (flags & needed) == needed && read(both[1] + offsetof(CONTEXT, Edi), regs, sizeof regs)) {
        f.context_ok = true;
        f.edi = regs[0];
        f.esi = regs[1];
        f.ebx = regs[2];
        f.edx = regs[3];
        f.ecx = regs[4];
        f.eax = regs[5];
        f.ebp = regs[6];
        f.eip = regs[7];
        f.esp = regs[10];
    }
    out = f;
    return true;
}

// Return addresses up the saved-ebp chain from the faulting frame, while it stays within `span`
// bytes above `esp`. Code without frame pointers ends the chain early.
template <class Read>
size_t frame_chain(uint32_t esp, uint32_t ebp, Read&& read, uint32_t* returns, size_t most, uint32_t span = 0x100000) {
    size_t n = 0;
    while (n < most && ebp >= esp && ebp - esp < span) {
        uint32_t frame[2]{};
        if (!read(ebp, frame, sizeof frame) || !frame[1]) break;
        returns[n++] = frame[1];
        if (frame[0] <= ebp) break;
        ebp = frame[0];
    }
    return n;
}
}
