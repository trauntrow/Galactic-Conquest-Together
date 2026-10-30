// Tests the Versus bots hook (src/bots_hook.h) through MinHook, on a stand-in for the game's
// player-team check with the same calling convention and first instructions.
#include "../src/bots_hook.h"
#include "../third_party/minhook/include/MinHook.h"
#include <cstdio>

static_assert(sizeof(void*) == 4, "the hook is x86");

#include "check.h"

// The stand-in's local player team; the game reads its own from the character table.
extern "C" unsigned local_team = 2;

// The stand-in: AL 1 when the team in ECX is the local player's, else 0; the rest of EAX is arbitrary.
extern "C" __declspec(naked) void standin_player_team() {
    __asm {
        mov eax, dword ptr [local_team]
        push esi
        push edi
        mov edi, ecx
        cmp edi, eax
        jne other
        pop edi
        pop esi
        mov eax, 0x12345601  // AL 1
        ret
    other:
        pop edi
        pop esi
        mov eax, 0xdeadbe00  // AL 0
        ret
    }
}

// Calls it as the game does: the team in ECX, the answer in AL, and EBX, ESI and EDI kept.
// Returns 0xFF when one of those registers changed.
static unsigned ask(unsigned team) {
    unsigned result = 0;
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, 0x11111111
        mov esi, 0x22222222
        mov edi, 0x33333333
        mov ecx, team
        call standin_player_team
        movzx eax, al
        cmp ebx, 0x11111111
        jne bad
        cmp esi, 0x22222222
        jne bad
        cmp edi, 0x33333333
        jne bad
        jmp done
    bad:
        mov eax, 0xff
    done:
        pop edi
        pop esi
        pop ebx
        mov result, eax
    }
    return result;
}

int main() {
    // The game's own answers, before any hook.
    CHECK(ask(2) == 1 && ask(1) == 0);
    // Hooked through MinHook, as the DLL hooks the game.
    MH_STATUS status = MH_Initialize();
    CHECK(status == MH_OK);
    void* original = nullptr;
    status = MH_CreateHook(reinterpret_cast<void*>(&standin_player_team),
                           reinterpret_cast<void*>(&gc::bots::player_team), &original);
    CHECK(status == MH_OK && original);
    gc::bots::original = reinterpret_cast<gc::bots::PlayerTeamFn>(original);
    CHECK(MH_EnableHook(reinterpret_cast<void*>(&standin_player_team)) == MH_OK);
    // No team chosen (co-op, a joined battle, the galaxy): the game's own answer.
    CHECK(ask(2) == 1 && ask(1) == 0);
    local_team = 1;
    CHECK(ask(1) == 1 && ask(2) == 0);
    // A Versus battle this PC hosts: its chosen team, whatever the game says.
    gc::bots::chosen = 2;
    CHECK(ask(2) == 1 && ask(1) == 0 && ask(3) == 0);
    gc::bots::chosen = 1;
    CHECK(ask(1) == 1 && ask(2) == 0);
    // Unchosen again, and then unhooked: the game's own answers.
    gc::bots::chosen = 0;
    local_team = 2;
    CHECK(ask(2) == 1 && ask(1) == 0);
    CHECK(MH_DisableHook(reinterpret_cast<void*>(&standin_player_team)) == MH_OK);
    CHECK(ask(2) == 1 && ask(1) == 0);
    MH_Uninitialize();
    if (failures) {
        std::printf("bots_hook_test FAILED %d\n", failures);
        return 1;
    }
    std::printf("bots_hook_test PASS: the chosen team when set, the game's own answer otherwise, registers kept\n");
    return 0;
}
