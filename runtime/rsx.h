// Renderer hook: receives non-synchronisation RSX methods and flips.
#pragma once
#include "ppu_runtime.h"

namespace rsx {
extern u64 g_last_frame_draws;   // draws of the frame shown last
// output format, hle_aspect.cpp
float res_scale_x();
float res_scale_y();
bool res_scaled();
void video_mode(u8* id, u8* aspect);   // what cellVideoOutGetState reports
bool wide_frame();                     // this frame shows the 3D scene at the format's own aspect ratio
double present_aspect(u32 w, u32 h);   // of the image of a w x h guest surface on screen
void note_display(u32 offset);         // a flipped buffer
bool hud_draw(u32 offset, bool blend, bool depth_test);          // interface draw, kept 16:9
void hud_rect(u32 pw, u32 ph, float* x, float* y, float* w, float* h);   // where it goes in a pw x ph target
bool use_vulkan();    // F1CE_RENDERER=vulkan: the Vulkan renderer instead of OpenGL (rsx.cpp)
extern u32 R[0x10000 / 4];
extern bool g_plain_on;          // the renderer filled g_plain (it may stay off)
extern bool g_plain[0x800];      // methods below 0x2000 that only store their value in R, for the front end's shortcut
// the vertex program upload ports (TRANSFORM_PROGRAM 0x0B80, TRANSFORM_CONSTANT 0x1F00) in runs of words, from a renderer
// that takes them in bulk (else null: one method call per word)
extern void (*g_upload_words)(u32 method, const u32* args, u32 n, bool non_inc);
void init();
void method(u32 method, u32 arg);                               // NV4097 state / draw methods
void flip(u32 ea, u32 pitch, u32 width, u32 height);            // present a display buffer
bool report(u32 type, u32 ea);   // NV4097_GET_REPORT value for `type` at ea (+8); false = not handled here
}  // namespace rsx
