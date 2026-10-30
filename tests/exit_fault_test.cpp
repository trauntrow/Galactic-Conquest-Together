// Tests src/exit_fault.h on a made-up stack laid out as the game's last-resort handler leaves it
// at _exit(0): return slot, saved esp and exception pointers above it, record and context below.
#include "../src/exit_fault.h"
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"

namespace f = gc::fault;

// A made-up address space covering [base, base + size); reads outside it fail.
struct Memory {
    uint32_t base;
    std::vector<unsigned char> bytes;
    Memory(uint32_t b, size_t n) : base(b), bytes(n) {}
    bool read(uint32_t at, void* out, size_t n) const {
        if (at < base || at - base > bytes.size() || bytes.size() - (at - base) < n) return false;
        memcpy(out, bytes.data() + (at - base), n);
        return true;
    }
    void put(uint32_t at, const void* data, size_t n) { memcpy(bytes.data() + (at - base), data, n); }
    void word(uint32_t at, uint32_t v) { put(at, &v, 4); }
};

// Addresses in the made-up stack.
constexpr uint32_t fake_stack = 0x00100000, fake_slot = 0x00100F00, fake_pointers = 0x00100400,
                   fake_record = 0x00100200, fake_context = 0x00100500, fake_esp = 0x00100100, fake_ebp = 0x00100180;

// The handler's frame for a fault at 0x006D1234, by default an access violation reading 0x10.
static Memory frame(DWORD code = f::access_violation, DWORD flags = CONTEXT_FULL) {
    Memory m(fake_stack, 0x2000);
    m.word(fake_slot, 0x00797B4E);  // the return into the handler
    m.word(fake_slot + 4, 0);       // _exit's argument
    m.word(fake_slot + f::saved_esp_offset, fake_slot + f::saved_esp_value);
    m.word(fake_slot + f::pointers_offset, fake_pointers);
    m.word(fake_pointers, fake_record);
    m.word(fake_pointers + 4, fake_context);
    EXCEPTION_RECORD r{};
    r.ExceptionCode = code;
    r.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(0x006D1234));
    r.NumberParameters = 2;
    r.ExceptionInformation[0] = 0;
    r.ExceptionInformation[1] = 0x10;
    m.put(fake_record, &r, sizeof r);
    CONTEXT c{};
    c.ContextFlags = flags;
    c.Eip = 0x006D1234;
    c.Esp = fake_esp;
    c.Ebp = fake_ebp;
    c.Eax = 0x10;
    c.Ebx = 0x11;
    c.Ecx = 0x12;
    c.Edx = 0x13;
    c.Esi = 0x14;
    c.Edi = 0x15;
    m.put(fake_context, &c, sizeof c);
    // Frames above the fault, [ebp] the caller's ebp and [ebp+4] its return; the third links to itself.
    m.word(fake_ebp, 0x00100300);
    m.word(fake_ebp + 4, 0x0053B3DC);
    m.word(0x00100300, 0x00100380);
    m.word(0x00100304, 0x0053BAF6);
    m.word(0x00100380, 0x00100380);
    m.word(0x00100384, 0x0053ADD9);
    return m;
}

int main() {
    auto read_from = [](const Memory& m) {
        return [&m](uint32_t at, void* out, size_t n) { return m.read(at, out, n); };
    };
    // The handler's frame: the fault, its access, the registers and the frame chain.
    {
        const Memory m = frame();
        f::Fault got;
        CHECK(f::read_fault(fake_slot, read_from(m), got));
        CHECK(got.code == f::access_violation && got.address == 0x006D1234);
        CHECK(got.access_known && got.access == 0 && got.target == 0x10);
        CHECK(got.context_ok && got.eip == 0x006D1234 && got.esp == fake_esp && got.ebp == fake_ebp);
        CHECK(got.eax == 0x10 && got.ebx == 0x11 && got.ecx == 0x12 && got.edx == 0x13 && got.esi == 0x14 &&
              got.edi == 0x15);
        uint32_t returns[8]{};
        CHECK(f::frame_chain(got.esp, got.ebp, read_from(m), returns, 8) == 3);
        CHECK(returns[0] == 0x0053B3DC && returns[1] == 0x0053BAF6 && returns[2] == 0x0053ADD9);
        CHECK(f::frame_chain(got.esp, got.ebp, read_from(m), returns, 1) == 1);      // at most `most`
        CHECK(f::frame_chain(got.esp, got.esp - 4, read_from(m), returns, 8) == 0);  // an ebp below the stack
    }
    // A float divide by zero, the filter's other exception: no access to report.
    {
        const Memory m = frame(f::float_divide_by_zero);
        f::Fault got;
        CHECK(f::read_fault(fake_slot, read_from(m), got) && got.code == f::float_divide_by_zero && !got.access_known);
    }
    // Any other exception is not the handler's: refused, the output left alone.
    {
        const Memory m = frame(0x80000003);
        f::Fault got;
        got.code = 7;
        CHECK(!f::read_fault(fake_slot, read_from(m), got) && got.code == 7);  // unchanged
    }
    // Not the handler's frame: a wrong saved esp, no pointers, or pointers that do not read.
    {
        Memory m = frame();
        m.word(fake_slot + f::saved_esp_offset, fake_slot + 0x40);
        f::Fault got;
        CHECK(!f::read_fault(fake_slot, read_from(m), got));
        m = frame();
        m.word(fake_slot + f::pointers_offset, 0);
        CHECK(!f::read_fault(fake_slot, read_from(m), got));
        m = frame();
        m.word(fake_pointers, 0);
        CHECK(!f::read_fault(fake_slot, read_from(m), got));
        m = frame();
        m.word(fake_slot + f::pointers_offset, 0x00900000);  // unmapped
        CHECK(!f::read_fault(fake_slot, read_from(m), got));
        CHECK(!f::read_fault(0x00900000, read_from(m), got));
    }
    // A context that does not read back: the fault still, without registers.
    {
        Memory m = frame(f::access_violation, CONTEXT_CONTROL);  // no integer registers
        f::Fault got;
        CHECK(f::read_fault(fake_slot, read_from(m), got) && got.address == 0x006D1234 && !got.context_ok);
        m = frame();
        m.word(fake_pointers + 4, 0);
        CHECK(f::read_fault(fake_slot, read_from(m), got) && !got.context_ok);
    }
    if (failures) {
        std::printf("exit_fault_test FAILED %d\n", failures);
        return 1;
    }
    std::printf(
        "exit_fault_test PASS: fault, registers and frame chain read back; other exceptions and frames refused\n");
    return 0;
}
