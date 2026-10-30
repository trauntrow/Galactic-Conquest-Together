// Logging, gct.ini settings, the game-build check, and debug-build observers for faults and state.
#include "diagnostics.h"
#include "profile_data.h"
#include "battle_logic.h"
#include "sha256.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <array>

namespace gc {
static SRWLOCK log_lock = SRWLOCK_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;
// Defined here, not in observers.cpp, so test binaries without the observers still link.
static volatile LONG shell_reloads = 0;

static volatile LONG place_now = 0;  // a session::Place value

// gct.ini's battles= value, set once in open_log before any hook runs.
static battle::Mode battles_setting = battle::default_mode;
battle::Mode battle_mode() { return battles_setting; }

void note_shell_reload() { InterlockedIncrement(&shell_reloads); }
void note_place(int place) { InterlockedExchange(&place_now, place); }
int game_place() { return static_cast<int>(InterlockedCompareExchange(&place_now, 0, 0)); }
unsigned shell_reload_count() { return static_cast<unsigned>(InterlockedCompareExchange(&shell_reloads, 0, 0)); }

#if GC_PLAYER
// The optional gct/gct.ini, written with its defaults on first run so players can find it. battles=
// stays commented out, so a file that never sets it follows the default even if that changes.
static const char default_settings[] =
    "; Galactic Conquest Together settings. Delete this file to restore the defaults.\r\n"
    "[gct]\r\n"
    "; Write a log of each game session to gct\\logs: on or off.\r\n"
    "log=on\r\n"
    "; How many session logs to keep; older ones are deleted when the game starts. 0 keeps all.\r\n"
    "keep_logs=20\r\n"
    "; Galactic Conquest battles: shared (the default: in a co-op game both players fight\r\n"
    "; each battle together), single (the host fights alone) or hosted (every battle; a test).\r\n"
    "; battles=shared\r\n";
struct PlayerSettings {
    bool log = true;
    unsigned keep_logs = 20;
    battle::Mode battles = battle::default_mode;
};

// Creates gct.ini if it is missing, then reads it. Bad or missing values fall back to the defaults.
static PlayerSettings read_settings(const std::wstring& gct) {
    const std::wstring ini = gct + L"\\gct.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, default_settings, sizeof default_settings - 1, &written, nullptr);
            CloseHandle(f);
        }
    }
    PlayerSettings s;
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"gct", L"log", L"on", value, 16, ini.c_str());
    s.log = _wcsicmp(value, L"off") && _wcsicmp(value, L"0") && _wcsicmp(value, L"no") && _wcsicmp(value, L"false");
    const UINT keep = GetPrivateProfileIntW(L"gct", L"keep_logs", 20, ini.c_str());
    s.keep_logs = keep <= 10000 ? keep : 20;  // a negative entry reads back as a huge number
    wchar_t mode[16]{};
    char narrow[16]{};
    GetPrivateProfileStringW(L"gct", L"battles", L"", mode, 16, ini.c_str());  // empty if missing: the default
    for (size_t i = 0; i < 15 && mode[i]; ++i) narrow[i] = mode[i] < 0x80 ? static_cast<char>(mode[i]) : '?';
    s.battles = battle::parse_mode(narrow);
    return s;
}

