#pragma once
// Logging, the game-build check, and the calls through which the hooks, the Lua bridge and the
// Steam thread share state.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "battle_logic.h"

namespace gc {
// Opens this session's log file.
bool open_log(HMODULE source_module = nullptr);
// Appends a timestamped line. Never waits long: under heavy contention the line is dropped.
void log(const char* format, ...);
// Reads memory without faulting; false if any byte is unreadable.
bool read_memory(const void* address, void* output, size_t size);
// Lowercase hex SHA-256 of a file, or "" if it cannot be read.
std::string sha256_file(const wchar_t* path);
// Accepts only the supported game build. relocated: module runs at its own base (false for a mapped
// view, whose header holds the base); process: where module lives.
bool verify_image(HMODULE module, const wchar_t* file, bool relocated, HANDLE process = GetCurrentProcess());
// Hooks the game's imports (observers.cpp).
void install_observers(HMODULE module);
// A shell rebuild recreates the interface tables, which orphans any installed script wrapper.
// Rebuilds are only counted, never acted on.
void note_shell_reload();
unsigned shell_reload_count();
// Where this PC's game is, as a session::Place (0 unknown, 1 menus, 2 galaxy). Written by the frame
// hook, read by the Steam thread.
void note_place(int place);
int game_place();
// Debug builds: logs engine globals when they change.
void poll_state(HMODULE module);
// Feeds one GCOBS channel tag to the in-process capture validator (capture_live.cpp).
void capture_channel(const char* tag);
// Co-op ready check (steam_invite.cpp): whether the host's galaxy waits for a joining friend, asked
// by player_hold.lua, and that popup's events ("shown", "released", "timeout", "cancel").
bool hold_wanted();
void hold_event(const char* event);
// The Online menu: shell_probe adds it when the shell probes custom_gc_0.lvl (lua_query.cpp);
// menu_event and menu_answer take its GCOBS:menu:* tags and GCOBS:ask:* questions (steam_invite.cpp).
void shell_probe();
void menu_event(const char* event);
bool menu_answer(const char* question);
// The battles= setting from gct.ini, and whether this PC hosts a paired co-op game.
battle::Mode battle_mode();
bool coop_host_paired();
// The host's party screen rows while it has a co-op lobby, with a number that changes with them.
bool party_rows(unsigned& seq, std::string& text);
// The shell script's GCOBS:battle:* reports and GCOBS:ask:battle_* questions (lua_query.cpp).
void battle_event(const char* event);
bool battle_answer(const char* question);
// The friend's galaxy lock (lua_query.cpp): GCOBS:ask:galaxy_* questions, such as whether the
// host's galaxy shows on this PC or whether applying it has stopped.
bool galaxy_answer(const char* question);
// Versus (steam_invite.cpp): GCOBS:versus:* reports and GCOBS:ask:versus_* questions; whether this
// game is Versus (until it is back at the menus) and whether this PC is the session host.
void versus_event(const char* event);
bool versus_answer(const char* question);
bool versus_active();
bool versus_session_host();
// Whether a Versus session is ready, on either PC.
bool versus_paired();
// Defender's PC: the other PC's card request not shown yet, as its number and "<mission>", a line
// break, then three comma-separated cards. versus_card_taken marks it shown.
bool versus_card_pending(unsigned& number, std::string& data);
void versus_card_taken(unsigned number);
// Debug builds: logs faults without handling them.
void start_exception_observer();
// The current Lua state's top value if it is a string, for the game's exit log line (lua_query.cpp).
void current_lua_text(char* out, size_t size);
}
