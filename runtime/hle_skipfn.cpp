// Measurement only: F1CE_SKIP_FN=<hex>,<hex> makes the listed render functions return at once, to price a pass by its
// fps (as F1CE_CUBE_SKIP did for the cube map). The hooks also count calls (log `skipfn:` every 5 s). Hooks in
// tools/recomp.py HOOKS.
#include <cstdlib>
#include <cstring>
#include <string>

#include "kernel.h"

namespace {
bool skipped(u32 fn) {
    static const std::string list = getenv("F1CE_SKIP_FN") ? "," + std::string(getenv("F1CE_SKIP_FN")) + "," : "";
    char k[16];
    snprintf(k, sizeof(k), ",%x,", fn);
    return list.find(k) != std::string::npos;
}

struct Counter {
    u32 fn;
    bool skip;
    u32 n = 0;
    u64 window = 0;
    explicit Counter(u32 f) : fn(f), skip(skipped(f)) {}
    bool hit() {
        const u64 now = time_usec();
        if (!window) window = now;
        n++;
        if (now - window >= 5000000) {
            LOG_I("skipfn: %06x %.1f calls/s%s", fn, n * 1e6 / (now - window), skip ? " (skipped)" : "");
            window = now;
            n = 0;
        }
        return skip;
    }
};
}  // namespace

// foaLightingManager::RenderStaticShadows
bool skip_2f141c_hook(PPUContext&) { static Counter c(0x2f141c); return c.hit(); }
// foaLightingManager::RenderBlurredShadows
bool skip_2f1bec_hook(PPUContext&) { static Counter c(0x2f1bec); return c.hit(); }
// RenderShadowMap
bool skip_43f620_hook(PPUContext&) { static Counter c(0x43f620); return c.hit(); }
// RenderShadowToAlpha
bool skip_43ffa0_hook(PPUContext&) { static Counter c(0x43ffa0); return c.hit(); }
// foaCarModelManagerPS3::Render
bool skip_30388c_hook(PPUContext&) { static Counter c(0x30388c); return c.hit(); }