// Deletes the oldest game-*.log files so that `keep` remain with the new one. A log that another
// running game holds open cannot be deleted and stays.
static unsigned prune_logs(const std::wstring& logs, unsigned keep) {
    if (!keep) return 0;
    std::vector<std::pair<ULONGLONG, std::wstring>> found;
    WIN32_FIND_DATAW d{};
    HANDLE h = FindFirstFileW((logs + L"\\game-*.log").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG when =
            (static_cast<ULONGLONG>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        found.emplace_back(when, d.cFileName);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    unsigned deleted = 0;
    for (size_t i = keep - 1; i < found.size(); ++i)
        if (DeleteFileW((logs + L"\\" + found[i].second).c_str())) ++deleted;
    return deleted;
}
#endif

// Opens a new log file in logs beside source_module (gct/logs in Player builds, which read gct.ini
// first), or with no module, one appending file under LOCALAPPDATA.
bool open_log(HMODULE source_module) {
    wchar_t dir[32768];
    if (source_module) {
        const DWORD n = GetModuleFileNameW(source_module, dir, 32000);
        if (!n || n >= 32000) return false;
        wchar_t* end = wcsrchr(dir, L'\\');
        if (!end) return false;
        *end = L'\0';
#if GC_PLAYER
        // Everything this DLL writes stays in one gct folder beside it.
        wcscat_s(dir, L"\\gct");
        if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const PlayerSettings settings = read_settings(dir);
        battles_setting = settings.battles;
#endif
        wcscat_s(dir, L"\\logs");
        if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#if GC_PLAYER
        const unsigned pruned = prune_logs(dir, settings.log ? settings.keep_logs : 0);
        if (!settings.log) return true;  // logging off: everything else still runs
#endif
        wchar_t name[96];
        swprintf_s(name, L"\\game-%lu-%llu.log", GetCurrentProcessId(), GetTickCount64());
        wcscat_s(dir, name);
        log_file = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
#if GC_PLAYER
        if (log_file != INVALID_HANDLE_VALUE)
            log("PLAYER_SETTINGS gct.ini log=on keep_logs=%u battles=%s older_logs_deleted=%u", settings.keep_logs,
                battle::mode_name(settings.battles), pruned);
#endif
        return log_file != INVALID_HANDLE_VALUE;
    }
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, 32000);
    if (!n || n >= 32000) return false;
    wcscat_s(dir, L"\\SWBF2_Coop");
    CreateDirectoryW(dir, nullptr);
#if GC_DEV
    wcscat_s(dir, L"\\rebuild_dev.log");
#else
    wcscat_s(dir, L"\\rebuild_release.log");
#endif
    log_file = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return log_file != INVALID_HANDLE_VALUE;
}

// Appends one line: UTC time, tick count, process and thread ids, then the formatted text.
void log(const char* format, ...) {
    char line[2048];
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    int prefix = sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%llu pid=%lu tid=%lu] ", utc.wYear, utc.wMonth,
                           utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, GetTickCount64(),
                           GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, format);
    _vsnprintf_s(line + prefix, sizeof(line) - prefix - 3, _TRUNCATE, format, ap);
    va_end(ap);
    strcat_s(line, "\r\n");
    // Never block a game or exception thread behind another writer: retry the lock briefly, about one
    // WriteFile's worth, then drop the line.
    bool held = false;
    for (int tries = 0; tries < 64 && !(held = TryAcquireSRWLockExclusive(&log_lock) != 0); ++tries) SwitchToThread();
    if (!held) return;
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(log_file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&log_lock);
}

bool read_memory(const void* address, void* output, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &read) && read == size;
}

