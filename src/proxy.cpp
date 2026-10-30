// dinput8.dll proxy: passes DirectInput8Create to the system DLL and starts the mod's worker thread.
#include "diagnostics.h"
#include "version.h"
#include <unknwn.h>
#include <cstdio>
#include "frame_probe.h"
#include "lua_query.h"
#include "steam_invite.h"

using DirectInput = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static INIT_ONCE forward_once = INIT_ONCE_STATIC_INIT;
static DirectInput real_direct_input = nullptr;

// Loads the system's dinput8.dll once, on the first DirectInput8Create call.
static BOOL CALLBACK load_system_dinput(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 13) return FALSE;
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE dll = LoadLibraryW(path);
    if (!dll) return FALSE;
    real_direct_input = reinterpret_cast<DirectInput>(GetProcAddress(dll, "DirectInput8Create"));
    return real_direct_input != nullptr;
}

// The one function the game imports from dinput8.dll, passed straight through.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out,
                                             LPUNKNOWN outer) {
    if (!InitOnceExecuteOnce(&forward_once, load_system_dinput, nullptr, nullptr)) return E_FAIL;
    return real_direct_input(instance, version, iid, out, outer);
}

// True when the game loaded this DLL at startup as its own dinput8.dll, false if injected later.
static bool loaded_with_game = false;

// The worker thread: opens the log, checks the game build, installs the hooks, then services them.
static DWORD WINAPI worker(void*) {
    // Pin the DLL: hooks can call into it until the process exits.
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&worker), &self))
        return 1;
    if (!gc::open_log(self)) return 1;
#if !GC_DEV
    gc::log("GCT Galactic Conquest Together %s release", GC_VERSION);
#else
    gc::log("GCT debug build; DEV=%d", GC_DEV);
#endif
    gc::log("PLAYER_LOAD loaded_with_game=%d", loaded_with_game);
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
    if (!n || n >= 32768) return 1;
    // Logged before hashing, so a stall can be placed before or after the hash.
    gc::log("PROFILE hashing the game's file");
    HMODULE game = GetModuleHandleW(nullptr);
    if (!gc::verify_image(game, exe, true)) return 1;
    gc::start_exception_observer();
    gc::install_observers(game);
    // Runs on its own thread; if it fails to start, everything else keeps working.
    if (!gc::start_steam_invite(self, game)) gc::log("STEAM_INVITE_START_FAILED");
    if (!gc::initialize_lua_query(game)) return 1;
    if (!gc::install_frame_probe(game)) return 1;
#if GC_DEV
    // Debug builds also log engine state, worker liveness and the GOG Galaxy SDK export.
    using GetGalaxy = void*(__cdecl*)();
    GetGalaxy galaxy = nullptr;
    void* previous = reinterpret_cast<void*>(~uintptr_t(0));
    unsigned ticks = 0;
    for (;;) {
        gc::poll_state(game);
        gc::drain_frame_probe();
        gc::report_lua_query();
        if (ticks++ % 40 == 0) gc::log("CAPTURE_ALIVE worker_ticks=%u", ticks);
        if (!galaxy) {
            HMODULE sdk = GetModuleHandleW(L"Galaxy.dll");
            if (sdk)
                galaxy = reinterpret_cast<GetGalaxy>(
                    GetProcAddress(sdk, "?GetInstance@GalaxyFactory@api@galaxy@@SAPAVIGalaxy@23@XZ"));
        }
        // The SDK export is only looked up and logged, never called from this thread.
        void* current = reinterpret_cast<void*>(galaxy);
        if (current != previous) {
            gc::log("Galaxy GetInstance export=%p", current);
            previous = current;
        }
        Sleep(250);
    }
#else
    // Release: the frame hook steps the Lua bridge only while its ring has room, so keep draining it.
    for (;;) {
        gc::drain_frame_probe();
        gc::report_lua_query();
        Sleep(250);
    }
#endif
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH) {
        loaded_with_game = reserved != nullptr;  // non-null when loaded at process start
        // Work starts on a new thread, away from the loader lock. DisableThreadLibraryCalls is not
        // used: the static CRT and thread_local state need thread notifications.
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
