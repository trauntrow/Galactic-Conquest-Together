#pragma once
// Expected code of the game's SetState callback and helper, checked before the DLL triggers a state change.
// Absolute addresses inside assume the preferred base 0x400000; lua_query.cpp relocates them.
namespace gc {
constexpr unsigned char setstate_callback[] = {85,  139, 236, 81, 106, 0,   106, 1,  255, 53, 160, 121, 229, 1,
                                               232, 189, 247, 16, 0,   131, 196, 12, 141, 77, 252, 80,  232, 17};
constexpr unsigned char setstate_helper[] = {
    198, 5,   176, 99,  229, 1,   1,   133, 201, 116, 66,  129, 249, 1,   252, 225, 17,  116, 37,  129,
    249, 57,  3,   246, 143, 116, 8,   198, 5,   176, 99,  229, 1,   0,   195, 199, 5,   172, 99,  229,
    1,   124, 185, 126, 0,   199, 5,   180, 99,  229, 1,   57,  3,   246, 143, 195, 199, 5,   172, 99,
    229, 1,   152, 185, 126, 0,   199, 5,   180, 99,  229, 1,   1,   252, 225, 17,  195, 199, 5,   172,
    99,  229, 1,   0,   0,   0,   0,   199, 5,   180, 99,  229, 1,   0,   0,   0,   0,   195};
}