std::string sha256_file(const wchar_t* path) {
    HANDLE f =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Sha256 hash;
    std::array<unsigned char, 65536> buffer{};
    bool ok = true;
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(f, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr)) {
            ok = false;
            break;
        }
        if (!n) break;
        hash.update(buffer.data(), n);
    }
    CloseHandle(f);
    if (!ok) return {};
    const std::array<unsigned char, 32> digest = hash.finish();
    char hex[65];
    for (size_t i = 0; i < digest.size(); ++i) sprintf_s(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

// Accepts the module only if it is the supported build: exact file hash, PE header fields, and each
// guard's code bytes with relocated operands moved back to the preferred base.
bool verify_image(HMODULE module, const wchar_t* file, bool relocated, HANDLE process) {
    const auto read = [process](const void* address, void* output, size_t size) {
        SIZE_T count = 0;
        return ReadProcessMemory(process, address, output, size, &count) && count == size;
    };
    const auto hash = sha256_file(file);
    if (hash != expected_sha256) {
        log("PROFILE REJECT: file hash %s", hash.empty() ? "unreadable" : hash.c_str());
        return false;
    }
    auto base = reinterpret_cast<const unsigned char*>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x100000 || !read(base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.TimeDateStamp != expected_timestamp || nt.OptionalHeader.SizeOfImage != expected_image_size) {
        log("PROFILE REJECT: loaded PE metadata");
        return false;
    }
    // Slide from the preferred base 0x400000: from the actual base for a running module, or from the
    // header's ImageBase for a mapped view, which Windows may already have relocated.
    const uint32_t slide =
        (relocated ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)) : nt.OptionalHeader.ImageBase) - 0x400000;
    log("IMAGE base=%p preferred=%08lX slide=%08X", module, nt.OptionalHeader.ImageBase, slide);
    // Collect the HIGHLOW fixups, so relocated operands inside the guards can be compared.
    std::vector<uint32_t> fixups;
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (slide && directory.Size) {
        if (directory.VirtualAddress > expected_image_size ||
            directory.Size > expected_image_size - directory.VirtualAddress)
            return false;
        uint32_t pos = 0;
        while (pos < directory.Size) {
            IMAGE_BASE_RELOCATION block{};
            if (directory.Size - pos < sizeof block ||
                !read(base + directory.VirtualAddress + pos, &block, sizeof block) ||
                block.SizeOfBlock < sizeof block || block.SizeOfBlock > directory.Size - pos || (block.SizeOfBlock % 2))
                return false;
            for (uint32_t j = sizeof block; j < block.SizeOfBlock; j += 2) {
                WORD entry;
                if (!read(base + directory.VirtualAddress + pos + j, &entry, sizeof entry)) return false;
                if ((entry >> 12) == IMAGE_REL_BASED_HIGHLOW) fixups.push_back(block.VirtualAddress + (entry & 0xfff));
                else if ((entry >> 12) != IMAGE_REL_BASED_ABSOLUTE) return false;
            }
            pos += block.SizeOfBlock;
        }
    }
    bool all = true;
    for (const auto& guard : guards) {
        unsigned char actual[16];
        if (!read(base + guard.rva, actual, sizeof actual)) return false;
        // Undo the slide on operands that overlap the guard, in the copy only.
        for (uint32_t fix : fixups) {
            if (fix + 4 > guard.rva && fix < guard.rva + 16) {
                if (fix > expected_image_size - 4) return false;
                uint32_t value;
                if (!read(base + fix, &value, 4)) return false;
                value -= slide;
                const auto bytes = reinterpret_cast<const unsigned char*>(&value);
                for (uint32_t j = 0; j < 4; ++j)
                    if (fix + j >= guard.rva && fix + j < guard.rva + 16) actual[fix + j - guard.rva] = bytes[j];
            }
        }
        const bool match = !memcmp(guard.bytes, actual, sizeof actual);
        log("GUARD %s RVA=%08X %s", guard.name, guard.rva, match ? "MATCH" : "MISMATCH");
        if (!match) {
            char hex[49]{};
            for (size_t i = 0; i < 16; ++i) sprintf_s(hex + 3 * i, 4, "%02X ", actual[i]);
            log(" normalized actual: %s", hex);
        }
        all &= match;
    }
    log("PROFILE %s", all ? "ACCEPT" : "REJECT");
    return all;
}

