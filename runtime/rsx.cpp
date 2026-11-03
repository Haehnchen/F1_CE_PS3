// Picks the renderer: OpenGL unless F1CE_RENDERER=vulkan (or vk). Decided once at start-up, like the window type.
#include "rsx.h"
#include "kernel.h"

#include <cstdlib>
#include <cstring>

namespace rsx {
bool g_plain_on = false;
bool g_plain[0x800];
void (*g_upload_words)(u32, const u32*, u32, bool) = nullptr;
namespace gl { void init(); void method(u32, u32); void flip(u32, u32, u32, u32); bool report(u32, u32); }
#ifdef F1CE_VULKAN
namespace vk { void init(); void method(u32, u32); void flip(u32, u32, u32, u32); bool report(u32, u32); }

bool use_vulkan() {
    static const bool v = [] {
        const char* e = getenv("F1CE_RENDERER");
        return e && (!strcmp(e, "vulkan") || !strcmp(e, "vk"));
    }();
    return v;
}

void init() { if (use_vulkan()) vk::init(); else gl::init(); }
void method(u32 m, u32 arg) { if (use_vulkan()) vk::method(m, arg); else gl::method(m, arg); }
void flip(u32 ea, u32 pitch, u32 w, u32 h) { if (use_vulkan()) vk::flip(ea, pitch, w, h); else gl::flip(ea, pitch, w, h); }
bool report(u32 type, u32 ea) { return use_vulkan() ? vk::report(type, ea) : gl::report(type, ea); }
#else
bool use_vulkan() {
    static const bool asked = [] {
        const char* e = getenv("F1CE_RENDERER");
        return e && (!strcmp(e, "vulkan") || !strcmp(e, "vk"));
    }();
    if (asked) { static bool once = (LOG_W("F1CE_RENDERER=vulkan: built without F1CE_VULKAN, using OpenGL"), true); (void)once; }
    return false;
}

void init() { use_vulkan(); gl::init(); }
void method(u32 m, u32 arg) { gl::method(m, arg); }
void flip(u32 ea, u32 pitch, u32 w, u32 h) { gl::flip(ea, pitch, w, h); }
bool report(u32 type, u32 ea) { return gl::report(type, ea); }
#endif
}  // namespace rsx
