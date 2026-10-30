// Observers on the game's imports: the scripts' channel through FindFirstFileA, engine debug output
// and the C runtime's exit functions.
#include "diagnostics.h"
#include "exit_fault.h"
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace gc {
// The real imports. FindFirstFileA carries the scripts' GCOBS channel; OutputDebugStringA and W
// carry the engine's messages.
using Find = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using DebugA = void(WINAPI*)(LPCSTR);
using DebugW = void(WINAPI*)(LPCWSTR);
static Find real_find = nullptr;
static DebugA real_a = nullptr;
static DebugW real_w = nullptr;
static thread_local bool logging = false;  // stops re-entry while an ENGINE line is logged

// The scripts talk to the DLL by asking FindFirstFileA about "GCOBS:" paths. Tags are logged and
// dispatched; questions are answered by finding a real file (yes) or not (no).
static HANDLE WINAPI find_observer(LPCSTR path, LPWIN32_FIND_DATAA data) {
    const DWORD saved = GetLastError();
    char text[256]{};
    if (path) {
        for (size_t i = 0; i < sizeof(text) - 1; ++i) {
            if (!read_memory(path + i, &text[i], 1) || !text[i]) break;
        }
        // A question comes from ScriptCB_IsFileExist, which closes the handle itself. Asked every frame
        // of a popup, so not logged. Yes looks up the game's own executable, which always exists.
        const char* menu_question = nullptr;
        menu_question = strstr(text, "GCOBS:ask:");
        if (strstr(text, "GCOBS:hold:ask") || menu_question) {
            static char exe[MAX_PATH]{};
            bool yes = false;
            // Each feature answers its own questions; GCOBS:hold:ask is the ready check.
            if (menu_question && !strncmp(menu_question + 10, "battle_", 7)) yes = battle_answer(menu_question + 10);
            else if (menu_question && !strncmp(menu_question + 10, "galaxy_", 7))
                yes = galaxy_answer(menu_question + 10);
            else if (menu_question && !strncmp(menu_question + 10, "versus", 6))
                yes = versus_answer(menu_question + 10);
            else yes = menu_question ? menu_answer(menu_question + 10) : hold_wanted();
            if (yes && (exe[0] || GetModuleFileNameA(nullptr, exe, MAX_PATH))) {
                SetLastError(saved);
                return real_find(exe, data);
            }
            SetLastError(saved);
            return real_find(path, data);
        }
        // The shell probing for custom_gc_0.lvl is the moment to add the Online menu.
        if (strstr(text, "custom_gc_0.lvl")) {
            log("CHANNEL_SHELL %s", text);
            shell_probe();
            SetLastError(saved);
            return real_find(path, data);
        }
        // Tags are found anywhere in the path, because the engine can prepend its search directory.
        if (const char* tag = strstr(text, "GCOBS:")) {
            log("CHANNEL %s", tag);
            static volatile LONG path_reported = 0;
            if (InterlockedCompareExchange(&path_reported, 1, 0) == 0) log("CHANNEL_PATH %s", text);
            capture_channel(tag);
            if (!strncmp(tag, "GCOBS:hold:", 11)) hold_event(tag + 11);
            if (!strncmp(tag, "GCOBS:menu:", 11)) menu_event(tag + 11);
            if (!strncmp(tag, "GCOBS:battle:", 13)) battle_event(tag + 13);
            if (!strncmp(tag, "GCOBS:versus:", 13)) versus_event(tag + 13);
        } else if (!strncmp(text, "GC", 2)) log("CHANNEL %s", text);
    }
    SetLastError(saved);
    return real_find(path, data);
}

// Logs the engine's debug output as ENGINE lines.
static void WINAPI debug_a(LPCSTR text) {
    DWORD saved = GetLastError();
    if (!logging && text) {
        logging = true;
        char copy[1024]{};
        for (size_t i = 0; i < sizeof(copy) - 1; ++i) {
            if (!read_memory(text + i, copy + i, 1) || !copy[i]) break;
        }
        log("ENGINE %s", copy);
        logging = false;
        // A display-mode reset comes with every shell rebuild; it is only counted.
        if (strstr(copy, "pcSetDisplayMode")) note_shell_reload();
    }
    SetLastError(saved);
    real_a(text);
}

// The same for wide strings, logged as UTF-8.
static void WINAPI debug_w(LPCWSTR text) {
    DWORD saved = GetLastError();
    if (!logging && text) {
        logging = true;
        wchar_t copy[512]{};
        char utf8[2048]{};
        for (size_t i = 0; i < 511; ++i) {
            if (!read_memory(text + i, copy + i, sizeof(wchar_t)) || !copy[i]) break;
        }
        WideCharToMultiByte(CP_UTF8, 0, copy, -1, utf8, sizeof utf8, nullptr, nullptr);
        log("ENGINE %s", utf8);
        logging = false;
    }
    SetLastError(saved);
    real_w(text);
}

// The game's exits through its C runtime (MSVCR120): exit, _exit, __crtTerminateProcess and
// _amsg_exit. Each is logged with its code, caller and the Lua state's top string, then goes ahead.
using CExit = void(__cdecl*)(int);
static CExit real_exit = nullptr, real_quick_exit = nullptr, real_crt_terminate = nullptr, real_runtime_error = nullptr;
static void exit_line(const char* how, int code, void* caller) {
    char lua[160]{};
    current_lua_text(lua, sizeof lua);
    log("PROCESS_EXIT %s code=%d caller=%p lua=%s", how, code, caller, lua[0] ? lua : "-");
}
// An _exit from the game's top-level exception handler follows a fault it swallowed. The fault is
// read back (exit_fault.h) before anything else reuses that stack, and logged after the exit line.
static uintptr_t game_image = 0;
static DWORD game_image_size = 0;
static fault::Fault swallowed;  // static: a local could overwrite the stack being read
// read_memory in the form exit_fault.h expects.
static bool read_quietly(uint32_t address, void* out, size_t size) {
    return read_memory(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), out, size);
}
// Names an address: game:<preferred-base address>, module+offset, or the bare value.
static void where(DWORD address, char* out, size_t size) {
    if (game_image && address >= game_image && address - game_image < game_image_size) {
        _snprintf_s(out, size, _TRUNCATE, "game:%08lX", static_cast<DWORD>(address - game_image + 0x00400000));
        return;
    }
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(address)), &module) &&
        module && GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* name = strrchr(path, '\\');
        _snprintf_s(out, size, _TRUNCATE, "%s+%lX", name ? name + 1 : path,
                    static_cast<DWORD>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    _snprintf_s(out, size, _TRUNCATE, "%08lX", address);
}
// Logs the swallowed fault: code and location, registers, the first stack words and the call chain.
static void fault_lines(const fault::Fault& f) {
    char at[MAX_PATH + 16]{}, target[MAX_PATH + 16]{};
    where(f.address, at, sizeof at);
    if (f.access_known) where(f.target, target, sizeof target);
    log("PROCESS_FAULT code=%08lX at=%s address=%08lX access=%s target=%s", f.code, at, f.address,
        !f.access_known ? "-"
        : f.access == 0 ? "read"
        : f.access == 1 ? "write"
        : f.access == 8 ? "execute"
                        : "other",
        f.access_known ? target : "-");
    if (!f.context_ok) return;
    log("PROCESS_FAULT_REGISTERS eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX",
        f.eip, f.esp, f.ebp, f.eax, f.ebx, f.ecx, f.edx, f.esi, f.edi);
    // Stack words inside the game's image show as game: addresses, the rest as plain values.
    char line[24 * 24]{};
    size_t used = 0;
    uint32_t words[24]{};
    if (read_quietly(f.esp, words, sizeof words)) {
        for (uint32_t w : words) {
            char one[MAX_PATH + 24]{};
            if (game_image && w >= game_image && w - game_image < game_image_size)
                _snprintf_s(one, sizeof one, _TRUNCATE, " game:%08lX", static_cast<DWORD>(w - game_image + 0x00400000));
            else _snprintf_s(one, sizeof one, _TRUNCATE, " %08lX", static_cast<DWORD>(w));
            const size_t n = strlen(one);
            if (used + n >= sizeof line) break;
            memcpy(line + used, one, n + 1);
            used += n;
        }
        log("PROCESS_FAULT_STACK esp+0:%s", line);
    }
    uint32_t returns[16]{};
    const size_t count = fault::frame_chain(f.esp, f.ebp, read_quietly, returns, 16);
    used = 0;
    line[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        char one[MAX_PATH + 24]{};
        where(returns[i], one + 1, sizeof one - 1);
        one[0] = ' ';
        const size_t n = strlen(one);
        if (used + n >= sizeof line) break;
        memcpy(line + used, one, n + 1);
        used += n;
    }
    log("PROCESS_FAULT_CALLS frames=%u%s", static_cast<unsigned>(count), line);
}
// Replacements for the runtime's exit functions: log the exit, then call the original.
static void __cdecl exit_observer(int code) {
    exit_line("exit", code, _ReturnAddress());
    real_exit(code);
}
// _exit also reads back the fault when it is called from the game's exception handler.
static void __cdecl quick_exit_observer(int code) {
    void* caller = _ReturnAddress();
    const bool faulted =
        game_image && reinterpret_cast<uintptr_t>(caller) == game_image + fault::handler_return_rva &&
        fault::read_fault(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())), read_quietly,
                          swallowed);
    exit_line("_exit", code, caller);
    if (faulted) fault_lines(swallowed);
    real_quick_exit(code);
}
static void __cdecl crt_terminate_observer(int code) {
    exit_line("__crtTerminateProcess", code, _ReturnAddress());
    real_crt_terminate(code);
}
static void __cdecl runtime_error_observer(int code) {
    exit_line("_amsg_exit", code, _ReturnAddress());
    real_runtime_error(code);
}

