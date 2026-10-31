// Renderer hook: receives non-synchronisation RSX methods and flips.
#pragma once
#include "ppu_runtime.h"

namespace rsx {
void init();
void method(u32 method, u32 arg);                               // NV4097 state / draw methods
void flip(u32 ea, u32 pitch, u32 width, u32 height);            // present a display buffer
bool report(u32 type, u32 ea);   // NV4097_GET_REPORT value for `type` at ea (+8); false = not handled here
}  // namespace rsx
