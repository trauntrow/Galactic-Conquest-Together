// Tests the frame probe's trampoline (src/frame_probe.cpp) on stand-ins for the game's frame driver.
#include "../src/frame_probe.h"
#include "../third_party/minhook/include/MinHook.h"
#include <cstdio>
#include <cstring>

// What the hooked function saw: eax to ebp, EFLAGS, xmm0 and st(0), and its caller's return address.
static DWORD observed[10], expected[10], caller_address, result;
// A stand-in Lua state header (top at +8, base at +C), found through `context`.
static DWORD header[12] = {0, 0, 0x12345008, 0x12345000};
static DWORD context = reinterpret_cast<DWORD>(header);
static DWORD flag = 0;
static void* target;

// The hooked target: records the machine state and returns 12345678h.
__declspec(naked) static void record_entry() {
    __asm {
        pushfd
        pop dword ptr [observed+28]
        mov dword ptr [observed], eax
        mov dword ptr [observed+4], ecx
        mov dword ptr [observed+8], edx
        mov dword ptr [observed+12], ebx
        mov dword ptr [observed+16], esi
        mov dword ptr [observed+20], edi
        mov dword ptr [observed+24], ebp
        movd dword ptr [observed+32], xmm0
        fst dword ptr [observed+36]
        mov eax, [esp]
        mov caller_address, eax
        mov eax, 12345678h
        ret
    }
}

// The game's frame driver starts the same way: PUSH ECX, then a seven-byte absolute CMP that
// crosses the five-byte detour.
__declspec(naked) static void game_shape() {
    __asm {
        push ecx
        cmp byte ptr [flag], 0
        pop ecx
        jmp record_entry
    }
}

// Calls the target with known values in every register, xmm0 and st(0), and the carry flag set.
__declspec(naked) static void invoke() {
    __asm {
        pushfd
        pushad
        xor eax, eax
        mov eax, 11111111h
        mov ecx, 22222222h
        mov edx, 33333333h
        mov ebx, 44444444h
        mov esi, 55555555h
        mov edi, 66666666h
        mov ebp, 77777777h
        movd xmm0, eax
        fld1
        stc
        call dword ptr [target]
        mov result, eax
        fstp st(0)
        popad
        popfd
        ret
    }
}

// "game-shape" hooks the stand-in with the game's first instructions; "shared" initializes MinHook
// first, as the battle code does before this install.
int main(int argc, char** argv) {
    const bool shape = argc > 1 && !strcmp(argv[1], "game-shape");
    const bool shared = argc > 1 && !strcmp(argv[1], "shared");
    // The real installer refuses an executable that is not the game.
    if (gc::install_frame_probe(GetModuleHandleW(nullptr))) {
        puts("FAIL: frame guard accepted the test executable");
        return 8;
    }
    if (shared && MH_Initialize() != MH_OK) {
        puts("FAIL: MinHook would not initialize first");
        return 9;
    }
    target = shape ? reinterpret_cast<void*>(&game_shape) : reinterpret_cast<void*>(&record_entry);
    // Unhooked, the call gives the reference state.
    invoke();
    memcpy(expected, observed, sizeof expected);
    const DWORD expected_caller = caller_address;
    // Hooked, the call must see the same registers, flags, x87 and XMM state, result and LastError.
    if (!gc::test_install_frame_probe(target, expected_caller, &context)) return 1;
    SetLastError(0x13579);
    invoke();
    const DWORD error = GetLastError();
    if (memcmp(expected, observed, sizeof expected) || result != 0x12345678 || error != 0x13579) {
        puts("FAIL: register, flags, x87, XMM, return value or LastError changed");
        return 2;
    }
    // The sample names this thread, the caller, the context and its header.
    gc::FrameSample s;
    if (!gc::take_frame_sample(s) || s.caller != expected_caller || s.thread != GetCurrentThreadId() || !s.readable ||
        s.context != context || memcmp(s.header, header, sizeof header))
        return 3;
    context = 1;  // an invalid state gives an unreadable sample, not an exception
    invoke();
    if (!gc::take_frame_sample(s) || s.readable || s.context != 1) return 4;
    context = 0;  // no state
    invoke();
    if (!gc::take_frame_sample(s) || s.readable || s.context) return 5;
    context = reinterpret_cast<DWORD>(header);
    // The queue holds 256 samples; the other 44 count as dropped.
    for (int i = 0; i < 300; ++i) invoke();
    unsigned count = 0;
    while (gc::take_frame_sample(s)) ++count;
    if (count != 256 || gc::frame_dropped() != 44) return 6;
    // After draining, samples are taken again.
    invoke();
    if (!gc::take_frame_sample(s) || !s.readable) return 7;
    puts("PASS: frame trampoline preserves machine state; null/invalid contexts and queue overflow handled");
    return 0;
}