// Swaps an import slot to replacement. The original is stored first, so the hook can always call it.
template <class T> static bool replace(void** slot, T& original, T replacement) {
    original = reinterpret_cast<T>(*slot);
    if (!original || original == replacement) return false;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement));
    DWORD unused;
    if (!VirtualProtect(slot, sizeof(void*), protection, &unused))
        log("IAT protection restore failed: %lu", GetLastError());
    return true;
}

// Swaps the import slots of the observed functions. Runs only after verify_image accepted the game,
// and changes no code bytes.
void install_observers(HMODULE module) {
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!directory.VirtualAddress || directory.VirtualAddress >= size ||
        directory.Size > size - directory.VirtualAddress)
        return;
    bool fa = false, da = false, dw = false;
    bool ex = false, qx = false, ct = false, rt = false;
    game_image = reinterpret_cast<uintptr_t>(base);
    game_image_size = size;
    for (DWORD off = 0; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= directory.Size;
         off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress + off);
        if (!desc->Name) break;
        if (!desc->OriginalFirstThunk || desc->OriginalFirstThunk >= size || desc->FirstThunk >= size) continue;
        for (DWORD i = 0; i < (size - desc->OriginalFirstThunk) / 4 && i < (size - desc->FirstThunk) / 4; ++i) {
            DWORD name = reinterpret_cast<DWORD*>(base + desc->OriginalFirstThunk)[i];
            if (!name) break;
            if (IMAGE_SNAP_BY_ORDINAL32(name) || name + 2 >= size) continue;
            char imported[64]{};
            if (!read_memory(base + name + 2, imported, sizeof imported - 1)) continue;
            auto slot = reinterpret_cast<void**>(base + desc->FirstThunk + 4 * i);
            if (!fa && !strcmp(imported, "FindFirstFileA")) fa = replace(slot, real_find, find_observer);
            if (!da && !strcmp(imported, "OutputDebugStringA")) da = replace(slot, real_a, debug_a);
            if (!dw && !strcmp(imported, "OutputDebugStringW")) dw = replace(slot, real_w, debug_w);
            if (!ex && !strcmp(imported, "exit")) ex = replace(slot, real_exit, exit_observer);
            if (!qx && !strcmp(imported, "_exit")) qx = replace(slot, real_quick_exit, quick_exit_observer);
            if (!ct && !strcmp(imported, "__crtTerminateProcess"))
                ct = replace(slot, real_crt_terminate, crt_terminate_observer);
            if (!rt && !strcmp(imported, "_amsg_exit")) rt = replace(slot, real_runtime_error, runtime_error_observer);
        }
    }
    log("IAT observation FindFirstFileA=%d OutputDebugStringA=%d W=%d", fa, da, dw);
    log("IAT exit observation exit=%d _exit=%d __crtTerminateProcess=%d _amsg_exit=%d", ex, qx, ct, rt);
}
}
