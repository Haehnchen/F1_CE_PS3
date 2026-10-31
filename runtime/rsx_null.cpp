// Minimal software renderer: tracks colour-surface state and executes surface clears in RSX
// local memory, so the presented display buffers reflect at least the clear colour. Geometry is
// ignored until the hardware renderer exists.
#include "host.h"
#include "kernel.h"
#include "rsx.h"

#include <algorithm>
#include <vector>

namespace rsx {
namespace {
struct State {
    u32 surface_format = 0;
    u32 color_ctx = 0;          // DMA context of colour surface A (local / main memory)
    u32 color_offset = 0, color_pitch = 0;
    u32 clip_x = 0, clip_w = 0, clip_y = 0, clip_h = 0;
    u32 clear_color = 0;
} st;
std::atomic<u64> g_methods{0}, g_clears{0};
u64 g_hist[0x10000 / 4];

constexpr u32 CTX_LOCAL = 0xFEED0000;

void clear_surface(u32 mask) {
    static u32 logged = 0;
    if (logged++ < 8)
        LOG_I("rsx clear: mask=%x fmt=%x ctx=%08x off=%08x pitch=%x clip=%u,%u %ux%u color=%08x", mask,
              st.surface_format, st.color_ctx, st.color_offset, st.color_pitch, st.clip_x, st.clip_y, st.clip_w,
              st.clip_h, st.clear_color);
    if (!(mask & 0xF0)) return;                      // colour channels not selected
    if ((st.surface_format & 0x1F) != 0x8) return;   // only A8R8G8B8 for now
    if (st.color_ctx != CTX_LOCAL) return;
    u32 base = mem::RSX_LOCAL + st.color_offset;
    for (u32 y = st.clip_y; y < st.clip_y + st.clip_h; y++) {
        u32 row = base + y * st.color_pitch;
        for (u32 x = st.clip_x; x < st.clip_x + st.clip_w; x++) st32(row + x * 4, st.clear_color);
    }
    g_clears++;
}
}  // namespace

void init() { LOG_I("rsx: software clear-only renderer"); }

void method(u32 m, u32 arg) {
    g_methods++;
    g_hist[(m & 0xFFFC) / 4]++;
    switch (m) {
        case 0x0194: st.color_ctx = arg; break;                                   // SET_CONTEXT_DMA_COLOR_A
        case 0x0208: st.surface_format = arg; break;                              // SET_SURFACE_FORMAT
        case 0x020C: st.color_pitch = arg; break;                                 // SET_SURFACE_PITCH_A
        case 0x0210: st.color_offset = arg; break;                                // SET_SURFACE_COLOR_AOFFSET
        case 0x0200: st.clip_x = arg & 0xFFFF; st.clip_w = arg >> 16; break;      // SET_SURFACE_CLIP_HORIZONTAL
        case 0x0204: st.clip_y = arg & 0xFFFF; st.clip_h = arg >> 16; break;      // SET_SURFACE_CLIP_VERTICAL
        case 0x1D90: st.clear_color = arg; break;                                 // SET_COLOR_CLEAR_VALUE
        case 0x1D94: clear_surface(arg); break;                                   // CLEAR_SURFACE
        default: break;
    }
}

bool report(u32, u32) { return false; }
void flip(u32 ea, u32 pitch, u32 width, u32 height) {
    static u32 n = 0;
    if (n == 1500) {
        std::vector<std::pair<u64, u32>> v;
        for (u32 i = 0; i < 0x4000; i++) if (g_hist[i]) v.push_back({g_hist[i], i * 4});
        std::sort(v.rbegin(), v.rend());
        std::string s;
        for (auto& [cnt, m] : v) { char b[32]; snprintf(b, sizeof(b), " %04x:%llu", m, (unsigned long long)cnt); s += b; }
        LOG_I("rsx method histogram:%s", s.c_str());
    }
    if (++n % 600 == 1) LOG_I("rsx: %llu methods, %llu clears so far", (unsigned long long)g_methods.load(),
                             (unsigned long long)g_clears.load());
    host::present(ea, pitch, width, height);
}
}  // namespace rsx