#if GC_DEV
// Fatal faults worth logging besides access violations. C++ exceptions are not among them, and a
// stack overflow is left alone because the thread has no room to log.
static bool fatal_code(DWORD code) {
    switch (code) {
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN
        return true;
    default: return false;
    }
}
// Logs access violations, and the first 64 other fatal faults, with a frame-pointer walk of the
// stack. Never handles anything.
static LONG CALLBACK exception_observer(EXCEPTION_POINTERS* info) {
    static std::atomic<unsigned> others{0};
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool av = code == EXCEPTION_ACCESS_VIOLATION;
    if (!av && !(fatal_code(code) && others.fetch_add(1) < 64)) return EXCEPTION_CONTINUE_SEARCH;
    {
        const auto* r = info->ExceptionRecord;
        if (av)
            log("AV ip=%p access=%lu address=%p; continuing search", r->ExceptionAddress,
                r->NumberParameters >= 2 ? r->ExceptionInformation[0] : 0,
                r->NumberParameters >= 2 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr);
        else log("FAULT code=%08lX ip=%p flags=%lu; continuing search", code, r->ExceptionAddress, r->ExceptionFlags);
        DWORD frame = info->ContextRecord->Ebp;
        for (int i = 0; i < 16; ++i) {
            DWORD pair[2];
            if ((frame & 3) || !read_memory(reinterpret_cast<void*>(frame), pair, sizeof pair)) break;
            log("  frame %d return=%08lX", i, pair[1]);
            if (pair[0] <= frame || pair[0] - frame > 0x100000) break;
            frame = pair[0];
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// Debug builds only; the release build installs no exception handler.
void start_exception_observer() {
#if GC_DEV
    log("Read-only exception observer=%p", AddVectoredExceptionHandler(1, exception_observer));
#endif
}

#if GC_DEV
// Logs engine globals whenever they change. For diagnosis only; no feature depends on them.
void poll_state(HMODULE module) {
    // Flow state, current Lua state, render count and two engine flags.
    struct Fields {
        uint32_t flow, lua, count;
        unsigned char suspend, lock;
    };
    static Fields previous{};
    static bool have_previous = false;
    Fields now{};
    const auto base = reinterpret_cast<const unsigned char*>(module);
    if (!read_memory(base + 0x3e7e9c, &now.flow, 4) || !read_memory(base + 0x1a579a0, &now.lua, 4) ||
        !read_memory(base + 0x4f6db8, &now.count, 4) || !read_memory(base + 0x1a56fa3, &now.suspend, 1) ||
        !read_memory(base + 0x1a5605f, &now.lock, 1)) {
        log("State read unavailable");
        return;
    }
    if (!have_previous || now.flow != previous.flow || now.lua != previous.lua || now.count != previous.count ||
        now.suspend != previous.suspend || now.lock != previous.lock) {
        log("STATE flow=%u lua=%08X render_count=%u suspend=%u candidate_lock=%u", now.flow, now.lua, now.count,
            now.suspend, now.lock);
        previous = now;
        have_previous = true;
    }
    // What decides a battle's rules, sides, result and return. galaxy_signin is a GOG Galaxy sign-in
    // in progress; one that finishes while lan is set crashes the game.
    struct Battle {
        uint32_t state, rules, side0, side1, bots, missions, mission_index;
        int32_t victory;
        unsigned char metagame, campaign, net_select, net_a8, net_a9, dedicated, host, in_mp, in_net, lan,
            galaxy_signin, loop, random, can_switch, locked, slot0, slot1;
    };
    static Battle before{};
    static bool have_before = false;
    Battle b{};
    auto at = [base](uint32_t va) { return base + (va - 0x400000); };
    const bool read = read_memory(at(0x01e563b8), &b.state, 4) && read_memory(at(0x007e7e9c), &b.rules, 4) &&
                      read_memory(at(0x01e56014), &b.side0, 4) && read_memory(at(0x01e56018), &b.side1, 4) &&
                      read_memory(at(0x007e853c), &b.bots, 4) && read_memory(at(0x01e579c8), &b.missions, 4) &&
                      read_memory(at(0x01e579cc), &b.mission_index, 4) && read_memory(at(0x007e6688), &b.victory, 4) &&
                      read_memory(at(0x01e56033), &b.metagame, 1) && read_memory(at(0x01e5605d), &b.campaign, 1) &&
                      read_memory(at(0x007e8007), &b.net_select, 1) && read_memory(at(0x01e62ea8), &b.net_a8, 1) &&
                      read_memory(at(0x01e62ea9), &b.net_a9, 1) && read_memory(at(0x01e62eaa), &b.dedicated, 1) &&
                      read_memory(at(0x01e57a0d), &b.host, 1) && read_memory(at(0x01ead4b7), &b.in_mp, 1) &&
                      read_memory(at(0x01ead92f), &b.in_net, 1) && read_memory(at(0x01fa2511), &b.lan, 1) &&
                      read_memory(at(0x01e65030), &b.galaxy_signin, 1) && read_memory(at(0x01e579c4), &b.loop, 1) &&
                      read_memory(at(0x01e579c5), &b.random, 1) && read_memory(at(0x007e651f), &b.can_switch, 1) &&
                      read_memory(at(0x007e802d), &b.locked, 1) && read_memory(at(0x01ecdbf8), &b.slot0, 1) &&
                      read_memory(at(0x01ecdbf8 + 0x208), &b.slot1, 1);
    if (read && (!have_before || memcmp(&b, &before, sizeof b))) {
        log("BATTLE_FLAGS state=%08X rules=%u metagame=%u campaign=%u net=%u/%u/%u dedicated=%u host=%u in_mp=%u "
            "in_net=%u lan=%u galaxy_signin=%u sides=%u,%u victory=%d missions=%u/%u loop=%u random=%u bots=%u "
            "can_switch=%u locked=%u slots=%u,%u",
            b.state, b.rules, b.metagame, b.campaign, b.net_select, b.net_a8, b.net_a9, b.dedicated, b.host, b.in_mp,
            b.in_net, b.lan, b.galaxy_signin, b.side0, b.side1, b.victory, b.mission_index, b.missions, b.loop,
            b.random, b.bots, b.can_switch, b.locked, b.slot0, b.slot1);
        before = b;
        have_before = true;
    }
}
#endif
}
