// Host platform layer: window/presentation, input and audio output.
#pragma once
#include "ppu_runtime.h"

namespace host {
struct PadState {
    u16 digital1 = 0, digital2 = 0;          // CELL_PAD_CTRL_* bit layout
    u16 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    u16 pressure[12] = {};
};

void init();
PadState pad_state();
void input_clock_reset();   // scripted input times count from this call (end of the start-up shader phase)
bool quit_requested();
void audio_push(const float* stereo, u32 frames);   // 48 kHz interleaved stereo
// Presents a guest framebuffer (A8R8G8B8 big-endian in guest memory). CPU path (no GPU renderer).
void present(u32 ea, u32 pitch, u32 width, u32 height);

// GPU path: makes an OpenGL 3.3 core context current on the calling thread (waits for the
// window). Returns false if no GL is available (headless).
bool gl_begin();
void* gl_proc(const char* name);
void gl_swap();
// A second context sharing objects with the one made current by gl_begin (call on that thread); a worker thread
// then makes it current with gl_make_current. Returns null if unavailable.
void* gl_create_shared_context(bool share = true);   // share = false: a separate share group
bool gl_make_current(void* ctx);
void gl_drawable_size(int* w, int* h);
// Saves a frame (host ARGB, top-down) for F1CE_DUMP_FRAMES debugging.
void dump_frame(const u32* argb, u32 width, u32 height);
}  // namespace host
