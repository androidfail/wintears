/* Real-time frame step (upstream games/totk/runtime/game_patches.c patch_realtime, TOTK 1.2.1).
   The game's variable frame step (main+0x7a83e4: ratio = interval * 0.5, seconds = interval / 60) and its global ratio
   (main+0x8f68b8) follow the present interval it picks, so a frame that takes longer than the interval runs game time
   slow. These 10 instructions make both read {ratio = seconds * 30, seconds} from a 64-bit value the host writes at each
   present (nvn.c), so game time follows the measured frame time. Upstream's hair / cloth clock veneers are left out: they
   only matter above 30 fps.
   Applied twice, identically: by the runtime to main's text before linking (main.c), and by tools/recomp/wt_recomp to the
   code it recompiles (the recompiled blocks chain into each other directly, so a runtime-only patch would not reach them).
   Each checks every original word first and patches nothing on any mismatch (another game version). */
#pragma once
#include <stdint.h>
#include <string.h>

/* the value's offset from main's base: the host's runtime page + 0x13000 (memory.c: RUNTIME_OFF + RT_OBJECTS + 0x1000) */
#define GP_STEP_OFF 0x0fc13000ull

static inline uint32_t gp_adrp_x9(uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | 9u;
}

/* image: main's bytes, image[0] at main_base. Returns the number of words patched (10, or 0 on a mismatch). */
static inline int gp_apply_realtime(uint8_t *image, uint64_t main_base, uint64_t text_end) {
    static const struct { uint32_t off, original; } s[10] = {
        {0x7a83f4, 0x1e220020}, {0x7a83f8, 0x52a84e08}, {0x7a83fc, 0x1e270101}, {0x7a8400, 0x1e2c1002}, {0x7a840c, 0x1e211801},
        {0x7a8410, 0x1e220800}, {0x8f68b8, 0xbd425e60}, {0x8f68bc, 0x1e2c1001}, {0x8f68cc, 0x5e21d800}, {0x8f68d0, 0x1e210800}};
    const uint32_t nop = 0xd503201f;
    const uint64_t data = main_base + GP_STEP_OFF;
    const uint32_t repl[10] = {gp_adrp_x9(main_base + 0x7a83f4, data), 0xbd400120 /* ldr s0, [x9]: ratio */,
                               0xbd400521 /* ldr s1, [x9, #4]: seconds */, nop, nop, nop,
                               gp_adrp_x9(main_base + 0x8f68b8, data), 0xbd400120 /* ldr s0, [x9]: global ratio */, nop, nop};
    for (int i = 0; i < 10; i++) {
        uint32_t w;
        if (s[i].off + 4 > text_end) return 0;
        memcpy(&w, image + s[i].off, 4);
        if (w != s[i].original && w != repl[i]) return 0;
    }
    for (int i = 0; i < 10; i++) memcpy(image + s[i].off, &repl[i], 4);
    return 10;
}
