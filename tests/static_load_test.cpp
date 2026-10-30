// Loads the proxy dinput8.dll as the game does: through this exe's import table, from the exe's folder.
// Checks DirectInput forwarding, the import-time load in the log, and the refusal of a non-game exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <string>

extern "C" __declspec(dllimport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
// IID_IDirectInput8A.
static const GUID iid_input8a = {0xbf798030, 0x483a, 0x4da2, {0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00}};

// This process's newest proxy log.
static std::string newest_log() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = L'\0';
    const std::wstring folder = std::wstring(dir) + L"\\gct\\logs\\";
    const std::wstring pattern = folder + L"game-" + std::to_wstring(GetCurrentProcessId()) + L"-*.log";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return {};
    FindClose(find);
    // The proxy keeps the log open for appending, so share read and write.
    HANDLE f = CreateFileW((folder + data.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(f, buffer, sizeof buffer, &n, nullptr) && n) text.append(buffer, n);
    CloseHandle(f);
    return text;
}

int main() {
    IUnknown* input = nullptr;
    const HRESULT hr =
        DirectInput8Create(GetModuleHandleW(nullptr), 0x0800, iid_input8a, reinterpret_cast<void**>(&input), nullptr);
    if (FAILED(hr) || !input) {
        std::printf("FAIL: forwarded DirectInput8Create hr=%08lX\n", hr);
        return 1;
    }
    input->Release();
    // The proxy loads the System32 DLL too, so find the module that owns the bound import.
    wchar_t proxy[MAX_PATH]{};
    HMODULE bound = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DirectInput8Create), &bound);
    GetModuleFileNameW(bound, proxy, MAX_PATH);
    std::string log;
    // Up to 5 s for the proxy to log its refusal.
    for (int i = 0; i < 50 && log.find("PROFILE REJECT") == std::string::npos; ++i) {
        Sleep(100);
        log = newest_log();
    }
    const bool static_load = log.find("PLAYER_LOAD loaded_with_game=1") != std::string::npos;
    const bool refused = log.find("PROFILE REJECT") != std::string::npos;
    // The loader keeps the importer's spelling (DINPUT8.dll), so compare paths case-insensitively.
    wchar_t local[MAX_PATH]{};
    GetModuleFileNameW(nullptr, local, MAX_PATH);
    wchar_t* name = wcsrchr(local, L'\\') + 1;
    wcscpy_s(name, MAX_PATH - (name - local), L"dinput8.dll");
    const bool local_proxy = !_wcsicmp(proxy, local);
    if (!static_load || !refused || !local_proxy) {
        std::printf("FAIL: static_load=%d refused=%d local_proxy=%d proxy=%ls\n%s", static_load, refused, local_proxy,
                    proxy, log.c_str());
        return 1;
    }
    std::printf(
        "PASS: import-time load from the application folder, DirectInput forwarded, non-game executable refused\n");
    return 0;
}
