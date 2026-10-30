// The frame hook: runs on the game thread once per frame, samples it and steps the Lua bridge.
#include "frame_probe.h"
#include "../third_party/minhook/include/MinHook.h"
#include <atomic>
#include <cstring>
#if GC_LUA_QUERY
#include "lua_query.h"
#endif

static_assert(sizeof(void*) == 4, "Frame probe requires x86");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Callback cannot take locks");
namespace {
// Hook state, and a ring of samples the hook writes and the worker thread reads.
void* trampoline = nullptr;
DWORD expected_caller = 0;
DWORD* context_slot = nullptr;
constexpr uint32_t capacity = 256;
gc::FrameSample samples[capacity]{};
std::atomic<uint32_t> written{0}, consumed{0}, dropped{0};
std::atomic_flag producer = ATOMIC_FLAG_INIT;  // set while recording; an overlapping call is dropped

// Runs on the game thread each frame: records a sample and steps the Lua bridge. No logging or locks,
// and memory is read with read_memory, so a bad Lua state cannot fault the game.
void __cdecl observe_frame(DWORD caller) {
    const DWORD error = GetLastError();
    if (producer.test_and_set(std::memory_order_acquire)) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        SetLastError(error);
        return;
    }
    const uint32_t w = written.load(std::memory_order_relaxed);
    if (w - consumed.load(std::memory_order_acquire) < capacity) {
        auto& s = samples[w % capacity];
        s = {};
        s.caller = caller;
        s.thread = GetCurrentThreadId();
        s.tick = GetTickCount64();
        if (caller == expected_caller && gc::read_memory(context_slot, &s.context, sizeof s.context)) {
            s.readable = s.context && gc::read_memory(reinterpret_cast<void*>(s.context), s.header, sizeof s.header);
            if (!s.readable) memset(s.header, 0, sizeof s.header);
        }
#if GC_LUA_QUERY
        gc::step_lua_query(s);
#endif
        written.store(w + 1, std::memory_order_release);
    } else {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    producer.clear(std::memory_order_release);
    SetLastError(error);
}

// Saves registers, flags and FPU/SSE state around observe_frame, then jumps to the original function
// through MinHook's trampoline, so the original returns to the game itself.
__declspec(naked) void frame_entry() {
    __asm {
        pushfd
        pushad
        mov ebp, esp
        sub esp, 528  // room for the 512-byte fxsave area, aligned to 16
        and esp, -16
        fxsave [esp]
        cld
        push dword ptr [ebp+36]  // return address of the hooked call
        call observe_frame
        add esp, 4
        fxrstor [esp]
        mov esp, ebp
        popad
        popfd
        jmp dword ptr [trampoline]
    }
}

// Hooks target. caller is the expected return address; *slot is the current Lua state.
// Needs SSE2; installs once.
bool install(void* target, DWORD caller, DWORD* slot) {
    if (!IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) || trampoline) return false;
    expected_caller = caller;
    context_slot = slot;
    // MinHook is shared by the whole DLL, and the battle code may have initialized it already.
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        gc::log("FRAME_PROBE initialize rejected: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(target, &frame_entry, &trampoline);
    if (status == MH_OK) status = MH_EnableHook(target);
    gc::log("FRAME_PROBE install=%s target=%p caller=%08lX", MH_StatusToString(status), target, caller);
    // The hook is never removed, even after a failed enable: another thread may be inside it.
    return status == MH_OK;
}
}

namespace gc {
// Worker side of the ring: takes the oldest sample, if there is one.
bool take_frame_sample(FrameSample& sample) {
    const uint32_t r = consumed.load(std::memory_order_relaxed);
    if (r == written.load(std::memory_order_acquire)) return false;
    sample = samples[r % capacity];
    consumed.store(r + 1, std::memory_order_release);
    return true;
}
uint32_t frame_dropped() { return dropped.load(std::memory_order_relaxed); }

// Hooks the function the frame driver calls once per frame (RVA 0x13AE90). Its first bytes, with the
// relocated flag address patched in, and the calling instruction must match exactly.
bool install_frame_probe(HMODULE game) {
    auto base = reinterpret_cast<unsigned char*>(game);
    unsigned char expected[16] = {0x51, 0x80, 0x3d, 0xb0, 0x63, 0xe5, 0x01, 0x00,
                                  0x74, 0x16, 0xe8, 0x21, 0xff, 0xff, 0xff, 0xc6};
    const DWORD flag = reinterpret_cast<DWORD>(base) + 0x1a563b0;
    memcpy(expected + 3, &flag, 4);
    const unsigned char call[5] = {0xe8, 0xf4, 0x33, 0xf2, 0xff};
    unsigned char actual[16], caller[5];
    if (!read_memory(base + 0x13ae90, actual, sizeof actual) || memcmp(actual, expected, sizeof actual) ||
        !read_memory(base + 0x217a97, caller, sizeof caller) || memcmp(caller, call, sizeof caller)) {
        log("FRAME_PROBE REJECT entry/caller guard mismatch");
        return false;
    }
    // caller is the return address after that call; the slot holds the current Lua state.
    return install(base + 0x13ae90, reinterpret_cast<DWORD>(base) + 0x217a9c,
                   reinterpret_cast<DWORD*>(base + 0x1a579a0));
}

// Debug builds log every changed sample and a summary every 10 s.
void drain_frame_probe() {
#if GC_DEV
    static FrameSample previous{};
    static bool have_previous = false;
    static uint64_t total = 0, mismatches = 0;
    static ULONGLONG last_report = 0;
    FrameSample sample;
    // At most one ring's worth per pass, even if frames keep arriving.
    for (unsigned i = 0; i < capacity && take_frame_sample(sample); ++i) {
        ++total;
        if (sample.caller != expected_caller) ++mismatches;
        if (!have_previous || memcmp(&sample, &previous, offsetof(FrameSample, tick))) {
            log("FRAME_STATE sample_ms=%llu game_tid=%lu caller=%08lX expected=%d context=%08lX readable=%lu top=%08lX base=%08lX raw10=%08lX raw14=%08lX raw18=%08lX raw1c=%08lX raw20=%08lX raw24=%08lX raw28=%08lX raw2c=%08lX",
                sample.tick, sample.thread, sample.caller, sample.caller == expected_caller, sample.context,
                sample.readable, sample.header[2], sample.header[3], sample.header[4], sample.header[5],
                sample.header[6], sample.header[7], sample.header[8], sample.header[9], sample.header[10],
                sample.header[11]);
            previous = sample;
            have_previous = true;
        }
    }
    const auto now = GetTickCount64();
    if (total && (!last_report || now - last_report >= 10000)) {
        log("FRAME_PROBE_ALIVE sampled=%llu dropped=%u caller_mismatches=%llu", total, frame_dropped(), mismatches);
        last_report = now;
    }
#else
    // Release builds log one line, once the hook runs on the game thread.
    static bool reported = false;
    FrameSample sample;
    for (unsigned i = 0; i < capacity && take_frame_sample(sample); ++i) {
        if (!reported && sample.caller == expected_caller) {
            log("FRAME_PROBE_RUNNING game_tid=%lu expected=1", sample.thread);
            reported = true;
        }
    }
#endif
}
#if GC_FRAME_TEST
// Test builds: hook an arbitrary function.
bool test_install_frame_probe(void* target, DWORD caller, DWORD* slot) { return install(target, caller, slot); }
#endif
}
