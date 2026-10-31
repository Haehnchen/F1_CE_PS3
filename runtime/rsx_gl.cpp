// OpenGL 3.3 renderer for the RSX command stream.
//
// Runs on the RSX front-end thread, which owns the GL context. RSX state is kept in a register file
// indexed by method; trigger methods (clear, draw, program/constant uploads, flip) act on it.
// Vertex and fragment programs (RSX microcode in guest memory) are translated to GLSL.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "host.h"
#include "kernel.h"
#include "rsx.h"

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#include <algorithm>
#include <map>
#include <deque>
#include <condition_variable>
#include <thread>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

extern u64 g_rsx_idle_us;   // hle_gcm.cpp: time the FIFO reader waited for commands (profiling)
namespace rsx {
u32 io_offset_to_ea(u32 io);   // hle_gcm.cpp

namespace {
// ------------------------------------------------------------------ GL loader
#define GL_FUNCS(X)                                                                                      \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)        \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                             \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)    \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                               \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                       \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLCREATESHADERPROC, glCreateShader)              \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader)                  \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)              \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader)                  \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                      \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram)              \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM4FVPROC, glUniform4fv)            \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM4FPROC, glUniform4f)                                \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)  \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLGENBUFFERSPROC, glGenBuffers)                  \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData)                            \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                               \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                       \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                                     \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f) X(PFNGLACTIVETEXTUREPROC, glActiveTexture)              \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D)                                             \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                                   \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) X(PFNGLBLENDCOLORPROC, glBlendColor)      \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)          X(PFNGLPRIMITIVERESTARTINDEXPROC, glPrimitiveRestartIndex) X(PFNGLUNIFORM1FVPROC, glUniform1fv) X(PFNGLUNIFORM2FVPROC, glUniform2fv) X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLBINDVERTEXBUFFERSPROC, glBindVertexBuffers) X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat) \
    X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding) X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary) X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary) \
    X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri) \
    X(PFNGLDETACHSHADERPROC, glDetachShader) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex) X(PFNGLBUFFERSTORAGEPROC, glBufferStorage) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) X(PFNGLFENCESYNCPROC, glFenceSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLTEXBUFFERPROC, glTexBuffer) X(PFNGLUNIFORM4IVPROC, glUniform4iv) X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) X(PFNGLBINDIMAGETEXTUREPROC, glBindImageTexture) X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData)

#define X(type, name) type name;
GL_FUNCS(X)
#undef X

bool load_gl() {
    bool ok = true;
#define X(type, name)                                     \
    name = (type)host::gl_proc(#name);                    \
    if (!name) { LOG_E("GL: missing %s", #name); ok = false; }
    GL_FUNCS(X)
#undef X
    return ok;
}

// ------------------------------------------------------------------ RSX registers
u32 R[0x10000 / 4];
FORCE_INLINE u32 reg(u32 m) { return R[m >> 2]; }
FORCE_INLINE float regf(u32 m) { float f; memcpy(&f, &R[m >> 2], 4); return f; }

u32 vp_words[512 * 4];          // vertex program memory
u32 vp_load_pos = 0;            // in words
float vp_consts[468 * 4];
// per constant: value of vp_ver when it was last written; a program re-uploads only the constants newer than its upload
u32 vp_slot_ver[468];
u32 vp_ver = 1;
u32 vp_const_pos = 0;           // in floats
bool gl_ok = false;

constexpr u32 CTX_LOCAL = 0xFEED0000, CTX_MAIN = 0xFEED0001;

u32 location_to_ea(u32 offset, bool main_memory);
// fingerprint of [ea, ea + bytes): 512 words sampled evenly
u64 mem_fingerprint(u32 ea, u32 bytes) {
    u64 h = 1469598103934665603ull;
    u32 step = std::max(4u, (bytes / 512) & ~3u);
    for (u32 o = 0; o + 4 <= bytes; o += step) { u32 v; memcpy(&v, gmem(ea + o), 4); h = (h ^ v) * 1099511628211ull; }
    return h;
}
u32 location_to_ea(u32 offset, bool main_memory) {
    return main_memory ? io_offset_to_ea(offset) : mem::RSX_LOCAL + offset;
}

// ------------------------------------------------------------------ surfaces
// Colour surfaces and depth (zeta) buffers are separate objects keyed by their local-memory
// offsets, as on RSX: a depth buffer can later be sampled as a texture (shadow maps).
struct Surface {
    u32 offset = 0, width = 0, height = 0, format = 0;
    GLuint tex = 0, fbo = 0;
    GLuint attached_depth = 0;
    u64 last_use = 0;
    // the GPU's rendering never reaches guest memory here, so guest memory changing under a local-memory surface
    // means the CPU loaded something else there (an image where a video used to be rendered): sampled fingerprint
    // of the memory when the surface was last rendered, and whether the memory has changed since
    u64 mem_sig = 0, sig_checked = ~0ull;
    u32 sig_bytes = 0;
    bool stale = false;
    u32 scale = 1;           // physical size = width x scale, height x scale (resolution scaling)
    bool local_wb = false;   // local-memory target written back like a main-memory one (F1CE_WB_LOCAL)
    u64 write_seq = 0;       // g_write_seq of the last draw/clear that wrote colour into it
    // antialiased target (surface format bits 12-15): drawn into 4x multisampled renderbuffers (colour + its own
    // depth/stencil), resolved into tex / the zeta texture before they are read
    GLuint ms_fbo = 0, ms_color = 0, ms_depth = 0;
    u32 ms_w = 0, ms_h = 0;
    bool ms_active = false, ms_dirty = false;
    u32 main_ea = 0, pitch = 0;   // colour buffer in main memory: written back to guest memory (the CPU reads it)
    bool wb_dirty = false, wb_pending = false;   // drawn since the last readback / readback in flight in wb_pbo
    GLuint wb_pbo = 0;
};
constexpr u32 MAIN_SURFACE_KEY = 0x80000000;   // g_surfaces key bit for main-memory colour buffers
// Resolution scaling (roadmap step 3, F1CE_SCALE=1..4, default 1): local render targets and depth buffers are
// allocated at scale x their guest size and drawn with a scaled viewport and scissor; the vertex programs map window
// coordinates through the guest size, so geometry needs no change. Main-memory targets (read back by the CPU) stay
// native. Fragment programs see WPOS in guest pixels (u_res_scale), occlusion counts are divided by scale^2 (the
// game picks levels of detail and flare intensity from them), cube maps and copies use the physical sizes.
// The fragment programs read their scale from uniforms (u_res_scale, u_ui_clamp), so one source serves every scale
// and the shader cache is shared: changing F1CE_SCALE no longer compiles every program again (the Driver garage stayed
// white for seconds the first time at a new scale). F1CE_NATIVE_GLSL=1: the scale-1 sources of before.
bool scaled_glsl() { static const bool v = !getenv("F1CE_NATIVE_GLSL"); return v; }
u32 res_scale() {
    static const u32 v = [] {
        const char* e = getenv("F1CE_SCALE");
        u32 x = e ? (u32)atoi(e) : 1;
        return std::clamp(x, 1u, 4u);
    }();
    return v;
}
struct DepthSurface {
    u32 offset = 0, width = 0, height = 0;
    GLuint tex = 0;
    u64 write_seq = 0;   // g_write_seq of the last draw/clear that wrote it
};
// draws and clears are numbered; a colour and a depth surface at the same offset are told apart by which one was
// written last (a depth-only pass also creates an unwritten colour surface at its offset)
u64 g_write_seq = 0;
u64 g_square_write_seq = 0;
// sRGB output (SHADER_PACKER 0x1FD8 bit 0): colour surfaces are sRGB textures, so with GL_FRAMEBUFFER_SRGB the GPU
// encodes and blends in linear space as the RSX does (encoding in the shader broke additive passes: the menu video
// composites its planes with ONE/ONE blending and came out washed out and magenta). Clears, blits and readbacks
// move raw values: conversion off for them.
void srgb_write(bool on) {
    static int cur = -1;
    if (cur == (int)on) return;
    cur = on;
    if (on) glEnable(0x8DB9 /*GL_FRAMEBUFFER_SRGB*/); else glDisable(0x8DB9);
}   // g_write_seq of the last colour write to a square target (cube faces are square)
DepthSurface* g_cur_depth = nullptr;
std::unordered_map<u32, Surface> g_surfaces;      // keyed by local-memory offset of colour buffer A
std::unordered_map<u32, DepthSurface> g_depths;   // keyed by local-memory offset of the zeta buffer
u64 g_frame = 0;
// profiling: microseconds per frame section (reported every 120 frames by flip())
struct Prof { u64 program = 0, textures = 0, vertices = 0, submit = 0, other = 0, draws = 0, verts = 0, frame_start = 0, conv_verts = 0, conv_inst = 0, hashed = 0, tex_hashed = 0, t_index = 0, t_hash = 0, t_conv = 0, conv_new = 0, conv_changed = 0, idx_built = 0, ir_grow = 0, ir_changed = 0, idx_old = 0, t_pre = 0, t_apply = 0, t_upload = 0, t_bind = 0, binds = 0, t_draw = 0, t_ringwait = 0, ring_segs = 0, t_texdec = 0, tex_decoded = 0, memo_hits = 0, memo_miss = 0, t_miss = 0; } g_prof;
// debug: F1CE_WATCH_FP=<local offset> logs 2D-engine writes into [offset, offset + 0x800) and the constants of draws
// that use the fragment program at that offset
const u32 g_watch_fp = getenv("F1CE_WATCH_FP") ? (u32)strtoul(getenv("F1CE_WATCH_FP"), nullptr, 16) : ~0u;

// texture binding state, tracked so draws can skip redundant binds and wrap-mode changes
u32 g_active_unit = 0;
GLuint g_bound_tex[20] = {};
std::unordered_map<GLuint, u32> g_wrap_of;   // texture -> last applied address register bits (wrap s/t)
std::unordered_map<GLuint, u64> g_filter_of;   // texture -> last applied filter register / control0 (draw())
void active_unit(u32 u) {
    if (g_active_unit != u) { glActiveTexture(GL_TEXTURE0 + u); g_active_unit = u; }
}
void bind_tex2d(GLuint t) {
    if (g_bound_tex[g_active_unit] != t) { glBindTexture(GL_TEXTURE_2D, t); g_bound_tex[g_active_unit] = t; }
}
void delete_tex(GLuint* t) {
    for (GLuint& b : g_bound_tex) if (b == *t) b = 0;
    g_wrap_of.erase(*t);
    g_filter_of.erase(*t);
    glDeleteTextures(1, t);
}

// F1CE_TRACE_FRAME=<n> traces frame n; F1CE_TRACE_COUNT=<k> extends it to frames n .. n+k-1
bool trace_on(long long first) {
    static const long long count = getenv("F1CE_TRACE_COUNT") ? atoll(getenv("F1CE_TRACE_COUNT")) : 1;
    return first >= 0 && (long long)g_frame >= first && (long long)g_frame < first + count;
}
std::map<u32, u32> g_frame_targets;   // debug (F1CE_FLIP_LOG): draws + clears per target in the current frame

// GL state last set by bind_target / apply_state / draw, so unchanged state costs no driver calls; code that changes
// it directly clears the matching flag
bool g_tgt_valid = false, g_state_valid = false;
GLuint g_cur_prog = 0;

// colour buffers whose offset is currently used at another size (the game renders a 256x128 and a 1280x720 target
// at the same offset every frame): swapped back in instead of reallocated
std::unordered_map<u64, Surface> g_surface_spares;
// bumped when a surface or depth buffer appears, is reallocated or changes between stale and current: the per-unit
// texture memo of draw() is valid only within one epoch (and one frame)
u32 g_surf_epoch = 0;
u32 g_texparam_ver = 0;   // bumped when wrap / filter parameters of any texture are set

Surface* get_surface(u32 offset, u32 w, u32 h, u32 fmt) {
    Surface& s = g_surfaces[offset];
    if (s.tex && (s.width != w || s.height != h)) {
        auto size_key = [&](u32 sw, u32 sh) { return ((u64)offset << 32) | ((u64)sw << 16) | sh; };
        Surface old = s;
        auto it = g_surface_spares.find(size_key(w, h));
        if (it != g_surface_spares.end()) { s = it->second; g_surface_spares.erase(it); } else s = Surface{};
        g_surface_spares[size_key(old.width, old.height)] = old;
        g_surf_epoch++;
    }
    if (!s.tex) {
        g_surf_epoch++;
        s.offset = offset; s.width = w; s.height = h; s.format = fmt;
        s.scale = (offset & MAIN_SURFACE_KEY) ? 1 : res_scale();
        glGenTextures(1, &s.tex);
        bind_tex2d(s.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x8C43 /*GL_SRGB8_ALPHA8*/, (GLsizei)(w * s.scale), (GLsizei)(h * s.scale), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // sampled raw: the TEXTURE_ADDRESS gamma bits decide the decode (in the shader)
        glTexParameteri(GL_TEXTURE_2D, 0x8A48 /*GL_TEXTURE_SRGB_DECODE_EXT*/, 0x8A4A /*GL_SKIP_DECODE_EXT*/);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (!s.fbo) glGenFramebuffers(1, &s.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        g_tgt_valid = false;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
        LOG_I("rsx: surface %08x %ux%u fmt %x", offset, w, h, fmt);
    }
    s.last_use = g_frame;
    return &s;
}

std::vector<u32> g_ms_surfaces;   // offsets of surfaces with multisampled storage
// samples of the targets the game renders antialiased (surface format bits 12-15; the RSX uses 4):
// F1CE_MSAA=2/4/8 (roadmap step 3, more samples smooth the edges further)
GLsizei msaa_samples() {
    static const GLsizei v = [] {
        const char* e = getenv("F1CE_MSAA");
        int x = e ? atoi(e) : 4;
        GLint mx = 4;
        glGetIntegerv(0x8D57 /*GL_MAX_SAMPLES*/, &mx);
        return (GLsizei)std::clamp(x == 2 || x == 4 || x == 8 || x == 16 ? x : 4, 1, (int)mx);
    }();
    return v;
}

GLuint ensure_ms(Surface* s) {
    if (!s->ms_fbo || s->ms_w != s->width || s->ms_h != s->height) {
        if (!s->ms_fbo) {
            glGenFramebuffers(1, &s->ms_fbo);
            glGenRenderbuffers(1, &s->ms_color);
            glGenRenderbuffers(1, &s->ms_depth);
            g_ms_surfaces.push_back(s->offset);
            LOG_I("rsx: surface %08x %ux%u is antialiased: %ux multisampled", s->offset, s->width, s->height, msaa_samples());
        }
        glBindRenderbuffer(GL_RENDERBUFFER, s->ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples(), 0x8C43 /*GL_SRGB8_ALPHA8*/, (GLsizei)(s->width * s->scale), (GLsizei)(s->height * s->scale));
        glBindRenderbuffer(GL_RENDERBUFFER, s->ms_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples(), GL_DEPTH24_STENCIL8, (GLsizei)(s->width * s->scale), (GLsizei)(s->height * s->scale));
        glBindFramebuffer(GL_FRAMEBUFFER, s->ms_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->ms_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->ms_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOG_W("GL: incomplete MSAA FBO %08x", s->offset);
        s->ms_w = s->width; s->ms_h = s->height;
        g_tgt_valid = false;
    }
    return s->ms_fbo;
}

void srgb_write(bool on);
// resolves a multisampled surface into its texture (and its zeta texture); true if the framebuffer binding changed
bool resolve_ms(Surface& s) {
    if (!s.ms_fbo || !s.ms_dirty) return false;
    srgb_write(false);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
    GLint pw = (GLint)(s.width * s.scale), ph = (GLint)(s.height * s.scale);
    glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph,
                      GL_COLOR_BUFFER_BIT | (s.attached_depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0), GL_NEAREST);
    s.ms_dirty = false;
    g_tgt_valid = false;
    return true;
}
// before a surface texture (colour, or a zeta texture attached to an antialiased target) is read
bool resolve_for(GLuint tex) {
    bool changed = false;
    for (u32 off : g_ms_surfaces) {
        auto it = g_surfaces.find(off);
        if (it != g_surfaces.end() && (it->second.tex == tex || it->second.attached_depth == tex)) changed |= resolve_ms(it->second);
    }
    return changed;
}
bool resolve_all_ms() {
    bool changed = false;
    for (u32 off : g_ms_surfaces) {
        auto it = g_surfaces.find(off);
        if (it != g_surfaces.end()) changed |= resolve_ms(it->second);
    }
    return changed;
}

DepthSurface* get_depth(u32 offset, u32 w, u32 h) {
    DepthSurface& d = g_depths[offset];
    if (d.tex && (d.width < w || d.height < h)) {
        delete_tex(&d.tex);
        d.tex = 0;
    }
    if (!d.tex) {
        g_surf_epoch++;
        d.offset = offset; d.width = w; d.height = h;
        glGenTextures(1, &d.tex);
        bind_tex2d(d.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)(w * res_scale()), (GLsizei)(h * res_scale()), 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        LOG_I("rsx: depth surface %08x %ux%u", offset, w, h);
    }
    return &d;
}

// debug: writes surface `off` to frame_s<off><tag>_<frame>_WxH.argb (top row first, BGRA bytes)
void dump_surface(u32 off, const char* tag) {
    auto it = g_surfaces.find(off);
    if (it == g_surfaces.end() || !it->second.tex) return;
    Surface& sf = it->second;
    resolve_ms(sf);
    const u32 pw = sf.width * sf.scale, ph = sf.height * sf.scale;   // (physical size)
    std::vector<u32> px((size_t)pw * ph), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sf.fbo);
    srgb_write(false);
    glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    for (u32 y = 0; y < ph; y++)
        memcpy(&fl[(size_t)y * pw], &px[(size_t)(ph - 1 - y) * pw], pw * 4);
    char name[96];
    snprintf(name, sizeof(name), "frame_s%08x%s_%05llu_%ux%u.argb", off, tag, (unsigned long long)g_frame, pw, ph);
    if (FILE* f = fopen(name, "wb")) { fwrite(fl.data(), 4, fl.size(), f); fclose(f); }
}

// copies a main-memory colour buffer (A8R8G8B8) back to guest memory, top row first
// Main-memory colour buffers are read back asynchronously: once per frame the pixels go into a pixel buffer
// object, and the next frame copies them to guest memory (one frame of latency, no pipeline stall).
void write_back_pending(Surface* s) {
    if (!s->wb_pending) return;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->wb_pbo);
    if (const u32* px = (const u32*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)s->width * s->height * 4, GL_MAP_READ_BIT)) {
        u32 pitch = s->pitch ? s->pitch : s->width * 4;
        bool b8 = (s->format & 0x1F) == 9;   // B8 target: one byte (blue) per pixel
        for (u32 y = 0; y < s->height; y++)
            for (u32 x = 0; x < s->width; x++) {
                u32 v = px[(size_t)(s->height - 1 - y) * s->width + x];
                if (b8) st8(s->main_ea + y * pitch + x, (u8)v);
                else st32(s->main_ea + y * pitch + x * 4, v);
            }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        if (s->local_wb && s->sig_bytes) s->mem_sig = mem_fingerprint(s->main_ea, s->sig_bytes);   // not a CPU write
        if (s->height == 1 && g_frame % 300 == 0) {   // the per-car lighting table (diagnostics)
            std::string t;
            for (u32 x = 0; x < std::min(s->width, 24u); x++) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(s->main_ea + x * 4)); t += b; }
            LOG_I("rsx: lighting table %08x:%s", s->main_ea, t.c_str());
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s->wb_pending = false;
}
void write_back_start(Surface* s) {
    if (!s->wb_dirty) return;
    if (!s->wb_pbo) glGenBuffers(1, &s->wb_pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->wb_pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)s->width * s->height * 4, nullptr, GL_STREAM_READ);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    srgb_write(false);
    glReadPixels(0, 0, (GLsizei)s->width, (GLsizei)s->height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s->wb_dirty = false;
    s->wb_pending = true;
}

// binds the current colour surface A (+ zeta buffer) as render target; returns it (or null if unsupported)
Surface* bind_target() {
    u32 fmt = reg(0x0208);
    u32 w = reg(0x0200) >> 16, h = reg(0x0204) >> 16;
    if (!w || !h) return nullptr;
    // colour buffer in main memory (e.g. small lighting lookup tables that the CPU reads back each frame)
    bool main = reg(0x0194) == CTX_MAIN;
    if (!main && reg(0x0194) != CTX_LOCAL) return nullptr;
    Surface* s = get_surface(main ? (reg(0x0210) | MAIN_SURFACE_KEY) : reg(0x0210), w, h, fmt);
    g_frame_targets[s->offset]++;
    if (!main && (s->stale || s->sig_checked != g_frame)) {   // rendered: the surface is the current content again
        s->sig_bytes = std::max(1u, reg(0x020C) & 0xFFFF) * h;
        s->mem_sig = mem_fingerprint(location_to_ea(reg(0x0210), false), s->sig_bytes);
        s->sig_checked = g_frame;
        if (s->stale) g_surf_epoch++;
        s->stale = false;
    }
    if (main) {
        if (!s->main_ea) LOG_I("rsx: main-memory render target io %08x %ux%u fmt %x (written back after draws)", reg(0x0210), w, h, fmt);
        s->main_ea = io_offset_to_ea(reg(0x0210));
        s->pitch = reg(0x020C) & 0xFFFF;
    } else {
        // experiment (F1CE_WB_LOCAL): small local-memory colour targets are also written back to guest memory, for
        // code that reads rendered results straight from video memory
        static const bool wb_local = getenv("F1CE_WB_LOCAL") != nullptr;
        if (wb_local && w * h <= 640 * 360) {
            if (!s->main_ea) LOG_I("rsx: local render target %08x %ux%u fmt %x written back", reg(0x0210), w, h, fmt);
            s->main_ea = location_to_ea(reg(0x0210), false);
            s->pitch = reg(0x020C) & 0xFFFF;
            s->local_wb = true;
        }
    }
    static GLuint bound_fbo;
    static u32 bound_w, bound_h, bound_sc0, bound_sc1;
    GLuint depth = 0;
    g_cur_depth = nullptr;
    if (!main && ((fmt >> 5) & 7) && reg(0x0198) == CTX_LOCAL) {   // SET_SURFACE_ZETA_OFFSET
        g_cur_depth = get_depth(reg(0x0214), w, h);
        depth = g_cur_depth->tex;
    }
    if (depth != s->attached_depth) {   // (also the resolve target of an antialiased surface)
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        g_tgt_valid = false;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        s->attached_depth = depth;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOG_W("GL: incomplete FBO %08x", s->offset);
    }
    static const bool no_msaa = getenv("F1CE_NO_MSAA") != nullptr;   // debug
    s->ms_active = !main && ((fmt >> 12) & 0xF) && !no_msaa;
    GLuint fb = s->ms_active ? ensure_ms(s) : s->fbo;
    if (!g_tgt_valid || bound_fbo != fb) glBindFramebuffer(GL_FRAMEBUFFER, fb);
    if (g_tgt_valid && bound_fbo == fb && bound_w == w && bound_h == h && bound_sc0 == reg(0x08C0) && bound_sc1 == reg(0x08C4))
        return s;
    const GLint k = (GLint)s->scale;
    glViewport(0, 0, (GLsizei)w * k, (GLsizei)h * k);
    u32 sx = reg(0x08C0) & 0xFFFF, sw = reg(0x08C0) >> 16, sy = reg(0x08C4) & 0xFFFF, sh = reg(0x08C4) >> 16;
    if (sw && sh && (sw < 4096 && sh < 4096)) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)sx * k, ((GLint)h - (GLint)(sy + sh)) * k, (GLsizei)sw * k, (GLsizei)sh * k);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    bound_fbo = fb; bound_w = w; bound_h = h; bound_sc0 = reg(0x08C0); bound_sc1 = reg(0x08C4);
    g_tgt_valid = true;
    return s;
}

// ------------------------------------------------------------------ shaders
GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        LOG_E("GL shader compile error:\n%s\n--- source ---\n%s", log, src.c_str());
    }
    return sh;
}

const char* SWZ = "xyzw";

std::string swizzle_str(u32 sx, u32 sy, u32 sz, u32 sw) {
    std::string s = ".";
    s += SWZ[sx]; s += SWZ[sy]; s += SWZ[sz]; s += SWZ[sw];
    return s;
}

std::string mask_str(bool x, bool y, bool z, bool w) {
    std::string s;
    if (x) s += 'x';
    if (y) s += 'y';
    if (z) s += 'z';
    if (w) s += 'w';
    return s;
}

// assigns a vec4 expression to dst through a write mask
std::string masked_assign(const std::string& dst, const std::string& mask, const std::string& expr, bool sat) {
    std::string e = sat ? "clamp(" + expr + ", 0.0, 1.0)" : expr;
    if (mask.size() == 4) return dst + " = " + e + ";";
    return dst + "." + mask + " = (" + e + ")." + mask + ";";
}

// ---------------- vertex program
struct VPInfo { std::string glsl; u32 outputs_used = 0; };

std::string vp_src(u32 src, u32 d1, bool abs_flag, u32 d3, u32 d0) {
    u32 type = src & 3, tmp = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3, sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    bool neg = (src >> 16) & 1;
    std::string r;
    switch (type) {
        case 1: r = "R[" + std::to_string(tmp) + "]"; break;
        case 2: r = "in_attr" + std::to_string((d1 >> 8) & 0xF); break;
        case 3: {
            u32 ci = (d1 >> 12) & 0x3FF;
            if (d3 & 2) r = "C[clamp(" + std::to_string(ci) + " + A[" + std::to_string((d0 >> 24) & 1) + "]." + SWZ[d0 & 3] + ", 0, 467)]";
            else r = "C[" + std::to_string(ci) + "]";
            break;
        }
        default: r = "vec4(0.0)";
    }
    r = "(" + r + ")" + swizzle_str(sx, sy, sz, sw);
    if (abs_flag) r = "abs(" + r + ")";
    if (neg) r = "-" + r;
    return r;
}

// highest constant index used with array `name` ("R" matches R[n] but not FC[n]) + 1; at least `min_n`
int array_size(const std::string& src, const char* name, int min_n) {
    int n = min_n;
    size_t len = strlen(name);
    for (size_t i = src.find(name); i != std::string::npos; i = src.find(name, i + 1)) {
        if (i > 0 && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        if (src.compare(i + len, 1, "[") != 0) continue;
        n = std::max(n, atoi(src.c_str() + i + len + 1) + 1);
    }
    return n;
}
// Vertex pulling (roadmap step 2): the vertex program reads the raw big-endian guest records itself from two buffer
// textures (the stream ring and the vertex heap) and decodes the RSX formats, as fetch_attr() does on the CPU.
// u_va[i]: x = byte offset of record 0 in its buffer, y = stride, z = type | size << 4 | modulo << 8 | ring << 9
// (type 0: attribute disabled, reads (0, 0, 0, 1)), w = frequency. gl_VertexID is the guest vertex index.
bool vp_uses_input(const std::string& body, int i) {   // "in_attr<i>" not followed by another digit
    std::string n = "in_attr" + std::to_string(i);
    for (size_t q = body.find(n); q != std::string::npos; q = body.find(n, q + 1))
        if (q + n.size() >= body.size() || !isdigit((unsigned char)body[q + n.size()])) return true;
    return false;
}

std::string vp_fetch_glsl(const std::string& body) {
    std::string s =
        "uniform usamplerBuffer u_va_ring;\nuniform usamplerBuffer u_va_heap;\nuniform ivec4 u_va[16];\n"
        "uint va_w(int a, bool r) { return r ? texelFetch(u_va_ring, a >> 2).r : texelFetch(u_va_heap, a >> 2).r; }\n"
        "uint va_u8(int a, bool r) { return (va_w(a, r) >> uint((a & 3) * 8)) & 255u; }\n"
        "uint va_u16(int a, bool r) {\n"
        " if ((a & 1) == 0) { uint x = (va_w(a, r) >> uint((a & 2) * 8)) & 0xFFFFu; return ((x & 255u) << 8) | (x >> 8); }\n"
        " return (va_u8(a, r) << 8) | va_u8(a + 1, r);\n"
        "}\n"
        "uint va_u32(int a, bool r) {\n"
        " if ((a & 3) == 0) { uint w = va_w(a, r); return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24); }\n"
        " return (va_u16(a, r) << 16) | va_u16(a + 2, r);\n"
        "}\n"
        "float va_s16(uint x) { return float(int(x << 16) >> 16); }\n"
        "vec4 va_fetch(int i) {\n"
        " ivec4 d = u_va[i];\n"
        " int type = d.z & 15, size = min((d.z >> 4) & 15, 4);\n"
        " vec4 o = vec4(0.0, 0.0, 0.0, 1.0);\n"
        " if (type == 0) return o;\n"
        " bool r = (d.z & 512) != 0;\n"
        " int v = gl_VertexID;\n"
        " if (d.w > 1) v = (d.z & 256) != 0 ? v % d.w : v / d.w;\n"
        " int a = d.x + v * d.y;\n"
        " if (type == 6) {   // CMP 11:11:10\n"
        "  uint p = va_u32(a, r);\n"
        "  if (size > 0) o.xyz = vec3(float(int(p << 21) >> 21) / 1023.0, float(int(p << 10) >> 21) / 1023.0, float(int(p) >> 22) / 511.0);\n"
        "  return o;\n"
        " }\n"
        " for (int k = 0; k < 4; k++) {\n"
        "  if (k >= size) break;\n"
        "  if (type == 1) o[k] = va_s16(va_u16(a + k * 2, r)) / 32767.0;\n"
        "  else if (type == 2) o[k] = uintBitsToFloat(va_u32(a + k * 4, r));\n"
        "  else if (type == 3) o[k] = unpackHalf2x16(va_u16(a + k * 2, r)).x;\n"
        "  else if (type == 4) o[k] = float(va_u8(a + k, r)) / 255.0;\n"
        "  else if (type == 5) o[k] = va_s16(va_u16(a + k * 2, r));\n"
        "  else if (type == 7) o[k] = float(va_u8(a + k, r));\n"
        " }\n"
        " return o;\n"
        "}\n";
    for (int i = 0; i < 16; i++)
        if (vp_uses_input(body, i)) s += "vec4 in_attr" + std::to_string(i) + ";\n";
    return s;
}

VPInfo translate_vp(u32 start) {
    VPInfo info;
    std::ostringstream body;
    for (u32 slot = start; slot < 512; slot++) {
        const u32* w = &vp_words[slot * 4];
        u32 d0 = w[0], d1 = w[1], d2 = w[2], d3 = w[3];
        u32 vec_op = (d1 >> 22) & 0x1F, sca_op = (d1 >> 27) & 0x1F;
        u32 src0 = ((d1 & 0xFF) << 9) | (d2 >> 23);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d2 & 0x3F) << 11) | (d3 >> 21);
        bool sat = (d0 >> 26) & 1, vec_result = (d0 >> 30) & 1;
        u32 vec_tmp = (d0 >> 15) & 0x3F, dst = (d3 >> 2) & 0x1F, sca_tmp = (d3 >> 7) & 0x3F;
        std::string vmask = mask_str((d3 >> 16) & 1, (d3 >> 15) & 1, (d3 >> 14) & 1, (d3 >> 13) & 1);
        std::string smask = mask_str((d3 >> 20) & 1, (d3 >> 19) & 1, (d3 >> 18) & 1, (d3 >> 17) & 1);
        std::string a = vp_src(src0, d1, (d0 >> 21) & 1, d3, d0);
        std::string b = vp_src(src1, d1, (d0 >> 22) & 1, d3, d0);
        std::string c = vp_src(src2, d1, (d0 >> 23) & 1, d3, d0);

        std::string ve;
        switch (vec_op) {
            case 0: break;
            case 1: ve = a; break;
            case 2: ve = a + " * " + b; break;
            case 3: ve = a + " + " + c; break;   // ADD uses src0 + src2
            case 4: ve = a + " * " + b + " + " + c; break;
            case 5: ve = "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"; break;
            case 6: ve = "vec4(dot(vec4((" + a + ").xyz, 1.0), " + b + "))"; break;
            case 7: ve = "vec4(dot(" + a + ", " + b + "))"; break;
            case 8: ve = "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"; break;
            case 9: ve = "min(" + a + ", " + b + ")"; break;
            case 10: ve = "max(" + a + ", " + b + ")"; break;
            case 11: ve = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 12: ve = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 13: body << "A[" << ((d0 >> 24) & 1) << "] = ivec4(floor(" << a << "));\n"; break;
            case 14: ve = "fract(" + a + ")"; break;
            case 15: ve = "floor(" + a + ")"; break;
            case 16: ve = "vec4(equal(" + a + ", " + b + "))"; break;
            case 17: ve = "vec4(0.0)"; break;
            case 18: ve = "vec4(greaterThan(" + a + ", " + b + "))"; break;
            case 19: ve = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
            case 20: ve = "vec4(notEqual(" + a + ", " + b + "))"; break;
            case 21: ve = "vec4(1.0)"; break;
            case 22: ve = "sign(" + a + ")"; break;
            case 25: ve = "textureLod(vtex" + std::to_string((d2 >> 8) & 3) + ", (" + a + ").xy, 0.0)"; break;   // TXL (vertex texture)
            default: LOG_W("rsx vp: unsupported vector op %u", vec_op);
        }
        std::string se;
        std::string cx = "(" + c + ").x";
        switch (sca_op) {
            case 0: break;
            case 1: se = "vec4(" + cx + ")"; break;
            case 2: se = "vec4(1.0 / " + cx + ")"; break;
            case 3: se = "vec4(clamp(1.0 / " + cx + ", -1.884467e19, 1.884467e19))"; break;
            case 4: se = "vec4(inversesqrt(abs(" + cx + ")))"; break;
            case 5: se = "vec4(exp2(floor(" + cx + ")), fract(" + cx + "), exp2(" + cx + "), 1.0)"; break;
            case 6: se = "vec4(floor(log2(abs(" + cx + "))), 1.0, log2(abs(" + cx + ")), 1.0)"; break;
            case 7: se = "vec4(1.0, max((" + c + ").x, 0.0), ((" + c + ").x > 0.0) ? pow(max((" + c + ").y, 0.0), clamp((" + c + ").w, -127.9961, 127.9961)) : 0.0, 1.0)"; break;
            case 13: se = "vec4(log2(" + cx + "))"; break;
            case 14: se = "vec4(exp2(" + cx + "))"; break;
            case 15: se = "vec4(sin(" + cx + "))"; break;
            case 16: se = "vec4(cos(" + cx + "))"; break;
            default: LOG_W("rsx vp: unsupported scalar op %u", sca_op);
        }
        // condition codes: D0 bits 10-12 condition (1 lt, 2 eq, 4 gt), 13 test enable, 14 + 29 update enable,
        // 25 condition register select, 2-9 condition swizzle (w, z, y, x)
        u32 cond = (d0 >> 10) & 7;
        bool cond_test = ((d0 >> 13) & 1) && cond != 7;
        bool cond_update = ((d0 >> 14) & 1) && ((d0 >> 29) & 1);
        std::string ccreg = "CC[" + std::to_string((d0 >> 25) & 1) + "]";
        std::string ccv = ccreg + swizzle_str((d0 >> 8) & 3, (d0 >> 6) & 3, (d0 >> 4) & 3, (d0 >> 2) & 3);
        std::string cmask = "step(vec4(0.5), vec4(0.0)";
        if (cond & 1) cmask += " + vec4(lessThan(" + ccv + ", vec4(0.0)))";
        if (cond & 2) cmask += " + vec4(equal(" + ccv + ", vec4(0.0)))";
        if (cond & 4) cmask += " + vec4(greaterThan(" + ccv + ", vec4(0.0)))";
        cmask += ")";
        auto put = [&](const std::string& target, const std::string& mask, const std::string& val) {
            if (cond_test) return masked_assign(target, mask, "mix(" + target + ", " + val + ", m)", false) + "\n";
            return masked_assign(target, mask, val, false) + "\n";
        };
        bool skip = ((d0 >> 13) & 1) && cond == 0;   // condition "never"
        if (!ve.empty() && !skip) {
            body << "{ vec4 v = " << (sat ? "clamp(" + ve + ", 0.0, 1.0)" : ve) << ";\n";
            if (cond_test) body << " vec4 m = " << cmask << ";\n";
            if (vec_tmp != 0x3F && !vmask.empty()) body << put("R[" + std::to_string(vec_tmp) + "]", vmask, "v");
            if ((vec_result || sca_op == 0) && dst != 0x1F && !vmask.empty()) {   // no scalar op: the vector unit owns the output
                body << put("O[" + std::to_string(dst) + "]", vmask, "v");
                info.outputs_used |= 1u << dst;
            }
            if (cond_update && !vmask.empty()) body << put(ccreg, vmask, "v");
            body << "}\n";
        }
        if (!se.empty() && !skip) {
            body << "{ vec4 s = " << (sat ? "clamp(" + se + ", 0.0, 1.0)" : se) << ";\n";
            if (cond_test) body << " vec4 m = " << cmask << ";\n";
            if (sca_tmp != 0x3F && !smask.empty()) body << put("R[" + std::to_string(sca_tmp) + "]", smask, "s");
            if (!vec_result && sca_op != 0 && dst != 0x1F && !smask.empty()) {
                body << put("O[" + std::to_string(dst) + "]", smask, "s");
                info.outputs_used |= 1u << dst;
            }
            if (cond_update && ve.empty() && !smask.empty()) body << put(ccreg, smask, "s");
            body << "}\n";
        }
        if (d3 & 1) break;   // last instruction
    }
    std::ostringstream src;
    src << "#version 420 core\n";
    extern bool g_vtx_gpu;
    if (g_vtx_gpu) src << vp_fetch_glsl(body.str());
    else for (int i = 0; i < 16; i++) src << "layout(location = " << i << ") in vec4 in_attr" << i << ";\n";
    src << "uniform vec4 C[468];\nuniform sampler2D vtex0;\nuniform sampler2D vtex1;\nuniform sampler2D vtex2;\nuniform sampler2D vtex3;\n"
           "uniform vec4 u_vp_scale;\nuniform vec4 u_vp_offset;\nuniform vec4 u_surface;\n"
           "out vec4 v_col0; out vec4 v_col1; out vec4 v_fog; out vec4 v_tex[10];\n"
           "void main() {\n";
    {   // only the temporaries the program uses (zero-initialised arrays of 48/64 registers compiled very slowly)
        int nr = array_size(body.str(), "R", 1);
        src << " vec4 R[" << nr << "]; for (int i = 0; i < " << nr << "; i++) R[i] = vec4(0.0);\n";
    }
    if (g_vtx_gpu)
        for (int i = 0; i < 16; i++)
            if (vp_uses_input(body.str(), i))
                src << " in_attr" << i << " = va_fetch(" << i << ");\n";
    src << " vec4 O[32]; for (int i = 0; i < 32; i++) O[i] = vec4(0.0, 0.0, 0.0, 1.0);\n ivec4 A[2]; A[0] = ivec4(0); A[1] = ivec4(0);\n"
           " vec4 CC[2]; CC[0] = vec4(0.0); CC[1] = vec4(0.0);\n";
    src << body.str();
    // RSX: clip-space position -> (divide) -> viewport scale/offset -> window coords (top-left origin).
    // Convert back to GL clip space for the current surface size, keeping w for perspective interpolation.
    src << " vec4 p = O[0];\n"
           " vec3 win = (p.xyz / p.w) * u_vp_scale.xyz + u_vp_offset.xyz;\n"
           " float y_top = u_surface.z > 0.5 ? -win.y : win.y;   // SHADER_WINDOW origin bottom: y measured upward\n"
           " vec3 ndc = vec3(win.x / u_surface.x * 2.0 - 1.0, 1.0 - y_top / u_surface.y * 2.0, win.z * 2.0 - 1.0);\n"
           " gl_Position = vec4(ndc * p.w, p.w);\n"
           " v_col0 = O[1]; v_col1 = O[2]; v_fog = O[5];\n"
           " for (int i = 0; i < 10; i++) v_tex[i] = O[7 + i];\n}\n";
    info.glsl = src.str();
    return info;
}

// ---------------- fragment program
struct FPInfo {
    std::string glsl;
    std::vector<u32> const_offsets;   // byte offsets (from program start) of inline constants
    u32 size = 0;                     // bytes
    u32 tex_used = 0;
};

// fragment programs are read from guest memory, or from a saved copy while the program database is replayed
const u8* g_fp_override = nullptr;   // big-endian program bytes standing for guest memory at g_fp_override_ea
u32 g_fp_override_ea = 0, g_fp_override_size = 0;
FORCE_INLINE u32 fp_word(u32 ea) {
    u32 v;
    if (__builtin_expect(g_fp_override != nullptr, 0)) {
        u32 o = ea - g_fp_override_ea;
        if (o + 4 > g_fp_override_size) return 0;
        memcpy(&v, g_fp_override + o, 4);
        v = __builtin_bswap32(v);
    } else v = ld32(ea);
    return (v << 16) | (v >> 16);
}

// The RSX half registers share the file of the full ones: H[2k] is stored in R[k].xy and H[2k+1] in R[k].zw, two
// halves per 32-bit word. The shader compiler packs both into the same storage when their lifetimes allow (the crowd
// shader writes H17.w right after its last read of R8.w) and moves packed halves with full-precision MOVs: with
// separate arrays the crowd's fallback lookup used stale coordinates and dressed every figure in the skin colour of
// its atlas cell. The file is kept as raw bits (uvec4 R[]): a packed pair whose high half is zero is a denormal
// float, which the GPU flushes to zero when it is copied as a float.
bool fp_h_alias() { static const bool v = getenv("F1CE_NO_H_ALIAS") == nullptr; return v; }   // debug: separate H/R

std::string fp_r_read(u32 n) {
    return fp_h_alias() ? "uintBitsToFloat(R[" + std::to_string(n) + "])" : "R[" + std::to_string(n) + "]";
}

std::string fp_h_read(u32 n) {
    if (!fp_h_alias()) return "H[" + std::to_string(n) + "]";
    return std::string(n & 1 ? "h_hi(R[" : "h_lo(R[") + std::to_string(n >> 1) + "])";
}

std::string fp_h_write(u32 n, const std::string& mask, const std::string& e) {
    if (!fp_h_alias()) return masked_assign("H[" + std::to_string(n) + "]", mask, e, false);
    return "{ vec4 hv = " + fp_h_read(n) + "; " + masked_assign("hv", mask, e, false) + " R[" + std::to_string(n >> 1) + "]." +
           (n & 1 ? "zw" : "xy") + " = h_pack(hv); }";
}

std::string fp_src(u32 s, u32 attr, bool abs_flag, int const_idx, bool& uses_const) {
    u32 type = s & 3, idx = (s >> 2) & 0x3F;
    bool half = (s >> 8) & 1;
    u32 sx = (s >> 9) & 3, sy = (s >> 11) & 3, sz = (s >> 13) & 3, sw = (s >> 15) & 3;
    bool neg = (s >> 17) & 1;
    std::string r;
    static const char* inputs[] = {"wpos", "v_col0", "v_col1", "fogc", "v_tex[0]", "v_tex[1]", "v_tex[2]",
                                   "v_tex[3]", "v_tex[4]", "v_tex[5]", "v_tex[6]", "v_tex[7]", "v_tex[8]", "v_tex[9]",
                                   "vec4(gl_FrontFacing ? 1.0 : -1.0)", "vec4(0.0)"};
    switch (type) {
        case 0: r = half ? fp_h_read(idx) : fp_r_read(idx); break;
        case 1: r = inputs[attr & 15]; break;
        case 2: r = "FC[" + std::to_string(const_idx) + "]"; uses_const = true; break;
        default: r = "vec4(0.0)";
    }
    r = "(" + r + ")" + swizzle_str(sx, sy, sz, sw);
    if (abs_flag) r = "abs(" + r + ")";
    if (neg) r = "-" + r;
    return r;
}

bool g_fp_stop_here = false;   // F1CE_FP_STOP applies to the program being translated (F1CE_FP_STOP_HASH)

u64 g_fp_hash_cur = 0;   // hash of the program being translated (debug options by hash)
FPInfo translate_fp(u32 ea, u32 shader_control, u32 cube_mask) {
    FPInfo info;
    std::ostringstream body;
    u32 off = 0;
    int nconst = 0;
    // IFE blocks: byte offsets (from the program start) where an else branch / the end of an if begins
    std::vector<u32> else_offs, end_offs;
    for (int guard = 0; guard < 2048; guard++) {
        for (auto it = std::find(end_offs.begin(), end_offs.end(), off); it != end_offs.end();
             it = std::find(end_offs.begin(), end_offs.end(), off)) {
            end_offs.erase(it);
            body << "}\n";
        }
        for (auto it = std::find(else_offs.begin(), else_offs.end(), off); it != else_offs.end();
             it = std::find(else_offs.begin(), else_offs.end(), off)) {
            else_offs.erase(it);
            body << "} else {\n";
        }
        u32 w0 = fp_word(ea + off), w1 = fp_word(ea + off + 4), w2 = fp_word(ea + off + 8), w3 = fp_word(ea + off + 12);
        bool end = w0 & 1;
        u32 dst = (w0 >> 1) & 0x3F;
        bool dst_half = (w0 >> 7) & 1;
        std::string mask = mask_str((w0 >> 9) & 1, (w0 >> 10) & 1, (w0 >> 11) & 1, (w0 >> 12) & 1);
        u32 attr = (w0 >> 13) & 0xF, tex = (w0 >> 17) & 0xF;
        u32 op = ((w0 >> 24) & 0x3F) | (((w2 >> 31) & 1) << 6);
        bool no_dest = (w0 >> 30) & 1, sat = (w0 >> 31) & 1;
        u32 scale = (w2 >> 28) & 7;
        bool uses_const = false, flow = op >= 0x40 && op <= 0x45;   // BRK CAL IFE LOOP REP RET: no sources
        std::string a, b, c;
        if (!flow) {
            a = fp_src(w1, attr, (w1 >> 29) & 1, nconst, uses_const);
            b = fp_src(w2, attr, (w2 >> 18) & 1, nconst, uses_const);
            c = fp_src(w3, attr, (w3 >> 18) & 1, nconst, uses_const);
        }
        off += 16;
        if (uses_const) {
            info.const_offsets.push_back(off);
            nconst++;
            off += 16;
        }
        // condition codes: writes execute per component where CC[cond_reg].swizzle compares (<, ==, >) 0 as enabled;
        // set_cond also stores the result into CC[cond_mod_reg]
        u32 cond = (w1 >> 18) & 7;
        bool cond_all = cond == 7, set_cond = (w0 >> 8) & 1;
        static const bool cc_swap = getenv("F1CE_FP_CCSWAP") != nullptr;   // debug: swap the tested / written CC bits
        std::string ccv = "CC[" + std::to_string((w1 >> (cc_swap ? 30 : 31)) & 1) + "]" +
                          swizzle_str((w1 >> 21) & 3, (w1 >> 23) & 3, (w1 >> 25) & 3, (w1 >> 27) & 3);
        std::string cond_mask = "(";
        if (cond & 1) cond_mask += "vec4(lessThan(" + ccv + ", vec4(0.0))) + ";
        if (cond & 2) cond_mask += "vec4(equal(" + ccv + ", vec4(0.0))) + ";
        if (cond & 4) cond_mask += "vec4(greaterThan(" + ccv + ", vec4(0.0))) + ";
        cond_mask += "vec4(0.0))";
        std::string e;
        // all lookups go through sample_unit(): v flip for surfaces, depth compare for zeta buffers
        auto tex2d = [&](const std::string& coord4, bool projective) {
            info.tex_used |= 1u << tex;
            std::string t = std::to_string(tex);
            if (cube_mask & (1u << tex)) return "remap(texture(tex" + t + ", (" + coord4 + ").xyz), " + t + ")";
            return "sample_unit(tex" + t + ", " + coord4 + ", " + t + ", " + (projective ? "true" : "false") + ")";
        };
        switch (op) {
            case 0x00: break;
            case 0x01: e = a; break;
            case 0x02: e = a + " * " + b; break;
            case 0x03: e = a + " + " + b; break;
            case 0x04: e = a + " * " + b + " + " + c; break;
            case 0x05: e = "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"; break;
            case 0x06: e = "vec4(dot(" + a + ", " + b + "))"; break;
            case 0x07: e = "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"; break;
            case 0x08: e = "min(" + a + ", " + b + ")"; break;
            case 0x09: e = "max(" + a + ", " + b + ")"; break;
            case 0x0A: e = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 0x0B: e = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 0x0C: e = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
            case 0x0D: e = "vec4(greaterThan(" + a + ", " + b + "))"; break;
            case 0x0E: e = "vec4(notEqual(" + a + ", " + b + "))"; break;
            case 0x0F: e = "vec4(equal(" + a + ", " + b + "))"; break;
            case 0x10: e = "fract(" + a + ")"; break;
            case 0x11: e = "floor(" + a + ")"; break;
            case 0x12:   // KIL: discard where the condition holds
                body << "if (" << (cond_all ? "true" : "any(greaterThan(" + cond_mask + ", vec4(0.5)))") << ") discard;\n";
                break;
            case 0x15: e = "dFdx(" + a + ")"; break;
            case 0x16: e = "dFdy(" + a + ")"; break;
            case 0x17: e = tex2d(a, false); break;
            case 0x18: e = tex2d(a, true); break;
            case 0x19: e = tex2d(a, false); break;
            case 0x1A: e = "vec4(1.0 / (" + a + ").x)"; break;
            case 0x1B: e = "vec4(inversesqrt(abs((" + a + ").x)))"; break;
            case 0x1C: e = "vec4(exp2((" + a + ").x))"; break;
            case 0x1D: e = "vec4(log2((" + a + ").x))"; break;
            case 0x1E: e = "vec4(1.0, max((" + a + ").x, 0.0), ((" + a + ").x > 0.0) ? pow(max((" + a + ").y, 0.0), (" + a + ").w) : 0.0, 1.0)"; break;
            case 0x1F: e = "mix(" + c + ", " + b + ", " + a + ")"; break;
            case 0x20: e = "vec4(1.0)"; break;
            case 0x21: e = "vec4(0.0)"; break;
            case 0x22: e = "vec4(cos((" + a + ").x))"; break;
            case 0x23: e = "vec4(sin((" + a + ").x))"; break;
            case 0x26: e = "vec4(pow((" + a + ").x, (" + b + ").x))"; break;
            case 0x2F: e = tex2d(a, false); break;
            case 0x31: e = tex2d(a, false); break;
            case 0x38: e = "vec4(dot((" + a + ").xy, (" + b + ").xy))"; break;
            case 0x39: e = "vec4(normalize((" + a + ").xyz), 1.0)"; break;
            case 0x3A: e = a + " / (" + b + ").x"; break;
            case 0x3B: e = a + " * inversesqrt(abs((" + b + ").x))"; break;
            case 0x3D: case 0x3E: break;   // FENCT / FENCB: ordering fences, no effect here
            case 0x42: {   // IFE: if (condition) { .. } else { .. }; word 2 = else, word 3 = endif (in 32-bit words)
                // the crowd shader picks each figure's clothes in such a branch: running both sides painted every
                // figure in the skin colour of its atlas cell
                u32 else_off = (w2 & 0x7FFFFFFF) << 2, end_off = (w3 & 0x7FFFFFFF) << 2;
                body << "if (" << (cond_all ? "true" : "any(greaterThan(" + cond_mask + ", vec4(0.5)))") << ") {\n";
                if (else_off != end_off) else_offs.push_back(else_off);
                end_offs.push_back(end_off);
                break;
            }
            default: LOG_W("rsx fp: unsupported opcode 0x%x", op);
        }
        {   // raw words (w0 w1 w2 w3, halves swapped) for decoding checks
            char raw[48];
            snprintf(raw, sizeof(raw), " [%08x %08x %08x %08x]", w0, w1, w2, w3);
            body << "// i" << guard << " op " << std::hex << op << std::dec << " cond " << cond << (set_cond ? " setcc" : "") << raw << "\n";
        }
        if (!e.empty() && !mask.empty() && cond != 0 && (!no_dest || set_cond)) {
            static const char* scales[] = {"", " * 2.0", " * 4.0", " * 8.0", "", " / 2.0", " / 4.0", " / 8.0"};
            std::string ex = "(" + e + ")" + scales[scale];
            if (sat) ex = "clamp(" + ex + ", 0.0, 1.0)";
            std::string reg = dst_half ? fp_h_read(dst) : fp_r_read(dst);
            std::string rbits = "R[" + std::to_string(dst) + "]";
            // a plain MOV between full registers copies bits (packed halves included)
            bool bit_move = fp_h_alias() && op == 0x01 && !dst_half && (w1 & 3) == 0 && !((w1 >> 8) & 1) && !sat &&
                            scale == 0 && !((w1 >> 17) & 1) && !((w1 >> 29) & 1);
            std::string move_src = "R[" + std::to_string((w1 >> 2) & 0x3F) + "]" +
                                   swizzle_str((w1 >> 9) & 3, (w1 >> 11) & 3, (w1 >> 13) & 3, (w1 >> 15) & 3);
            auto write = [&](const std::string& v) {
                if (dst_half) return fp_h_write(dst, mask, v);
                if (!fp_h_alias()) return masked_assign(reg, mask, v, false);
                return masked_assign(rbits, mask, "floatBitsToUint(" + v + ")", false);
            };
            std::string cc = "CC[" + std::to_string((w1 >> (cc_swap ? 31 : 30)) & 1) + "]";
            if (cond_all) {
                if (!no_dest) body << (bit_move ? masked_assign(rbits, mask, move_src, false) : write(ex)) << "\n";
                static const bool no_setcc = getenv("F1CE_FP_NOSETCC") != nullptr;   // debug experiment
                if (set_cond && !no_setcc) body << masked_assign(cc, mask, ex, false) << "\n";
            } else {
                body << "{ vec4 t = " << ex << "; vec4 m = step(vec4(0.5), " << cond_mask << ");\n";
                if (!no_dest && bit_move) body << masked_assign(rbits, mask, "r_sel(" + rbits + ", " + move_src + ", m)", false) << "\n";
                else if (!no_dest && !dst_half && fp_h_alias())
                    body << masked_assign(rbits, mask, "r_sel(" + rbits + ", floatBitsToUint(t), m)", false) << "\n";
                else if (!no_dest) body << write("mix(" + reg + ", t, m)") << "\n";
                if (set_cond) body << masked_assign(cc, mask, "mix(" + cc + ", t, m)", false) << "\n";
                body << "}\n";
            }
        }
        // debug: F1CE_FP_STOP=<n>:<R|H><m> outputs register m right after instruction n (every program that long)
        static const char* fp_stop = getenv("F1CE_FP_STOP");
        if (fp_stop && g_fp_stop_here && guard == atoi(fp_stop)) {
            const char* r = strchr(fp_stop, ':');
            if (r && r[1] == 'v')   // interpolated input v_tex[m]
                body << "{ frag_color = vec4(abs(v_tex[" << atoi(r + 2) << "].xyz), 1.0); return; }\n";
            else if (r && r[1] == 'f')   // fog: red = factor (fogc.y), green = fog input / 1000
                body << "{ frag_color = vec4(clamp(fogc.y, 0.0, 1.0), clamp(abs(fogc.x) / 1000.0, 0.0, 1.0), 0.0, 1.0); return; }\n";
            else if (r && r[1] == 'c')
                body << "{ frag_color = vec4(abs(v_col0.xyz), 1.0); return; }\n";
            else if (r && r[1] == 'C')   // condition register: > 0 / == 0 / < 0 per component y,z,w -> 1.0 / 0.5 / 0.0
                body << "{ vec4 x = CC[" << atoi(r + 2) << "]; frag_color = vec4(step(0.0, x.yzw) * 0.5 + step(0.0, -x.yzw) * -0.5 + 0.5, 1.0); return; }\n";
            else if (r && (r[1] == 'R' || r[1] == 'H') && getenv("F1CE_FP_STOP_NAN"))   // red = NaN, green = Inf
                body << "{ vec3 x = " << (r[1] == 'H' ? fp_h_read(atoi(r + 2)) : fp_r_read(atoi(r + 2))) << ".xyz; frag_color = vec4(any(isnan(x)) ? 1.0 : 0.0, "
                     << "any(isinf(x)) ? 1.0 : 0.0, 0.0, 1.0); return; }\n";
            else if (r && (r[1] == 'R' || r[1] == 'H'))
                body << "{ frag_color = vec4(abs(" << (r[1] == 'H' ? fp_h_read(atoi(r + 2)) : fp_r_read(atoi(r + 2))) << "."
                     << (getenv("F1CE_FP_STOP_SWZ") ? getenv("F1CE_FP_STOP_SWZ") : "xyz") << ") * "   // e.g. wwx
                     << (getenv("F1CE_FP_STOP_SCALE") ? getenv("F1CE_FP_STOP_SCALE") : "1.0") << ", 1.0); return; }\n";
        }
        if (end) break;
    }
    for (size_t i = 0; i < end_offs.size(); i++) body << "}\n";   // blocks that end with the program
    info.size = off;
    std::ostringstream src;
    src << "#version 420 core\n";
    // debug: F1CE_SHADER_SALT=<text> makes every program new to the disk cache (cold compile tests)
    if (const char* salt = getenv("F1CE_SHADER_SALT")) src << "// " << salt << "\n";
    src << "in vec4 v_col0; in vec4 v_col1; in vec4 v_fog; in vec4 v_tex[10];\n";
    for (int i = 0; i < 16; i++) src << ((cube_mask >> i) & 1 ? "uniform samplerCube tex" : "uniform sampler2D tex") << i << ";\n";
    // u_tex_scale: 1/size for textures with unnormalized (texel) coordinates (format bit 0x40), else 1
    if (scaled_glsl()) src << "uniform float u_ui_clamp;\n";
    src << "uniform float u_tex_flip[16];\nuniform vec2 u_tex_scale[16];\n"
           "vec2 tex_coord(vec2 c, int u) { c *= u_tex_scale[u]; return u_tex_flip[u] > 0.5 ? vec2(c.x, 1.0 - c.y) : c; }\n"
           "uniform float u_tex_depth[16];\n"
           // TEXTURE_CONTROL1 remap: bits 2i..2i+1 = source (0 A, 1 R, 2 G, 3 B) of output i (0 A, 1 R, 2 G, 3 B),
           // bits 8+2i.. = 0 zero, 1 one, 2 the source; 0xaae4 is the identity
           // bits 16-19: TEXTURE_ADDRESS gamma (sRGB decode of R, G, B, A), bit 20: unsigned remap BIASED (x * 2 - 1),
           // applied to the fetched texel before the remap
           "uniform float u_remap[16];\n"

           "vec4 remap(vec4 v, int u) {\n"
           " int r = int(u_remap[u]), c = r >> 16; r &= 0xFFFF;\n"
           " if (c != 0) {\n"
           "  vec4 m = vec4(ivec4(c, c >> 1, c >> 2, c >> 3) & 1);\n"
           "  v = mix(v, v * (v * (v * 0.305306011 + 0.682171111) + 0.012522878), m);\n"
           "  if ((c & 16) != 0) v = v * 2.0 - 1.0;\n"
           " }\n"
           " if (r == 0xaae4) return v;\n"
           " vec4 s = vec4(v.a, v.r, v.g, v.b); vec4 o;\n"
           " for (int i = 0; i < 4; i++) { int op = (r >> (8 + 2 * i)) & 3; o[i] = op == 0 ? 0.0 : op == 1 ? 1.0 : s[(r >> (2 * i)) & 3]; }\n"
           " return vec4(o.y, o.z, o.w, o.x);\n"
           "}\n"
           "vec4 sample_unit(sampler2D t, vec4 c, int u, bool proj) {\n"
           " vec3 p = proj ? c.xyz / c.w : c.xyz;\n";
    // resolution scaling: draws without depth test (2D interface, full-screen passes) keep coordinates inside [0, 1]
    // half a texel from the edges. Their quads map textures 1:1 onto pixels: at native size every sample hits a
    // texel centre, at a higher scale the edge samples blended in the opposite edge of repeating textures (a
    // coloured line down the Quick Race menu at the portrait's edge). Coordinates outside [0, 1] repeat as before.
    if (scaled_glsl())
        src << " vec2 tc = tex_coord(p.xy, u);\n"
               " if (u_ui_clamp > 0.5 && all(greaterThanEqual(tc, vec2(0.0))) && all(lessThanEqual(tc, vec2(1.0)))) {\n"
               "  vec2 hh = 0.5 / vec2(textureSize(t, 0)); tc = clamp(tc, hh, 1.0 - hh);\n"
               " }\n"
               " vec4 v = texture(t, tc);\n";
    else
        src << " vec4 v = texture(t, tex_coord(p.xy, u));\n";
    src << " int f = int(u_tex_depth[u]);\n"
           " if (f == 0) return remap(v, u);\n"
           " if (f == 9) {   // D24S8 zeta read with a colour format: A = d[23:16], R = d[15:8], G = d[7:0], B = stencil\n"
           "  uint z = uint(v.r * 16777215.0 + 0.5);\n"
           "  return remap(vec4(float((z >> 8) & 255u), float(z & 255u), 0.0, float(z >> 16)) / 255.0, u);\n"
           " }\n"
           " if (f == 12) return remap(vec4(v.b), u);   // B8 render target (stores blue) read as a B8 texture\n"
           " float d = v.r, r = clamp(p.z, 0.0, 1.0); bool pass;\n"
           " if (f == 10) return vec4(d); if (f == 11) return vec4(p.z);   // debug (F1CE_ZFUNC): stored depth / reference\n"
           // linear filtering of a depth compare (f + 16) is percentage-closer filtering on the RSX: the four texels
           // are compared, then the results are weighted bilinearly. Comparing the filtered depth spread wrong
           // shadows along depth edges (dark smears around the wheels and suspension).
           " if (f >= 16) {\n"
           "  f -= 16;\n"
           "  vec2 sz = vec2(textureSize(t, 0)), q = tex_coord(p.xy, u) * sz - 0.5, w = fract(q);\n"
           "  vec4 g = textureGather(t, (floor(q) + 1.0) / sz, 0);   // (x0,y1) (x1,y1) (x1,y0) (x0,y0)\n"
           "  vec4 c;\n"
           "  for (int i = 0; i < 4; i++) {\n"
           "   float e = g[i]; bool ps;\n"
           "   if (f == 1) ps = false; else if (f == 2) ps = e < r; else if (f == 3) ps = e == r;\n"
           "   else if (f == 4) ps = e <= r; else if (f == 5) ps = e > r; else if (f == 6) ps = e != r;\n"
           "   else if (f == 7) ps = e >= r; else ps = true;\n"
           "   c[i] = ps ? 1.0 : 0.0;\n"
           "  }\n"
           "  return vec4(mix(mix(c.w, c.z, w.x), mix(c.x, c.y, w.x), w.y));\n"
           " }\n"
           // RSX texture depth compare is "stored FUNC reference" (the reverse of GL's ref-to-texture order); the GL
           // order put the player car in its own shadow and darkened the whole race scene
           " if (f == 1) pass = false; else if (f == 2) pass = d < r; else if (f == 3) pass = d == r;\n"
           " else if (f == 4) pass = d <= r; else if (f == 5) pass = d > r; else if (f == 6) pass = d != r;\n"
           " else if (f == 7) pass = d >= r; else pass = true;\n"
           " return vec4(pass ? 1.0 : 0.0);\n"
           "}\n";
    // FOGC input: x = the VP fog output, y = fog factor from FOG_MODE / FOG_PARAMS (u_fog = mode, p0, p1)
    src << "vec4 h_lo(uvec4 r) { return vec4(unpackHalf2x16(r.x), unpackHalf2x16(r.y)); }\n"
           "vec4 h_hi(uvec4 r) { return vec4(unpackHalf2x16(r.z), unpackHalf2x16(r.w)); }\n"
           "uvec2 h_pack(vec4 v) { return uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw)); }\n"
           "uvec4 r_sel(uvec4 a, uvec4 b, vec4 m) {\n"
           " return uvec4(m.x > 0.5 ? b.x : a.x, m.y > 0.5 ? b.y : a.y, m.z > 0.5 ? b.z : a.z, m.w > 0.5 ? b.w : a.w);\n"
           "}\n";
    src << "uniform vec4 u_fog;\n"
           "vec4 fog_value() {\n"
           " float x = v_fog.x, p0 = u_fog.y, p1 = u_fog.z, y; int m = int(u_fog.x);\n"
           " if (m >= 3) x = abs(x);   // *_ABS modes\n"
           " if (m == 0 || m == 3) y = p1 * x + (p0 - 1.0);\n"
           " else if (m == 1 || m == 4) y = exp(11.084 * (p1 * x + p0 - 1.5));\n"
           " else y = exp(-pow(4.709 * (p1 * x + p0 - 1.5), 2.0));\n"
           " return vec4(v_fog.x, clamp(y, 0.0, 1.0), 0.0, 0.0);\n"
           "}\n";
    if (scaled_glsl()) src << "uniform float u_res_scale;\n";
    src << "uniform vec4 FC[" << std::max(nconst, 1) << "];\nuniform vec4 u_alpha_test;\nuniform vec2 u_prog_id;\nout vec4 frag_color;\n"
           "void main() {\n vec4 CC[2]; CC[0] = vec4(0.0); CC[1] = vec4(0.0);\n vec4 fogc = fog_value();\n";
    // WPOS.w is the clip-space w on the RSX; gl_FragCoord.w is 1/w. The crowd impostors multiply a texture coordinate
    // by it to recover texel coordinates in their figure table: with 1/w they read texels near the origin (empty)
    static const bool wpos_raw_all = getenv("F1CE_WPOS_CLIP_W") == nullptr;   // debug experiment: WPOS.w = clip w
    // F1CE_WPOS_CLIP_W_FP=<fp hash>,...: the experiment for those programs only (bisecting)
    static const std::string wpos_fps = getenv("F1CE_WPOS_CLIP_W_FP") ? "," + std::string(getenv("F1CE_WPOS_CLIP_W_FP")) + "," : "";
    char fhk[24];
    snprintf(fhk, sizeof(fhk), ",%016llx,", (unsigned long long)g_fp_hash_cur);
    bool listed = wpos_fps.find(fhk) != std::string::npos, except = wpos_fps.size() > 1 && wpos_fps[1] == '-';   // "-<hash>,..": all but
    bool wpos_raw = wpos_raw_all && (wpos_fps.empty() || (except ? listed : !listed));
    if (scaled_glsl())
        src << " vec4 wpos = vec4(gl_FragCoord.xy / u_res_scale, gl_FragCoord.z, " << (wpos_raw ? "gl_FragCoord.w" : "1.0 / gl_FragCoord.w") << ");\n";
    else
        src << (wpos_raw ? " vec4 wpos = gl_FragCoord;\n" : " vec4 wpos = vec4(gl_FragCoord.xyz, 1.0 / gl_FragCoord.w);\n");
    {   // only the registers the program uses (R1 for depth export, H0/R0 for the output)
        int nr = array_size(body.str(), "R", 2), nh = array_size(body.str(), "H", 1);
        src << (fp_h_alias() ? " uvec4 R[" : " vec4 R[") << nr << "]; for (int i = 0; i < " << nr << "; i++) R[i] = "
            << (fp_h_alias() ? "uvec4(0u)" : "vec4(0.0)") << ";\n"
            << " vec4 H[" << nh << "]; for (int i = 0; i < " << nh << "; i++) H[i] = vec4(0.0);\n";
    }
    src << body.str();
    static const bool force_r0 = getenv("F1CE_FP_FORCE_R0") != nullptr;   // debug experiment
    src << " frag_color = " << ((shader_control & 0x40) || force_r0 ? fp_r_read(0) : fp_h_read(0)) << ";\n";
    if (shader_control & 0xE) src << " gl_FragDepth = " << fp_r_read(1) << ".z;\n";   // depth export: fragment depth from R1.z
    // debug: F1CE_FP_DEBUG=1 shows the raw unit-0 texture for shaders that sample it (bypasses lighting)
    //        F1CE_FP_DEBUG=<n> (n = 0..9) shows the interpolated input v_tex[n].xyz instead
    static const char* fp_debug = getenv("F1CE_FP_DEBUG");
    if (fp_debug && (info.tex_used & 1)) {
        int n = atoi(fp_debug);
        if (*fp_debug == 't') src << " frag_color = vec4(texture(tex0, tex_coord(v_tex[0].xy, 0)).rgb, frag_color.a);\n";
        else if (*fp_debug == 'R' || *fp_debug == 'H')   // F1CE_FP_DEBUG=R3 / H1: show a register's final value
            src << " frag_color = vec4(abs(" << (*fp_debug == 'H' ? fp_h_read(std::clamp(atoi(fp_debug + 1), 0, 63)) : fp_r_read(std::clamp(atoi(fp_debug + 1), 0, 63))) << ".xyz), frag_color.a);\n";
        else src << " frag_color = vec4(abs(v_tex[" << std::clamp(n, 0, 9) << "].xyz), frag_color.a);\n";
    }
    // debug: F1CE_PROG_ID=1 paints every program in a colour encoding its id (R = id & 255, G = id >> 8)
    if (getenv("F1CE_PROG_ID")) src << " frag_color = vec4(u_prog_id.xy, 0.5, 1.0);\n";
    // alpha test: x = enabled, y = func (GL enum - 0x200), z = ref
    src << " if (u_alpha_test.x > 0.5) {\n"
           "  int f = int(u_alpha_test.y); float a = frag_color.a, r = u_alpha_test.z; bool pass = true;\n"
           "  if (f == 0) pass = false; else if (f == 1) pass = a < r; else if (f == 2) pass = a == r;\n"
           "  else if (f == 3) pass = a <= r; else if (f == 4) pass = a > r; else if (f == 5) pass = a != r;\n"
           "  else if (f == 6) pass = a >= r;\n"
           "  if (!pass) discard;\n }\n"
           // SHADER_PACKER (0x1FD8) bit 0: sRGB output (the game lights in linear space: textures are decoded on fetch
           // by their TEXTURE_ADDRESS gamma bits and the result is encoded when written)
           " if (u_alpha_test.w > 0.5) {\n"
           "  vec3 c = clamp(frag_color.rgb, 0.0, 1.0);\n"
           "  frag_color.rgb = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
           " }\n}\n";
    info.glsl = src.str();
    return info;
}

struct Program {
    GLuint prog = 0;
    bool uses_vtex = false;   // the vertex program fetches from vertex textures
    u32 c_ver = 0;   // vp_ver when C[] was last uploaded to this program
    u64 fh = 0;      // fragment program hash (fp_<hash>.glsl with F1CE_DUMP_SHADERS)
    u32 tex_used = 0;   // texture units the fragment program samples
    std::shared_ptr<struct CompileJob> job;   // background link in progress (draws are skipped until done)
    GLint loc_c = -1, loc_fc = -1, loc_scale = -1, loc_offset = -1, loc_surface = -1, loc_alpha = -1, loc_fog = -1;
    GLint loc_tex[16];
    GLint loc_flip = -1, loc_depth = -1, loc_tex_scale = -1, loc_remap = -1, loc_va = -1, loc_res_scale = -1, loc_ui_clamp = -1;
    float u_res = -1, u_ui = -1;
    int u_va[64];   // last uploaded u_va (vertex pulling)
    // last uploaded uniform values (uploads are skipped when unchanged); NaN-filled so the first upload happens
    float u_scale[4], u_offset[4], u_surface[4], u_alpha[4], u_fog[4], u_flip[16], u_tex_scale[32], u_depth[16], u_remap[16];
    std::vector<float> u_fc;
    Program() { memset(&u_scale, 0xFF, (char*)(&u_remap + 1) - (char*)&u_scale); memset(u_va, 0xFF, sizeof(u_va)); }
};
// uploads a uniform when its value differs from the program's cached copy
const bool g_no_state_cache = getenv("F1CE_NO_STATE_CACHE") != nullptr;   // debug: send all GL state every draw
template <size_t N> void uniform_cached(GLint loc, float (&cache)[N], const float* v, void (*up)(GLint, GLsizei, const GLfloat*), GLsizei count) {
    if (loc < 0 || (!g_no_state_cache && memcmp(cache, v, sizeof(cache)) == 0)) return;
    memcpy(cache, v, sizeof(cache));
    up(loc, count, v);
}
std::unordered_map<u64, Program> g_programs;
std::unordered_map<u64, std::pair<GLuint, FPInfo>> g_fp_cache;   // fp hash -> shader (compiled on first link)
struct VPEntry { GLuint sh = 0; std::string glsl; };
std::unordered_map<u64, VPEntry> g_vp_cache;

// Linked programs are cached on disk (shader_cache/ next to the executable, keyed by the GLSL sources and the GL
// driver): compiling the few hundred programs of a race scene took seconds on the first frames, during which no
// frame was presented (white screen after loading).
std::string shader_cache_dir() {
    static std::string dir = [] {
        std::string d = "shader_cache";
#ifdef _WIN32
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::string s(path, n);
            size_t slash = s.find_last_of("\\/");
            if (slash != std::string::npos) d = s.substr(0, slash + 1) + "shader_cache";
        }
        CreateDirectoryA(d.c_str(), nullptr);
#endif
        return d;
    }();
    return dir;
}
u64 g_compile_us = 0, g_compiled = 0, g_cache_hits = 0, g_preload_hits = 0;

// Program preload: loading a cached binary when a program is first drawn costs ~10 ms (glProgramBinary), which made
// the race stutter whenever a new effect appeared. The programs of earlier sessions (shader_cache/used.txt, keys in
// order of first use) are loaded at start-up on a second shared context; get_program takes them from g_preloaded.
std::mutex g_pre_m;
std::unordered_map<u64, GLuint> g_preloaded;   // disk cache key -> linked program, ready for the render context
std::set<u64> g_used_keys;                     // keys listed in used.txt (render thread only)
std::string shader_cache_dir();

void start_preload() {
    static bool started = false;
    if (started) return;
    started = true;
    std::vector<u64> keys;
    if (FILE* f = fopen((shader_cache_dir() + "/used.txt").c_str(), "r")) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            u64 k = strtoull(line, nullptr, 16);
            if (k && g_used_keys.insert(k).second) keys.push_back(k);
        }
        fclose(f);
    }
    if (keys.empty() || getenv("F1CE_NO_PRELOAD")) return;
    void* ctx = host::gl_create_shared_context();
    if (!ctx) return;
    std::thread([ctx, keys] {
        host::gl_make_current(ctx);
        u64 t0 = time_usec();
        u32 n = 0;
        std::vector<std::pair<u64, GLuint>> batch;
        auto publish = [&] {   // the programs are complete before the render context may use them
            glFinish();
            std::lock_guard lk(g_pre_m);
            for (auto& [k, prog] : batch) g_preloaded[k] = prog;
            batch.clear();
        };
        for (u64 k : keys) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%016llx.bin", shader_cache_dir().c_str(), (unsigned long long)k);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            u32 format = 0;
            std::vector<u8> bin;
            if (fread(&format, 4, 1, f) == 1) {
                fseek(f, 0, SEEK_END);
                long len = ftell(f) - 4;
                fseek(f, 4, SEEK_SET);
                if (len > 0) { bin.resize((size_t)len); if (fread(bin.data(), 1, bin.size(), f) != bin.size()) bin.clear(); }
            }
            fclose(f);
            if (bin.empty()) continue;
            GLuint prog = glCreateProgram();
            glProgramBinary(prog, format, bin.data(), (GLsizei)bin.size());
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) { glDeleteProgram(prog); continue; }
            batch.push_back({k, prog});
            n++;
            if (batch.size() >= 16) publish();
        }
        publish();
        LOG_I("rsx: preloaded %u of %zu programs used before in %.0f ms", n, keys.size(), (time_usec() - t0) / 1e3);
    }).detach();
}
u64 g_start_us = 0;   // first flip

u64 hash_words(const u32* p, size_t n, u64 h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// hashes the FP instructions only (inline constants excluded so constant changes reuse the shader)
u64 hash_fp(u32 ea, u32& size_out) {
    u64 h = 1469598103934665603ull;
    u32 off = 0;
    for (int guard = 0; guard < 2048; guard++) {
        u32 w[4];
        for (int i = 0; i < 4; i++) w[i] = fp_word(ea + off + i * 4);
        h = hash_words(w, 4, h);
        off += 16;
        bool uses_const = (w[1] & 3) == 2 || (w[2] & 3) == 2 || (w[3] & 3) == 2;
        if (uses_const) off += 16;
        if (w[0] & 1) break;
    }
    size_out = off;
    return h;
}

// uniform locations and sampler units of a linked program
void setup_program(Program& p) {
    glUseProgram(p.prog);
    g_cur_prog = p.prog;
    p.loc_c = glGetUniformLocation(p.prog, "C");
    p.loc_fc = glGetUniformLocation(p.prog, "FC");
    p.loc_scale = glGetUniformLocation(p.prog, "u_vp_scale");
    p.loc_offset = glGetUniformLocation(p.prog, "u_vp_offset");
    p.loc_surface = glGetUniformLocation(p.prog, "u_surface");
    p.loc_alpha = glGetUniformLocation(p.prog, "u_alpha_test");
    if (GLint l = glGetUniformLocation(p.prog, "u_prog_id"); l >= 0) { float id[2] = {(p.prog & 255) / 255.0f, (p.prog >> 8) / 255.0f}; glUniform2fv(l, 1, id); }
    p.loc_fog = glGetUniformLocation(p.prog, "u_fog");
    p.loc_flip = glGetUniformLocation(p.prog, "u_tex_flip");
    p.loc_tex_scale = glGetUniformLocation(p.prog, "u_tex_scale");
    p.loc_depth = glGetUniformLocation(p.prog, "u_tex_depth");
    p.loc_remap = glGetUniformLocation(p.prog, "u_remap");
    p.loc_va = glGetUniformLocation(p.prog, "u_va");
    p.loc_res_scale = glGetUniformLocation(p.prog, "u_res_scale");
    p.loc_ui_clamp = glGetUniformLocation(p.prog, "u_ui_clamp");
    if (GLint l = glGetUniformLocation(p.prog, "u_va_ring"); l >= 0) glUniform1i(l, 20);   // vertex pulling buffers
    if (GLint l = glGetUniformLocation(p.prog, "u_va_heap"); l >= 0) glUniform1i(l, 21);
    for (int i = 0; i < 16; i++) {
        char n[8];
        snprintf(n, sizeof(n), "tex%d", i);
        p.loc_tex[i] = glGetUniformLocation(p.prog, n);
        if (p.loc_tex[i] >= 0) glUniform1i(p.loc_tex[i], i);
    }
    for (int i = 0; i < 4; i++) {   // vertex textures on GL units 16..19
        char n[8];
        snprintf(n, sizeof(n), "vtex%d", i);
        GLint l = glGetUniformLocation(p.prog, n);
        if (l >= 0) { glUniform1i(l, 16 + i); p.uses_vtex = true; }
    }
}

// background program linking on a second GL context that shares objects with the renderer's
struct CompileJob {
    GLuint prog = 0;
    std::string vs, fs, path;
    std::atomic<bool> done{false};
    std::vector<u8> bin;   // linked program binary (empty: link failed)
    GLenum format = 0;
    std::string log;
};
// The compile thread links in a context of its own share group and hands over the program binary, which the
// renderer loads with glProgramBinary (at most two per frame). Linking in a context that shared objects with the
// renderer's made the AMD driver hold up every renderer GL call during each 50-100 ms link: 0.3 fps while a new
// scene's programs compiled, and the PPU then overran the FIFO (crash entering the Driver garage).
u64 g_bin_loads_frame = ~0ull;
u32 g_bin_loads = 0;

std::mutex g_cq_m;
std::condition_variable g_cq_cv;
std::deque<std::shared_ptr<CompileJob>> g_cq;
int g_cq_active = 0;                   // jobs being compiled (under g_cq_m)
u64 g_cq_burst_start = 0, g_cq_burst_n = 0;
GLuint compile(GLenum type, const std::string& src);
bool start_compile_thread() {
    static int state = getenv("F1CE_SYNC_COMPILE") ? -1 : 0;   // F1CE_SYNC_COMPILE: link on the render thread
    if (state) return state > 0;
    // compile threads, each with a context of its own share group (F1CE_COMPILE_THREADS, default 1)
    // (more threads do not help on the AMD driver: it serialises the links, each of 4 parallel ones took ~6x longer
    // and the game slowed to ~15 fps while compiling)
    int nthreads = getenv("F1CE_COMPILE_THREADS") ? std::clamp(atoi(getenv("F1CE_COMPILE_THREADS")), 1, 16) : 1;
    std::vector<void*> ctxs;
    for (int i = 0; i < nthreads; i++)
        if (void* c = host::gl_create_shared_context(false)) ctxs.push_back(c);
    state = ctxs.empty() ? -1 : 1;
    if (ctxs.empty()) return false;
    LOG_I("rsx: %zu shader compile threads", ctxs.size());
    for (void* ctx : ctxs) std::thread([ctx] {
        host::gl_make_current(ctx);
        for (;;) {
            std::shared_ptr<CompileJob> job;
            {
                std::unique_lock lk(g_cq_m);
                g_cq_cv.wait(lk, [] { return !g_cq.empty(); });
                job = g_cq.front();
                g_cq.pop_front();
                if (g_cq_active++ == 0 && !g_cq_burst_start) g_cq_burst_start = time_usec();
            }
            u64 t0 = time_usec();
            GLuint prog = glCreateProgram();   // this context's own object
            GLuint vs = compile(GL_VERTEX_SHADER, job->vs), fs = compile(GL_FRAGMENT_SHADER, job->fs);
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glProgramParameteri(prog, 0x8257 /*GL_PROGRAM_BINARY_RETRIEVABLE_HINT*/, GL_TRUE);
            glLinkProgram(prog);
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (ok) {
                GLint len = 0;
                glGetProgramiv(prog, 0x8741 /*GL_PROGRAM_BINARY_LENGTH*/, &len);
                if (len > 0) {
                    job->bin.resize((size_t)len);
                    glGetProgramBinary(prog, len, nullptr, &job->format, job->bin.data());
                    if (FILE* f = fopen(job->path.c_str(), "wb")) { u32 fm = job->format; fwrite(&fm, 4, 1, f); fwrite(job->bin.data(), 1, job->bin.size(), f); fclose(f); }
                }
            } else {
                char log[4096] = {};
                glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
                job->log = log;
            }
            glDetachShader(prog, vs);
            glDetachShader(prog, fs);
            glDeleteShader(vs);
            glDeleteShader(fs);
            glDeleteProgram(prog);
            {
                std::lock_guard lk(g_cq_m);   // (threads add up concurrently)
                g_compile_us += time_usec() - t0;
                // a burst of compiles: wall time from its first job to the empty queue
                g_cq_burst_n++;
                if (--g_cq_active == 0 && g_cq.empty()) {
                    LOG_I("rsx: compile burst: %llu programs in %.0f ms", (unsigned long long)g_cq_burst_n, (time_usec() - g_cq_burst_start) / 1e3);
                    g_cq_burst_start = 0; g_cq_burst_n = 0;
                }
            }
            job->done = true;
        }
    }).detach();
    return true;
}

// Program database (shader_cache/programs.bin): the guest microcode of every program pair the game has used (vertex
// program slots, fragment program bytes, shader control, cube mask). At start-up, before the renderer runs its first
// command, every entry is translated again with the current translator and its GL program is loaded from the binary
// cache or compiled, with a progress bar in the window: programs are never compiled or loaded during play any more
// (stutter, white screens while a new screen's programs compiled), also after a translator change or a new scale.
// Programs the game has never used before are still compiled once when they first appear, and recorded.
struct ProgRecord { u32 magic, vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, pad; u64 vh, fh; };
constexpr u32 PROG_MAGIC = 0x46314442;   // "F1DB"
std::set<u64> g_db_keys;   // program pairs in the database (vh * 31 + fh)
bool g_db_loaded = false;

void db_record(u32 vp_start, u32 vp_len, u32 fp_ea, u32 fp_size, u32 shader_control, u32 cube_mask, u64 vh, u64 fh) {
    if (!g_db_loaded || !g_db_keys.insert(vh * 31 + fh).second || fp_size == 0 || fp_size > 65536) return;
    FILE* f = fopen((shader_cache_dir() + "/programs.bin").c_str(), "ab");
    if (!f) return;
    ProgRecord r{PROG_MAGIC, vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, 0, vh, fh};
    fwrite(&r, sizeof(r), 1, f);
    fwrite(&vp_words[vp_start * 4], 4, (size_t)vp_len * 4, f);
    std::vector<u8> fp(fp_size);
    for (u32 o = 0; o < fp_size; o += 4) { u32 v = __builtin_bswap32(ld32(fp_ea + o)); memcpy(&fp[o], &v, 4); }   // as in memory
    fwrite(fp.data(), 1, fp_size, f);
    fclose(f);
}

// the GLSL of a program pair and its disk cache key (as get_program builds them)
std::string program_renderer_id() {
    static const std::string r = (const char*)glGetString(GL_RENDERER) + std::string("|") + (const char*)glGetString(GL_VERSION);
    return r;
}
u64 program_src_key(const std::string& vs, const std::string& fs) {
    u64 k = 1469598103934665603ull;
    const std::string& rid = program_renderer_id();
    for (const std::string* str : {&rid, &vs, &fs})
        for (unsigned char ch : *str) { k ^= ch; k *= 1099511628211ull; }
    return k;
}

void progress_bar(double frac, const char* what) {
    GLint ww = 1280, wh = 720;
    host::gl_drawable_size(&ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    srgb_write(false);
    glViewport(0, 0, ww, wh);
    glClearColor(0.05f, 0.05f, 0.06f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    GLint bw = ww * 6 / 10, bh = std::max(8, wh / 60), bx = (ww - bw) / 2, by = wh / 2 - bh / 2;
    glScissor(bx - 2, by - 2, bw + 4, bh + 4);
    glClearColor(0.35f, 0.35f, 0.38f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(bx, by, std::max(1, (GLint)(bw * frac)), bh);
    glClearColor(0.86f, 0.10f, 0.10f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    host::gl_swap();
    (void)what;
    g_tgt_valid = g_state_valid = false;
}

void precompile_programs() {
    g_db_loaded = true;
    FILE* f = fopen((shader_cache_dir() + "/programs.bin").c_str(), "rb");
    if (!f) { LOG_I("rsx: no program database yet (shader_cache/programs.bin)"); return; }
    struct Entry { ProgRecord r; std::vector<u32> vp; std::vector<u8> fp; };
    std::vector<Entry> entries;
    for (;;) {
        Entry e;
        if (fread(&e.r, sizeof(e.r), 1, f) != 1 || e.r.magic != PROG_MAGIC || e.r.vp_start + e.r.vp_len > 512 || e.r.fp_size > 65536) break;
        e.vp.resize((size_t)e.r.vp_len * 4);
        e.fp.resize(e.r.fp_size);
        if (fread(e.vp.data(), 4, e.vp.size(), f) != e.vp.size() || fread(e.fp.data(), 1, e.fp.size(), f) != e.fp.size()) break;
        if (g_db_keys.insert(e.r.vh * 31 + e.r.fh).second) entries.push_back(std::move(e));
    }
    fclose(f);
    if (entries.empty()) return;
    static const bool off = getenv("F1CE_NO_PRECOMPILE") != nullptr;   // debug: skip the phase (database still recorded)
    if (off) return;
    u64 t0 = time_usec(), last_bar = 0;
    std::vector<u32> saved_vp(vp_words, vp_words + 512 * 4);
    u32 loaded = 0, compiled = 0, failed = 0;
    std::unordered_map<u64, std::string> vs_of;   // vertex programs are shared by many pairs: translated once
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        if (time_usec() - last_bar > 50000) { progress_bar((double)i / entries.size(), "shaders"); last_bar = time_usec(); }
        auto vit = vs_of.find(e.r.vh);
        if (vit == vs_of.end()) {
            memcpy(&vp_words[e.r.vp_start * 4], e.vp.data(), e.vp.size() * 4);
            vit = vs_of.emplace(e.r.vh, translate_vp(e.r.vp_start).glsl).first;
        }
        g_fp_override = e.fp.data(); g_fp_override_ea = e.r.fp_ea; g_fp_override_size = e.r.fp_size;
        g_fp_hash_cur = e.r.fh;
        g_fp_stop_here = false;
        std::string fs = translate_fp(e.r.fp_ea, e.r.shader_control, e.r.cube_mask).glsl;
        g_fp_override = nullptr;
        u64 key = program_src_key(vit->second, fs);
        {
            std::lock_guard lk(g_pre_m);
            if (g_preloaded.count(key)) continue;
        }
        GLuint prog = glCreateProgram();
        GLint ok = 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/%016llx.bin", shader_cache_dir().c_str(), (unsigned long long)key);
        if (FILE* bf = fopen(path, "rb")) {   // binary cache
            u32 format = 0;
            std::vector<u8> bin;
            if (fread(&format, 4, 1, bf) == 1) {
                fseek(bf, 0, SEEK_END);
                long n = ftell(bf) - 4;
                fseek(bf, 4, SEEK_SET);
                if (n > 0) { bin.resize((size_t)n); if (fread(bin.data(), 1, bin.size(), bf) != bin.size()) bin.clear(); }
            }
            fclose(bf);
            if (!bin.empty()) {
                glProgramBinary(prog, format, bin.data(), (GLsizei)bin.size());
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            }
            if (ok) loaded++;
        }
        if (!ok) {   // not cached (new translator, new driver): compiled now, before the game runs
            GLuint v = compile(GL_VERTEX_SHADER, vit->second), fr = compile(GL_FRAGMENT_SHADER, fs);
            glAttachShader(prog, v);
            glAttachShader(prog, fr);
            glProgramParameteri(prog, 0x8257 /*GL_PROGRAM_BINARY_RETRIEVABLE_HINT*/, GL_TRUE);
            glLinkProgram(prog);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (ok) {
                GLint len = 0;
                glGetProgramiv(prog, 0x8741 /*GL_PROGRAM_BINARY_LENGTH*/, &len);
                if (len > 0) {
                    std::vector<u8> bin((size_t)len);
                    GLenum format = 0;
                    glGetProgramBinary(prog, len, nullptr, &format, bin.data());
                    if (FILE* bf = fopen(path, "wb")) { u32 fm = format; fwrite(&fm, 4, 1, bf); fwrite(bin.data(), 1, bin.size(), bf); fclose(bf); }
                }
                compiled++;
            }
            glDetachShader(prog, v);
            glDetachShader(prog, fr);
            glDeleteShader(v);
            glDeleteShader(fr);
        }
        if (!ok) { glDeleteProgram(prog); failed++; continue; }
        std::lock_guard lk(g_pre_m);
        g_preloaded[key] = prog;
        if (!g_used_keys.count(key)) {
            g_used_keys.insert(key);
            if (FILE* uf = fopen((shader_cache_dir() + "/used.txt").c_str(), "a")) { fprintf(uf, "%016llx\n", (unsigned long long)key); fclose(uf); }
        }
    }
    memcpy(vp_words, saved_vp.data(), saved_vp.size() * 4);
    progress_bar(1.0, "shaders");
    LOG_I("rsx: program database: %zu programs ready in %.1f s (%u from the binary cache, %u compiled, %u failed)", entries.size(),
          (time_usec() - t0) / 1e6, loaded, compiled, failed);
}

Program* get_program(std::vector<float>& fp_consts) {
    u32 vp_start = reg(0x1EA0) & 511;
    u32 vp_len = 0;
    while (vp_start + vp_len < 512 && !(vp_words[(vp_start + vp_len) * 4 + 3] & 1)) vp_len++;
    vp_len++;
    u64 vh = hash_words(&vp_words[vp_start * 4], vp_len * 4);

    u32 fp_ctl = reg(0x08E4);
    u32 fp_ea = location_to_ea(fp_ctl & ~3u, (fp_ctl & 3) == 2);
    u32 shader_control = reg(0x1D60);
    u32 fp_size;
    u64 fh = hash_fp(fp_ea, fp_size) ^ ((u64)(shader_control & 0x4E) << 56);
    // units bound to cube maps (format bit 2) are declared samplerCube: a variant of the program per cube mask
    u32 cube_mask = 0;
    for (u32 u = 0; u < 16; u++)
        if ((reg(0x1A0C + u * 0x20) & 0x80000000) && (reg(0x1A04 + u * 0x20) & 4)) cube_mask |= 1u << u;
    static const bool no_cube = getenv("F1CE_NO_CUBE") != nullptr;   // debug
    if (no_cube) cube_mask = 0;
    if (cube_mask) fh ^= (u64)cube_mask * 0x9E3779B97F4A7C15ull;

    auto fit = g_fp_cache.find(fh);
    if (fit == g_fp_cache.end()) {
        static const char* stop_hash = getenv("F1CE_FP_STOP_HASH");
        g_fp_stop_here = !stop_hash || strtoull(stop_hash, nullptr, 16) == fh;
        g_fp_hash_cur = fh;
        FPInfo info = translate_fp(fp_ea, shader_control, cube_mask);
        LOG_I("rsx: FP GLSL body (ctl %x):\n%s", shader_control, info.glsl.substr(info.glsl.find("void main")).c_str());
        if (getenv("F1CE_DUMP_SHADERS")) {   // debug: full translated source per fragment program
            char name[48];
            snprintf(name, sizeof(name), "fp_%016llx.glsl", (unsigned long long)fh);
            if (FILE* f = fopen(name, "w")) { fputs(info.glsl.c_str(), f); fclose(f); }
        }
        fit = g_fp_cache.emplace(fh, std::make_pair(0u, std::move(info))).first;
    }
    const FPInfo& fpi = fit->second.second;
    fp_consts.resize(std::max<size_t>(fpi.const_offsets.size(), 1) * 4);
    for (size_t i = 0; i < fpi.const_offsets.size(); i++)
        for (int k = 0; k < 4; k++) {
            u32 v = fp_word(fp_ea + fpi.const_offsets[i] + k * 4);
            memcpy(&fp_consts[i * 4 + k], &v, 4);
        }

    auto vit = g_vp_cache.find(vh);
    if (vit == g_vp_cache.end()) {
        VPInfo vpi = translate_vp(vp_start);
        LOG_I("rsx: VP GLSL body:\n%s", vpi.glsl.substr(vpi.glsl.find("void main")).c_str());
        for (u32 i = 0; i < vp_len; i++)
            LOG_I("  vp[%u] %08x %08x %08x %08x", vp_start + i, vp_words[(vp_start + i) * 4], vp_words[(vp_start + i) * 4 + 1],
                  vp_words[(vp_start + i) * 4 + 2], vp_words[(vp_start + i) * 4 + 3]);
        if (getenv("F1CE_DUMP_SHADERS")) {   // debug: full translated source + raw words per vertex program
            char name[48];
            snprintf(name, sizeof(name), "vp_%016llx.glsl", (unsigned long long)vh);
            if (FILE* f = fopen(name, "w")) {
                fputs(vpi.glsl.c_str(), f);
                for (u32 i = 0; i < vp_len; i++)
                    fprintf(f, "// vp[%u] %08x %08x %08x %08x\n", vp_start + i, vp_words[(vp_start + i) * 4],
                            vp_words[(vp_start + i) * 4 + 1], vp_words[(vp_start + i) * 4 + 2], vp_words[(vp_start + i) * 4 + 3]);
                fclose(f);
            }
        }
        vit = g_vp_cache.emplace(vh, VPEntry{0, std::move(vpi.glsl)}).first;
    }

    u64 key = vh * 31 + fh;
    auto pit = g_programs.find(key);
    if (pit != g_programs.end()) return &pit->second;
    db_record(vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, vh, fh);
    Program p;
    p.fh = fh;
    p.tex_used = fpi.tex_used;
    p.prog = glCreateProgram();
    u64 t_link = time_usec();
    // disk cache key: both sources and the renderer string
    static const std::string renderer = (const char*)glGetString(GL_RENDERER) + std::string("|") + (const char*)glGetString(GL_VERSION);
    u64 src_key = 1469598103934665603ull;
    for (const std::string* str : {&renderer, (const std::string*)&vit->second.glsl, (const std::string*)&fit->second.second.glsl})
        for (unsigned char ch : *str) { src_key ^= ch; src_key *= 1099511628211ull; }
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%016llx.bin", shader_cache_dir().c_str(), (unsigned long long)src_key);
    GLint ok = 0;
    GLuint pre = 0;
    {
        std::lock_guard lk(g_pre_m);
        auto it = g_preloaded.find(src_key);
        if (it != g_preloaded.end()) { pre = it->second; g_preloaded.erase(it); }
    }
    if (!g_used_keys.count(src_key)) {   // remembered for the next sessions' preload
        g_used_keys.insert(src_key);
        if (FILE* f = fopen((shader_cache_dir() + "/used.txt").c_str(), "a")) { fprintf(f, "%016llx\n", (unsigned long long)src_key); fclose(f); }
    }
    if (pre) {
        glDeleteProgram(p.prog);
        p.prog = pre;
        ok = 1;
        g_cache_hits++;
        g_preload_hits++;
    } else if (FILE* f = fopen(cache_path, "rb")) {
        u32 format = 0;
        std::vector<u8> bin;
        if (fread(&format, 4, 1, f) == 1) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f) - 4;
            fseek(f, 4, SEEK_SET);
            if (n > 0) { bin.resize((size_t)n); if (fread(bin.data(), 1, bin.size(), f) != bin.size()) bin.clear(); }
        }
        fclose(f);
        if (!bin.empty()) {
            glProgramBinary(p.prog, format, bin.data(), (GLsizei)bin.size());
            glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
        }
        if (ok) g_cache_hits++;
    }
    if (!ok && start_compile_thread()) {
        // not cached: linked on the compile thread (the driver needs ~50 ms per program; doing it here stalled the
        // first frames of a scene for seconds); draws with the program are skipped until it is ready
        auto job = std::make_shared<CompileJob>();
        job->prog = p.prog;
        job->vs = vit->second.glsl;
        job->fs = fit->second.second.glsl;
        job->path = cache_path;
        p.job = job;
        {
            std::lock_guard lk(g_cq_m);
            g_cq.push_back(job);
        }
        g_cq_cv.notify_one();
        g_compiled++;
        LOG_I("rsx: new program (vp %u instr @%u, fp %u bytes, %zu consts) vp_%016llx fp_%016llx: compiling in the background",
              vp_len, vp_start, fp_size, fpi.const_offsets.size(), (unsigned long long)vh, (unsigned long long)fh);
        return &(g_programs[key] = p);
    }
    if (!ok) {   // synchronous fallback: compile (shader objects are shared between programs), link, store the binary
        static u64 t_vs = 0, t_fs = 0, t_ln = 0, n_ln = 0;
        u64 t0 = time_usec();
        if (!vit->second.sh) vit->second.sh = compile(GL_VERTEX_SHADER, vit->second.glsl);
        u64 t1 = time_usec();
        if (!fit->second.first) fit->second.first = compile(GL_FRAGMENT_SHADER, fit->second.second.glsl);
        u64 t2 = time_usec();
        glAttachShader(p.prog, vit->second.sh);
        glAttachShader(p.prog, fit->second.first);
        glProgramParameteri(p.prog, 0x8257 /*GL_PROGRAM_BINARY_RETRIEVABLE_HINT*/, GL_TRUE);
        glLinkProgram(p.prog);
        glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
        t_vs += t1 - t0; t_fs += t2 - t1; t_ln += time_usec() - t2;
        if (++n_ln % 20 == 0) LOG_I("rsx: compile time over %llu programs: vs %.0f ms, fs %.0f ms, link %.0f ms", (unsigned long long)n_ln,
                                    t_vs / 1000.0, t_fs / 1000.0, t_ln / 1000.0);
        if (ok) {
            GLint len = 0;
            glGetProgramiv(p.prog, 0x8741 /*GL_PROGRAM_BINARY_LENGTH*/, &len);
            if (len > 0) {
                std::vector<u8> bin((size_t)len);
                GLenum format = 0;
                glGetProgramBinary(p.prog, len, nullptr, &format, bin.data());
                if (FILE* f = fopen(cache_path, "wb")) { u32 fm = format; fwrite(&fm, 4, 1, f); fwrite(bin.data(), 1, bin.size(), f); fclose(f); }
            }
        }
        g_compiled++;
    }
    g_compile_us += time_usec() - t_link;
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p.prog, sizeof(log), nullptr, log);
        LOG_E("GL link error: %s", log);
    }
    setup_program(p);
    LOG_I("rsx: new program (vp %u instr @%u, fp %u bytes, %zu consts) vp_%016llx fp_%016llx", vp_len, vp_start, fp_size, fpi.const_offsets.size(), (unsigned long long)vh, (unsigned long long)fh);
    return &(g_programs[key] = p);
}

// ------------------------------------------------------------------ textures
// change-detection hash of guest memory: four independent multiply chains (one chain is latency-bound);
// step > 8 samples one 8-byte word per step
u64 hash_range(const u8* p, u32 bytes, u32 step) {
    u64 h0 = 1469598103934665603ull, h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
    constexpr u64 M = 1099511628211ull;
    u32 k = 0;
    if (step == 8) {
        for (; k + 32 <= bytes; k += 32) {
            u64 x0, x1, x2, x3;
            memcpy(&x0, p + k, 8); memcpy(&x1, p + k + 8, 8); memcpy(&x2, p + k + 16, 8); memcpy(&x3, p + k + 24, 8);
            h0 = (h0 ^ x0) * M; h1 = (h1 ^ x1) * M; h2 = (h2 ^ x2) * M; h3 = (h3 ^ x3) * M;
        }
        for (; k + 8 <= bytes; k += 8) { u64 x; memcpy(&x, p + k, 8); h0 = (h0 ^ x) * M; }
        for (; k < bytes; k++) h1 = (h1 ^ p[k]) * M;
    } else {
        for (; k + 8 <= bytes; k += step) { u64 x; memcpy(&x, p + k, 8); h0 = (h0 ^ x) * M; }
    }
    return h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7) ^ bytes;
}

// cache entries keep their full identity: keys are hashes, and a static entry is used without re-hashing its data,
// so a colliding key must not hand out another draw's buffer
struct VtxEntry {
    GLuint buf = 0; u64 hash = 0, hash_sparse = 0; u32 nverts = 0; u64 last_frame = 0, last_change = 0;
    u64 seen_frame = 0;   // without a buffer yet: frame of the first sighting (data streamed until it comes back)
    u32 id_src = 0, id_fmt = 0, id_min = 0, id_freq = 0;
    bool id_mod = false;
};
FORCE_INLINE u64 mix64(u64 x) {   // splitmix64 finalizer
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
std::unordered_map<u64, VtxEntry> g_vtx_cache;   // converted vertex attribute buffers (see draw())
// Block heaps: raw vertex data (vertex pulling) and converted index lists that stay for several frames. Blocks are
// never rewritten: changed data gets a new block and the old one is reused only after the GPU finished the frame
// that freed it.
struct BlockHeap {
    static constexpr u32 ALIGN = 256;
    u32 size = 0;
    GLuint buf = 0, tex = 0, ring_tex = 0;
    u8* ptr = nullptr;                        // persistently mapped (index arena)
    std::map<u32, u32> free_off;              // offset -> size
    std::multimap<u32, u32> free_size;        // size -> offset
    struct Pending { u64 frame; u32 off, size; };
    std::deque<Pending> pending;              // freed blocks waiting for their frame's fence
    std::deque<std::pair<u64, GLsync>> fences;   // end-of-frame fences
    u64 done_frame = 0;                       // the GPU finished every frame up to this one
    u32 used = 0, failed = 0;
};
BlockHeap g_heap;    // vertex heap (video memory)
BlockHeap g_iheap;   // index arena (mapped, see iarena_init)

void heap_insert_free(BlockHeap& h, u32 off, u32 size) {   // with coalescing of neighbours
    auto erase_size = [&h](u32 o, u32 sz) {
        auto r = h.free_size.equal_range(sz);
        for (auto it = r.first; it != r.second; ++it) if (it->second == o) { h.free_size.erase(it); return; }
    };
    auto next = h.free_off.lower_bound(off);
    if (next != h.free_off.end() && next->first == off + size) {
        size += next->second;
        erase_size(next->first, next->second);
        next = h.free_off.erase(next);
    }
    if (next != h.free_off.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            erase_size(prev->first, prev->second);
            off = prev->first;
            size += prev->second;
            h.free_off.erase(prev);
        }
    }
    h.free_off[off] = size;
    h.free_size.emplace(size, off);
}

// a block of `bytes` in the heap (offset), ~0u when full
u32 heap_alloc(BlockHeap& h, u32 bytes) {
    bytes = (bytes + BlockHeap::ALIGN - 1) & ~(BlockHeap::ALIGN - 1);
    auto it = h.free_size.lower_bound(bytes);
    if (it == h.free_size.end()) { h.failed++; return ~0u; }
    u32 off = it->second, size = it->first;
    h.free_size.erase(it);
    h.free_off.erase(off);
    if (size > bytes) heap_insert_free(h, off + bytes, size - bytes);
    h.used += bytes;
    return off;
}

void heap_free(BlockHeap& h, u32 off, u32 bytes) {
    if (off == ~0u) return;
    bytes = (bytes + BlockHeap::ALIGN - 1) & ~(BlockHeap::ALIGN - 1);
    h.pending.push_back({g_frame, off, bytes});
    h.used -= bytes;
}

// at each flip: fence the frame, return the blocks freed by frames the GPU has finished
void heap_end_frame(BlockHeap& h) {
    if (!h.buf) return;
    h.fences.push_back({g_frame, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
    while (!h.fences.empty()) {
        GLenum r = glClientWaitSync(h.fences.front().second, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED && h.fences.size() < 8) break;
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)   // too many frames in flight: wait
            glClientWaitSync(h.fences.front().second, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        h.done_frame = h.fences.front().first;
        glDeleteSync(h.fences.front().second);
        h.fences.pop_front();
    }
    while (!h.pending.empty() && h.pending.front().frame <= h.done_frame) {
        heap_insert_free(h, h.pending.front().off, h.pending.front().size);
        h.pending.pop_front();
    }
}

struct IdxEntry {
    u32 off = ~0u, bytes = 0;   // block in the index arena (~0u: none)
    u64 hash = 0; u32 count = 0, min_v = 0, max_v = 0; u64 last_change = 0, last_frame = 0;
    u32 id_src = 0, id_count = 0, id_flags = 0;
    u64 seen_frame = 0;   // as VtxEntry::seen_frame
    u64 checked_frame = ~0ull;
};
std::unordered_map<u64, IdxEntry> g_idx_cache;   // converted index lists (see draw())
GLuint g_attr_buf[16];   // vertex buffer bound to each attribute binding of the VAO
GLintptr g_attr_off[16];   // ... and its offset

// Converted vertex data per buffer region: keyed by the attribute's base address and format, independent of the
// vertex range a draw uses (the game draws changing sub-ranges of static buffers as the camera moves, which made the
// range-keyed cache convert ~400k vertices per frame). The converted range grows to cover the draws; each draw binds
// it at an offset. The content is checked once per frame (every draw for small regions).
struct RegionEntry {
    GLuint buf = 0;
    u32 base_ea = 0, fmt = 0, first = 0, count = 0;
    u64 hash = 0, checked_frame = ~0ull, last_frame = 0, seen_frame = 0;
    u64 last_change = 0;   // frame the content last changed: recently changing small regions are checked every draw
};
std::unordered_map<u64, RegionEntry> g_regions;
// Index regions: the raw index buffer (byte-swapped, restart index marked) converted over a range that grows to cover
// the draws, which then draw at an offset with base vertex -min_v instead of rebuilding and rebasing their own list
// (sub-ranges of static index buffers change with the camera; ~500k indices were rebuilt per frame)
struct IdxRegion {
    u32 off = ~0u, bytes = 0;   // block in the index arena
    u32 ea = 0, flags = 0, first = 0, count = 0;
    u64 hash = 0, checked_frame = ~0ull, last_frame = 0;
    u32 changes = 0;   // content changes seen: rewritten (ring) buffers go back to per-draw lists
    std::vector<u32> cpu;                                   // converted indices (for min/max of sub-ranges)
    std::unordered_map<u64, std::pair<u32, u32>> minmax;    // (first << 32 | count) -> vertex range
};
std::unordered_map<u64, IdxRegion> g_idx_regions;

// drops vertex/index buffers not drawn for 120 frames (dynamic geometry in ring buffers gets a new key every frame)
void evict_buffers() {
    std::vector<GLuint> dead;
    for (auto it = g_vtx_cache.begin(); it != g_vtx_cache.end();)
        if (g_frame - it->second.last_frame > 120) {
            if (it->second.buf) dead.push_back(it->second.buf);
            it = g_vtx_cache.erase(it);
        } else ++it;
    for (auto it = g_idx_regions.begin(); it != g_idx_regions.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_iheap, it->second.off, it->second.bytes);
            it = g_idx_regions.erase(it);
        } else ++it;
    for (auto it = g_regions.begin(); it != g_regions.end();)
        if (g_frame - it->second.last_frame > 120) {
            if (it->second.buf) dead.push_back(it->second.buf);
            it = g_regions.erase(it);
        } else ++it;
    for (GLuint b : dead)
        for (GLuint& a : g_attr_buf) if (a == b) a = 0;   // deleting unbinds it from the VAO
    for (auto it = g_idx_cache.begin(); it != g_idx_cache.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_iheap, it->second.off, it->second.bytes);
            it = g_idx_cache.erase(it);
        } else ++it;
    if (!dead.empty()) glDeleteBuffers((GLsizei)dead.size(), dead.data());
}

struct TexEntry { GLuint tex = 0; u64 hash = 0; u32 w = 0, h = 0, fmt = 0; u64 checked_frame = ~0ull, last_change = 0; };
std::unordered_map<u64, TexEntry> g_textures;

u32 morton(u32 x, u32 y, u32 log_w, u32 log_h) {
    // RSX swizzle: interleave x/y bits while both have bits left, then the remaining bits of the larger one
    u32 off = 0, shift = 0;
    u32 lw = log_w, lh = log_h;
    while (lw || lh) {
        if (lw) { off |= (x & 1) << shift++; x >>= 1; lw--; }
        if (lh) { off |= (y & 1) << shift++; y >>= 1; lh--; }
    }
    return off;
}

u32 ilog2(u32 v) { u32 r = 0; while ((1u << r) < v) r++; return r; }

// Texture decoding on the GPU (roadmap step 2): the raw level bytes go into a staging buffer texture and a compute
// shader deswizzles and converts them (the formats of the CPU loop below, same results) straight into the texture
// level. The CPU loop read every texel through ld32/ld16 and cost tens of ms for large textures (the crowd's
// 2048x2048 table, video planes every frame): stutter when new textures appeared. F1CE_TEX_CPU=1: the CPU loop.
GLuint g_tex_cs = 0, g_tex_staging = 0, g_tex_staging_tex = 0;
GLint g_tex_cs_lvl = -1, g_tex_cs_lay = -1;
bool tex_gpu() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (getenv("F1CE_TEX_CPU") || !glDispatchCompute || !glBindImageTexture || !glTexBuffer) return false;
    static const char* src =
        "#version 430 core\n"
        "layout(local_size_x = 8, local_size_y = 8) in;\n"
        "layout(rgba8) writeonly uniform image2D u_dst;\n"
        "uniform usamplerBuffer u_src;\n"
        "uniform ivec4 u_lvl;   // x = byte offset of the level, y = width, z = height, w = format\n"
        "uniform ivec4 u_lay;   // x = linear, y = row pitch in bytes, z = bytes per texel, w = log2(w) | log2(h) << 8\n"
        "uint rd8(int a) { return (texelFetch(u_src, a >> 2).r >> uint((a & 3) * 8)) & 255u; }\n"
        "uint rd16(int a) { return (rd8(a) << 8) | rd8(a + 1); }\n"
        "uint rd32(int a) { return (rd16(a) << 16) | rd16(a + 2); }\n"
        "uint morton(uint x, uint y, int lw, int lh) {   // as morton() on the CPU\n"
        " uint off = 0u; int shift = 0;\n"
        " while (lw > 0 || lh > 0) {\n"
        "  if (lw > 0) { off |= (x & 1u) << shift; shift++; x >>= 1; lw--; }\n"
        "  if (lh > 0) { off |= (y & 1u) << shift; shift++; y >>= 1; lh--; }\n"
        " }\n"
        " return off;\n"
        "}\n"
        "void main() {\n"
        " ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
        " if (p.x >= u_lvl.y || p.y >= u_lvl.z) return;\n"
        " int bpp = u_lay.z;\n"
        " int texel = u_lay.x != 0 ? p.y * u_lay.y + p.x * bpp : int(morton(uint(p.x), uint(p.y), u_lay.w & 255, u_lay.w >> 8)) * bpp;\n"
        " int a = u_lvl.x + texel, f = u_lvl.w;\n"
        " uvec4 c;   // r g b a\n"
        " if (f == 0x81) { uint v = rd8(a); c = uvec4(v); }\n"
        " else if (f == 0x83) { uint v = rd16(a); c = uvec4((v >> 8) & 15u, (v >> 4) & 15u, v & 15u, (v >> 12) & 15u) * 17u; }\n"
        " else if (f == 0x82) { uint v = rd16(a); c = uvec4(((v >> 10) & 31u) * 255u / 31u, ((v >> 5) & 31u) * 255u / 31u, (v & 31u) * 255u / 31u, (v >> 15) != 0u ? 255u : 0u); }\n"
        " else if (f == 0x84) { uint v = rd16(a); c = uvec4(((v >> 11) & 31u) * 255u / 31u, ((v >> 5) & 63u) * 255u / 63u, (v & 31u) * 255u / 31u, 255u); }\n"
        " else if (f == 0x8B) { uint v = rd16(a); c = uvec4(v >> 8, v >> 8, v & 255u, v & 255u); }\n"
        " else { uint v = rd32(a); c = uvec4((v >> 16) & 255u, (v >> 8) & 255u, v & 255u, v >> 24); }   // A8R8G8B8 and the rest\n"
        " imageStore(u_dst, p, vec4(c) / 255.0);\n"
        "}\n";
    GLuint sh = compile(0x91B9 /*GL_COMPUTE_SHADER*/, src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(sh);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_W("rsx: texture decode shader: %s", log);
        return false;
    }
    g_tex_cs = prog;
    g_tex_cs_lvl = glGetUniformLocation(prog, "u_lvl");
    g_tex_cs_lay = glGetUniformLocation(prog, "u_lay");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_src"), 22);
    glUniform1i(glGetUniformLocation(prog, "u_dst"), 0);
    glUseProgram(g_cur_prog);
    glGenBuffers(1, &g_tex_staging);
    glGenTextures(1, &g_tex_staging_tex);
    glBindBuffer(GL_TEXTURE_BUFFER, g_tex_staging);
    glBufferData(GL_TEXTURE_BUFFER, 16, nullptr, GL_STREAM_DRAW);
    GLuint keep = g_active_unit;
    active_unit(22);
    glBindTexture(GL_TEXTURE_BUFFER, g_tex_staging_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_tex_staging);
    active_unit(keep);
    state = 1;
    LOG_I("rsx: textures decoded on the GPU");
    return true;
}

GLuint upload_texture(u32 unit, bool* is_surface, int* depth_func) {
    *is_surface = false;
    *depth_func = 0;
    u32 base = 0x1A00 + unit * 0x20;
    u32 offset = reg(base), format = reg(base + 4);
    u32 rect = reg(base + 0x18);
    u32 w = rect >> 16, h = rect & 0xFFFF;
    u32 pitch = reg(0x1840 + unit * 4) & 0xFFFFF;
    u32 fmt = (format >> 8) & 0xFF;
    bool linear = fmt & 0x20;
    u32 base_fmt = fmt & ~0x60u;
    bool main_mem = (format & 3) == 2;
    if (!w || !h || w > 4096 || h > 4096) return 0;
    // render-to-texture: sample a surface directly when the offset matches one. Compressed formats are never
    // render targets: the game reuses old render-target memory for images loaded from disc (the Quick Race
    // driver portrait sits where the menu video was rendered), so DXT textures always come from memory.
    bool compressed = base_fmt == 0x86 || base_fmt == 0x87 || base_fmt == 0x88;
    if (!main_mem && !compressed) {
        // depth buffer sampled as a texture (shadow map, depth formats 0x90-0x93): compared in the shader
        // with the zfunc of the texture address register (bits 28-31: never, less, equal, lequal,
        // greater, notequal, gequal, always). Checked first: a colour surface may share the offset.
        bool depth_format = base_fmt >= 0x90 && base_fmt <= 0x93;
        auto dit = g_depths.find(offset);
        if (depth_format && dit != g_depths.end() && dit->second.tex) {
            *is_surface = true;
            *depth_func = (int)((reg(base + 8) >> 28) & 7) + 1;
            static const char* zf = getenv("F1CE_ZFUNC");   // debug: override the compare function (1-8)
            if (zf) *depth_func = atoi(zf);
            if (resolve_for(dit->second.tex)) bind_target();
            return dit->second.tex;
        }
        auto it = g_surfaces.find(offset);
        // zeta buffer sampled with a colour format (depth-to-colour copies in post-processing): reinterpret
        // the bits in the shader, unless a newer colour surface lives at the same offset
        if (dit != g_depths.end() && dit->second.tex &&
            (it == g_surfaces.end() || !it->second.tex || dit->second.write_seq > it->second.write_seq)) {
            *is_surface = true;
            *depth_func = 9;
            if (resolve_for(dit->second.tex)) bind_target();
            return dit->second.tex;
        }
        if (it != g_surfaces.end() && it->second.tex) {   // unless the CPU has rewritten its memory since
            Surface& sf = it->second;
            if (sf.sig_checked != g_frame) {
                sf.sig_checked = g_frame;
                if (sf.sig_bytes && mem_fingerprint(location_to_ea(offset, false), sf.sig_bytes) != sf.mem_sig) { sf.stale = true; g_surf_epoch++; }
            }
        }
        if (it != g_surfaces.end() && it->second.tex && !it->second.stale) {
            *is_surface = true;
            // the surface is kept as RGBA; a single-channel B8 read returns the blue channel the target stores
            if (base_fmt == 0x81) *depth_func = 12;
            if (resolve_for(it->second.tex)) bind_target();   // antialiased: resolved first
            return it->second.tex;
        }
    }
    u32 ea = location_to_ea(offset, main_mem);
    u32 bpp = 4;
    bool dxt = base_fmt == 0x86 || base_fmt == 0x87 || base_fmt == 0x88;
    switch (base_fmt) {
        case 0x81: bpp = 1; break;
        case 0x82: case 0x83: case 0x84: case 0x8B: case 0x8F: case 0x97: case 0x92: bpp = 2; break;
        default: bpp = 4;
    }
    u32 size = dxt ? ((w + 3) / 4) * ((h + 3) / 4) * (base_fmt == 0x86 ? 8 : 16) : (linear && pitch ? pitch * h : w * h * bpp);
    u64 key = ((u64)ea << 32) ^ ((u64)fmt << 24) ^ ((u64)w << 12) ^ h;
    // the content is checked at most once per frame (a frame binds the same textures thousands of times)
    // large static swizzled/compressed textures (unchanged for 60 frames: art) are re-checked every 8 frames only,
    // staggered per texture; linear ones are always checked (video planes rewritten by the decoder flickered)
    static const bool tex_noskip = getenv("F1CE_TEX_NOSKIP") != nullptr;   // debug: hash every bind
    if (auto it = g_textures.find(key); !tex_noskip && it != g_textures.end() && it->second.tex) {
        TexEntry& e = it->second;
        if (e.checked_frame == g_frame) return e.tex;
        static const bool tex_nostatic = getenv("F1CE_TEX_NOSTATIC") != nullptr;   // debug
        if (!tex_nostatic && !linear && size > 65536 && g_frame - e.last_change > 60 && (g_frame + key) % 8 != 0) { e.checked_frame = g_frame; return e.tex; }
    }
    // content hash (sparse for large textures)
    u32 step = size > 65536 ? 64 : 8;
    u64 hsh = hash_range(gmem(ea), size, step);
    g_prof.tex_hashed += size / step * 8;
    TexEntry& te = g_textures[key];
    te.checked_frame = g_frame;
    if (te.tex && te.hash == hsh) return te.tex;
    te.last_change = g_frame;
    if (!te.tex) {
        u32 nz = 0;
        for (u32 i = 0; i < std::min<u32>(size, 65536); i += 4) nz += ld32(ea + i) != 0;
        LOG_I("rsx: texture unit %u: ea=%08x fmt=%02x %s %ux%u pitch=%u remap=%08x dim=%u%s levels=%u nonzero_words(first 64K)=%u", unit, ea, fmt,
              linear ? "linear" : "swizzled", w, h, pitch, reg(base + 0x10), (format >> 4) & 0xF, (format & 4) ? " CUBE" : "",
              (format >> 16) & 0xF, nz);
    }
    if (!te.tex) glGenTextures(1, &te.tex);
    te.hash = hsh;
    bind_tex2d(te.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // mip chain: the level count is in bits 16-19 of the format register; levels follow level 0 contiguously
    // (DXT: block rows; swizzled: each level swizzled at its own size). Linear pitched textures keep level 0.
    u32 levels = std::max(1u, (format >> 16) & 0xF);
    if (linear && !dxt) levels = 1;
    u32 off = 0;
    if (!dxt && tex_gpu()) {
        u32 total = 0;   // the bytes of all levels (they follow level 0 contiguously)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            total += linear ? (pitch ? pitch * (lh - 1) + lw * bpp : lw * lh * bpp) : lw * lh * bpp;
        }
        u64 tc = time_usec();
        glBindBuffer(GL_TEXTURE_BUFFER, g_tex_staging);
        glBufferData(GL_TEXTURE_BUFFER, (total + 3) & ~3u, gmem(ea), GL_STREAM_DRAW);   // (a new store: no wait)
        glUseProgram(g_tex_cs);
        for (u32 l = 0; l < levels; l++) {
            u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindImageTexture(0, te.tex, (GLint)l, GL_FALSE, 0, 0x88B9 /*GL_WRITE_ONLY*/, GL_RGBA8);
            const GLint lvl[4] = {(GLint)off, (GLint)lw, (GLint)lh, (GLint)base_fmt};
            const GLint lay[4] = {linear ? 1 : 0, (GLint)(pitch ? pitch : lw * bpp), (GLint)bpp, (GLint)(ilog2(lw) | (ilog2(lh) << 8))};
            glUniform4iv(g_tex_cs_lvl, 1, lvl);
            glUniform4iv(g_tex_cs_lay, 1, lay);
            glDispatchCompute((lw + 7) / 8, (lh + 7) / 8, 1);
            off += lw * lh * bpp;
        }
        glMemoryBarrier(0x00000008 /*GL_TEXTURE_FETCH_BARRIER_BIT*/ | 0x00000020 /*GL_SHADER_IMAGE_ACCESS_BARRIER_BIT*/);
        glUseProgram(g_cur_prog);
        g_prof.t_texdec += time_usec() - tc;
        g_prof.tex_decoded += size;
    }
    u64 tcpu = time_usec();
    bool cpu_decode = !dxt && !tex_gpu();
    for (u32 l = 0; l < levels && (dxt || !tex_gpu()); l++) {
        u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        u32 lea = ea + off;
        if (dxt) {
            GLenum gf = base_fmt == 0x86 ? 0x83F1 /*DXT1 RGBA*/ : base_fmt == 0x87 ? 0x83F2 /*DXT3*/ : 0x83F3 /*DXT5*/;
            u32 lsize = ((lw + 3) / 4) * ((lh + 3) / 4) * (base_fmt == 0x86 ? 8 : 16);
            // DXT blocks are little-endian on RSX as on PC: upload as-is
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, gf, (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)lsize, gmem(lea));
            off += lsize;
            continue;
        }
        std::vector<u32> rgba((size_t)lw * lh);
        u32 sw = ilog2(lw), sh = ilog2(lh);
        for (u32 y = 0; y < lh; y++)
            for (u32 x = 0; x < lw; x++) {
                u32 texel = linear ? (y * (pitch ? pitch : lw * bpp) + x * bpp) : morton(x, y, sw, sh) * bpp;
                u32 a = 255, r = 0, g = 0, b = 0;
                switch (base_fmt) {
                    case 0x85: { u32 v = ld32(lea + texel); a = v >> 24; r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255; break; }
                    case 0x81: r = g = b = a = ld8(lea + texel); break;
                    case 0x83: { u32 v = ld16(lea + texel); a = ((v >> 12) & 15) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; break; }
                    case 0x82: { u32 v = ld16(lea + texel); a = (v >> 15) ? 255 : 0; r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31; break; }
                    case 0x84: { u32 v = ld16(lea + texel); r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break; }
                    case 0x8B: { u32 v = ld16(lea + texel); static const bool sw = getenv("F1CE_G8B8_SWAP") != nullptr; if (sw) v = (v >> 8) | ((v & 255) << 8); g = v >> 8; b = v & 255; r = g; a = b; break; }
                    default: { u32 v = ld32(lea + texel); a = v >> 24; r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255; }
                }
                rgba[(size_t)y * lw + x] = r | (g << 8) | (b << 16) | (a << 24);
            }
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        off += lw * lh * bpp;
    }
    if (cpu_decode) { g_prof.t_texdec += time_usec() - tcpu; g_prof.tex_decoded += size; }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    g_filter_of.erase(te.tex);   // the level count may have changed: re-apply the sampling state at the next bind
    g_texparam_ver++;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    te.w = w; te.h = h; te.fmt = fmt;
    return te.tex;
}

// Cube map of texture unit u (format bit 2). The game renders its environment map each frame into six 256x256
// targets (+X, -X, +Y, -Y, +Z, -Z, one face size apart; double-buffered) and samples them as a cube for the car and
// cockpit reflections: the faces are blitted from the surfaces into a GL cube map when they were redrawn (flipped:
// surfaces are stored bottom-up, cube faces top row first like guest memory).
std::set<u32> g_cube_faces;   // surfaces sampled as cube map faces (written without sRGB encoding)
GLuint cube_texture(u32 u) {
    u32 base = 0x1A00 + u * 0x20;
    u32 offset = reg(base), rect = reg(base + 0x18);
    u32 w = rect >> 16, h = rect & 0xFFFF;
    if (!w || w != h || w > 4096) return 0;
    struct CubeEntry { GLuint tex = 0; u32 w = 0, pw = 0; u64 seq[6] = {}, checked_seq = ~0ull; };
    static std::unordered_map<u32, CubeEntry> cubes;
    static GLuint blit_fbo = 0;
    CubeEntry& ce = cubes[offset];
    const u32 pw = w * res_scale();   // faces at the scale of the render targets they come from
    if (!ce.tex || ce.w != w) {
        if (!ce.tex) glGenTextures(1, &ce.tex);
        glBindTexture(GL_TEXTURE_CUBE_MAP, ce.tex);
        for (int f = 0; f < 6; f++)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, (GLsizei)pw, (GLsizei)pw, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glEnable(0x884F /*GL_TEXTURE_CUBE_MAP_SEAMLESS*/);
        ce.w = w;
        std::fill(ce.seq, ce.seq + 6, 0);
        ce.checked_seq = ~0ull;
        LOG_I("rsx: cube map at %08x (%ux%u)", offset, w, w);
    }
    if (ce.checked_seq == g_square_write_seq) return ce.tex;   // no square target written since the last look
    ce.checked_seq = g_square_write_seq;
    u32 face_bytes = w * w * 4;
    for (u32 f = 0; f < 6; f++) g_cube_faces.insert(offset + f * face_bytes);
    bool blitted = false;
    for (u32 f = 0; f < 6; f++) {
        auto it = g_surfaces.find(offset + f * face_bytes);
        if (it == g_surfaces.end() || !it->second.tex || it->second.width != w || it->second.height != w) continue;
        if (it->second.write_seq == ce.seq[f]) continue;
        if (!blitted) {
            if (!blit_fbo) glGenFramebuffers(1, &blit_fbo);
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blit_fbo);
            blitted = true;
        }
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, ce.tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, it->second.fbo);
        srgb_write(false);
        GLint fw = (GLint)(it->second.width * it->second.scale);
        glBlitFramebuffer(0, 0, fw, fw, 0, (GLint)pw, (GLint)pw, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        ce.seq[f] = it->second.write_seq;
    }
    if (blitted) {   // back to the draw's target, viewport and scissor
        g_tgt_valid = false;
        bind_target();
    }
    return ce.tex;
}

float half_to_float(u16 h);

// vertex texture unit u (registers 0x0900 + 0x20 * u): float formats used as data tables by vertex programs
GLuint upload_vertex_texture(u32 u) {
    u32 base = 0x0900 + u * 0x20;
    u32 offset = reg(base), format = reg(base + 4), rect = reg(base + 0x18), pitch = reg(base + 0x10) & 0xFFFFF;
    u32 w = rect >> 16, h = rect & 0xFFFF, fmt = (format >> 8) & 0xFF, base_fmt = fmt & ~0x60u;
    bool linear = fmt & 0x20;
    if (!w || !h || w > 4096 || h > 4096) return 0;
    u32 comps, bytes;   // components per texel, bytes per component
    switch (base_fmt) {
        case 0x9B: comps = 4; bytes = 4; break;   // W32_Z32_Y32_X32_FLOAT
        case 0x9C: comps = 1; bytes = 4; break;   // X32_FLOAT
        case 0x9A: comps = 4; bytes = 2; break;   // W16_Z16_Y16_X16_FLOAT
        case 0x9F: comps = 2; bytes = 2; break;   // Y16_X16_FLOAT
        default: {
            static std::set<u32> warned;
            if (warned.insert(base_fmt).second) LOG_W("rsx: vertex texture format %02x not supported", base_fmt);
            return 0;
        }
    }
    u32 ea = location_to_ea(offset, (format & 3) == 2);
    u32 bpt = comps * bytes, row = linear && pitch ? pitch : w * bpt;
    u64 key = ((u64)ea << 32) ^ ((u64)fmt << 24) ^ ((u64)w << 12) ^ h ^ 0x5A5A000000000000ull;
    if (auto it = g_textures.find(key); it != g_textures.end() && it->second.tex && it->second.checked_frame == g_frame)
        return it->second.tex;
    u64 hsh = 1469598103934665603ull;
    for (u32 i = 0; i + 4 <= row * h; i += 4) { hsh ^= ld32(ea + i); hsh *= 1099511628211ull; }
    TexEntry& te = g_textures[key];
    te.checked_frame = g_frame;
    if (te.tex && te.hash == hsh) return te.tex;
    if (!te.tex) {
        glGenTextures(1, &te.tex);
        LOG_I("rsx: vertex texture %u: ea=%08x fmt=%02x %ux%u pitch=%u", u, ea, fmt, w, h, pitch);
    }
    te.hash = hsh;
    std::vector<float> data((size_t)w * h * 4, 0.0f);
    u32 lw = ilog2(w), lh = ilog2(h);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u32 t = linear ? y * row + x * bpt : morton(x, y, lw, lh) * bpt;
            float* o = &data[((size_t)y * w + x) * 4];
            o[3] = 1.0f;
            for (u32 c = 0; c < comps; c++)
                o[c] = bytes == 4 ? ldf32(ea + t + c * 4) : half_to_float(ld16(ea + t + c * 2));
        }
    static int logged = 0;
    if (logged++ < 6)
        for (u32 k = 0; k < 6; k++)
            LOG_I("   vtex data [%u] = %g %g %g %g", k, data[k * 4], data[k * 4 + 1], data[k * 4 + 2], data[k * 4 + 3]);
    bind_tex2d(te.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    te.w = w; te.h = h; te.fmt = fmt;
    return te.tex;
}

GLint wrap_mode(u32 v) {
    switch (v) {
        case 1: return GL_REPEAT;
        case 2: return GL_MIRRORED_REPEAT;
        case 3: return GL_CLAMP_TO_EDGE;
        case 4: return GL_CLAMP_TO_BORDER;
        default: return GL_CLAMP_TO_EDGE;
    }
}

// ------------------------------------------------------------------ draws
GLuint g_vao = 0, g_vbo[16], g_ibo = 0;

// Stream ring: one persistently mapped buffer for data used by a single frame (vertices and index lists the game
// writes anew every frame: ~140k vertices and ~550k indices). Converting straight into mapped memory and drawing at
// an offset replaces a glBufferData per draw (expensive in the AMD driver). Eight segments, each fenced when left
// and waited for before it is reused.
struct StreamRing {
    static constexpr u32 SEGS = 8, SEG = 8u << 20;
    GLuint buf = 0;
    u8* ptr = nullptr;
    u32 pos = 0;
    GLsync fence[SEGS] = {};
} g_ring;

void ring_init() {
    static const bool off = getenv("F1CE_NO_RING") != nullptr;   // debug
    if (off || !glBufferStorage || !glFenceSync) return;
    const GLbitfield fl = GL_MAP_WRITE_BIT | 0x40 /*GL_MAP_PERSISTENT_BIT*/ | 0x80 /*GL_MAP_COHERENT_BIT*/;
    glGenBuffers(1, &g_ring.buf);
    glBindBuffer(GL_ARRAY_BUFFER, g_ring.buf);
    glBufferStorage(GL_ARRAY_BUFFER, (GLsizeiptr)StreamRing::SEGS * StreamRing::SEG, nullptr, fl);
    g_ring.ptr = (u8*)glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)StreamRing::SEGS * StreamRing::SEG, fl);
    if (!g_ring.ptr) LOG_W("rsx: stream ring unavailable");
    else LOG_I("rsx: stream ring %u MB", StreamRing::SEGS * StreamRing::SEG >> 20);
}

// space for `bytes` (16-byte aligned) in the ring: pointer to write to and its buffer offset, or nullptr
u8* ring_alloc(u32 bytes, GLintptr* off) {
    if (!g_ring.ptr || bytes > StreamRing::SEG) return nullptr;
    u32 pos = (g_ring.pos + 15) & ~15u;
    u32 seg = pos / StreamRing::SEG;
    if (seg >= StreamRing::SEGS || (pos + bytes - 1) / StreamRing::SEG != seg) {   // into the next segment
        u32 cur = std::min(seg, StreamRing::SEGS - 1);
        u32 next = (cur + 1) % StreamRing::SEGS;
        if (g_ring.fence[cur]) glDeleteSync(g_ring.fence[cur]);
        g_ring.fence[cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (g_ring.fence[next]) {   // the GPU must be done with the segment's previous contents
            u64 tw = time_usec();
            glClientWaitSync(g_ring.fence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            g_prof.t_ringwait += time_usec() - tw;
            g_prof.ring_segs++;
            glDeleteSync(g_ring.fence[next]);
            g_ring.fence[next] = nullptr;
        }
        pos = next * StreamRing::SEG;
    }
    g_ring.pos = pos + bytes;
    *off = (GLintptr)pos;
    return g_ring.ptr + pos;
}
// Vertex heap (vertex pulling): raw guest vertex data that stays for several frames, in one buffer in video memory
// (the stream ring is mapped, i.e. usually system memory). Data goes through the ring and a GPU copy: glBufferSubData
// into the heap while draws read it stalled the AMD driver (0.1 fps). Blocks are never rewritten: changed data gets
// a new block and the old one is reused only after the GPU finished the frame that freed it.
bool g_vtx_gpu = false;   // vertex pulling on (F1CE_VTX_CPU=1: CPU conversion into float4 attribute buffers)
void heap_init() {
    static const bool cpu = getenv("F1CE_VTX_CPU") != nullptr;
    if (cpu || !g_ring.ptr || !glTexBuffer) return;
    GLint max_texels = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
    g_heap.size = 256u << 20;
    if ((u64)max_texels * 4 < g_heap.size || (u64)max_texels * 4 < (u64)StreamRing::SEGS * StreamRing::SEG) {
        LOG_W("rsx: buffer textures too small (%d texels): vertex pulling off", max_texels);
        return;
    }
    glGenBuffers(1, &g_heap.buf);
    glBindBuffer(GL_TEXTURE_BUFFER, g_heap.buf);
    glBufferStorage(GL_TEXTURE_BUFFER, g_heap.size, nullptr, 0x0100 /*GL_DYNAMIC_STORAGE_BIT*/);
    glGenTextures(1, &g_heap.tex);
    glGenTextures(1, &g_heap.ring_tex);
    active_unit(20);
    glBindTexture(GL_TEXTURE_BUFFER, g_heap.ring_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_ring.buf);
    active_unit(21);
    glBindTexture(GL_TEXTURE_BUFFER, g_heap.tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_heap.buf);
    active_unit(0);
    heap_insert_free(g_heap, 0, g_heap.size);
    g_vtx_gpu = true;
    LOG_I("rsx: vertex pulling on (heap %u MB)", g_heap.size >> 20);
}

// Index arena: every index list lives in one persistently mapped buffer, bound once as the VAO's element buffer:
// [0, IDX_RING) is a stream ring for lists used by one draw (four fenced segments), the rest a block heap for the
// cached ones (written through the mapping). A buffer per list cost a glBindBuffer per draw and a glBufferData per
// rebuild (~1.2 ms per frame in the race). Indices are small enough to be read from system memory.
constexpr u32 IDX_SIZE = 96u << 20, IDX_RING = 16u << 20, IDX_SEGS = 4, IDX_SEG = IDX_RING / IDX_SEGS;
struct IdxRing { u32 pos = 0; GLsync fence[IDX_SEGS] = {}; } g_iring;
bool g_idx_rebind = false;   // a draw used a buffer of its own: the arena goes back to the element binding after it

void iarena_init() {
    if (!glBufferStorage || !glFenceSync || getenv("F1CE_NO_IDX_ARENA")) return;
    const GLbitfield fl = GL_MAP_WRITE_BIT | 0x40 /*GL_MAP_PERSISTENT_BIT*/ | 0x80 /*GL_MAP_COHERENT_BIT*/;
    glGenBuffers(1, &g_iheap.buf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_iheap.buf);   // the VAO keeps it
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, IDX_SIZE, nullptr, fl);
    g_iheap.ptr = (u8*)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, IDX_SIZE, fl);
    if (!g_iheap.ptr) { LOG_W("rsx: index arena unavailable"); return; }
    g_iheap.size = IDX_SIZE;
    heap_insert_free(g_iheap, IDX_RING, IDX_SIZE - IDX_RING);
    LOG_I("rsx: index arena %u MB", IDX_SIZE >> 20);
}

// space for one draw's list in the arena's ring: pointer and offset, or nullptr
u8* iring_alloc(u32 bytes, GLintptr* off) {
    if (!g_iheap.ptr || bytes > IDX_SEG) return nullptr;
    u32 pos = (g_iring.pos + 15) & ~15u;
    u32 seg = pos / IDX_SEG;
    if (seg >= IDX_SEGS || (pos + bytes - 1) / IDX_SEG != seg) {   // into the next segment
        u32 cur = std::min(seg, IDX_SEGS - 1), next = (cur + 1) % IDX_SEGS;
        if (g_iring.fence[cur]) glDeleteSync(g_iring.fence[cur]);
        g_iring.fence[cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (g_iring.fence[next]) {   // the GPU must be done with the segment's previous lists
            u64 tw = time_usec();
            glClientWaitSync(g_iring.fence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            g_prof.t_ringwait += time_usec() - tw;
            g_prof.ring_segs++;
            glDeleteSync(g_iring.fence[next]);
            g_iring.fence[next] = nullptr;
        }
        pos = next * IDX_SEG;
    }
    g_iring.pos = pos + bytes;
    *off = (GLintptr)pos;
    return g_iheap.ptr + pos;
}

// a list used by this draw only: its offset in the element buffer bound for the draw
GLintptr idx_stream(const u32* data, u32 n) {
    GLintptr off;
    if (u8* p = iring_alloc(n * 4, &off)) { memcpy(p, data, (size_t)n * 4); return off; }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);   // larger than a ring segment (or no arena): its own buffer
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n * 4, data, GL_STREAM_DRAW);
    g_idx_rebind = g_iheap.buf != 0;
    return 0;
}

// a cached list: a new heap block with the data (the old block freed), false when the arena is full
bool idx_store(const u32* data, u32 n, u32& off, u32& bytes) {
    heap_free(g_iheap, off, bytes);
    bytes = n * 4;
    off = g_iheap.ptr ? heap_alloc(g_iheap, bytes) : ~0u;
    if (off == ~0u) return false;
    memcpy(g_iheap.ptr + off, data, bytes);
    return true;
}

u32 g_prim = 0;
struct Range { u32 first, count; };
std::vector<Range> g_array_ranges, g_index_ranges;
std::vector<u32> g_inline;
u64 g_draws = 0;

float half_to_float(u16 h) {
    u32 s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    u32 f;
    if (e == 0) f = m ? 0 : (s << 31);
    else if (e == 31) f = (s << 31) | 0x7F800000 | (m << 13);
    else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    if (e == 0 && m) { float v = std::ldexp((float)m, -24); return s ? -v : v; }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// reads attribute i of vertex v as float4
void fetch_attr(u32 i, u32 v, float* out) {
    u32 fmt = reg(0x1740 + i * 4), off = reg(0x1680 + i * 4);
    u32 type = fmt & 0xF, size = (fmt >> 4) & 0xF, stride = (fmt >> 8) & 0xFF;
    // instancing: frequency (bits 16-31) > 1 fetches element index % freq when the attribute's bit is set in
    // FREQUENCY_DIVIDER_OPERATION (0x1FC0), index / freq otherwise (e.g. crowd billboards: 4 corners per record)
    u32 freq = fmt >> 16;
    if (freq > 1) v = ((reg(0x1FC0) >> i) & 1) ? v % freq : v / freq;
    u32 ea = location_to_ea(off & 0x7FFFFFFF, off >> 31) + v * stride;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 k = 0; k < size && k < 4; k++) {
        switch (type) {
            case 1: out[k] = (s16)ld16(ea + k * 2) / 32767.0f; break;          // S1 (normalized short)
            case 2: out[k] = ldf32(ea + k * 4); break;                           // F
            case 3: out[k] = half_to_float(ld16(ea + k * 2)); break;             // SF
            case 4: out[k] = ld8(ea + k) / 255.0f; break;                        // UB (normalized)
            case 5: out[k] = (float)(s16)ld16(ea + k * 2); break;                // S32K
            case 7: out[k] = (float)ld8(ea + k); break;                          // UB256
            case 6: {                                                            // CMP 11:11:10
                u32 p = ld32(ea);
                s32 x = (s32)(p << 21) >> 21, y = (s32)(p << 10) >> 21, z = (s32)p >> 22;
                out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
                k = size;
                break;
            }
        }
    }
}

// Converts vertices first .. first+count-1 of attribute i to float4 (same results as fetch_attr, with the register
// decoding hoisted out of the loop and one tight loop per format: the per-vertex calls dominated vertex setup when
// the game streams its geometry into new buffers every frame)
void convert_attr(u32 i, u32 first, u32 count, float* out) {
    u32 fmt = reg(0x1740 + i * 4), off = reg(0x1680 + i * 4);
    u32 type = fmt & 0xF, size = std::min((fmt >> 4) & 0xF, 4u), stride = (fmt >> 8) & 0xFF;
    u32 freq = fmt >> 16;
    bool modulo = ((reg(0x1FC0) >> i) & 1) != 0;
    const u8* base = gmem(location_to_ea(off & 0x7FFFFFFF, off >> 31));
    for (u32 n = 0; n < count; n++) {
        u32 v = first + n;
        if (freq > 1) v = modulo ? v % freq : v / freq;
        const u8* q = base + (size_t)v * stride;
        float* o = out + (size_t)n * 4;
        o[0] = o[1] = o[2] = 0.0f;
        o[3] = 1.0f;
        switch (type) {
            case 1: for (u32 k = 0; k < size; k++) { u16 x; memcpy(&x, q + k * 2, 2); o[k] = (s16)__builtin_bswap16(x) / 32767.0f; } break;
            case 2: for (u32 k = 0; k < size; k++) { u32 x; memcpy(&x, q + k * 4, 4); x = __builtin_bswap32(x); memcpy(&o[k], &x, 4); } break;
            case 3: for (u32 k = 0; k < size; k++) { u16 x; memcpy(&x, q + k * 2, 2); o[k] = half_to_float(__builtin_bswap16(x)); } break;
            case 4: for (u32 k = 0; k < size; k++) o[k] = q[k] / 255.0f; break;
            case 5: for (u32 k = 0; k < size; k++) { u16 x; memcpy(&x, q + k * 2, 2); o[k] = (float)(s16)__builtin_bswap16(x); } break;
            case 7: for (u32 k = 0; k < size; k++) o[k] = (float)q[k]; break;
            case 6: {
                u32 x; memcpy(&x, q, 4); x = __builtin_bswap32(x);
                s32 a = (s32)(x << 21) >> 21, b = (s32)(x << 10) >> 21, c = (s32)x >> 22;
                if (size) { o[0] = a / 1023.0f; o[1] = b / 1023.0f; o[2] = c / 511.0f; }
                break;
            }
        }
    }
}

// Raw vertex data per buffer region (vertex pulling): as RegionEntry, but the guest bytes are copied unconverted
// into a heap block. Keyed by base address, stride and element size (the bytes a record's attribute occupies).
struct RawRegion {
    u32 off = ~0u, bytes = 0;   // heap block
    u32 base_ea = 0, stride = 0, elem = 0, first = 0, count = 0;
    u64 hash = 0, checked_frame = ~0ull, last_frame = 0, seen_frame = 0, last_change = 0;
};
std::unordered_map<u64, RawRegion> g_raw_regions;

void evict_raw_regions() {
    for (auto it = g_raw_regions.begin(); it != g_raw_regions.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_heap, it->second.off, it->second.bytes);
            it = g_raw_regions.erase(it);
        } else ++it;
}

// sets u_va for the attributes of this draw (vertices min_v .. max_v); the draw then uses base vertex 0
void setup_attrs_gpu(Program* p, u32 min_v, u32 max_v) {
    u32 input_mask = reg(0x1FF0), nverts = max_v - min_v + 1;
    int va[64] = {};
    struct Memo { u64 key, h; };
    Memo memo[16];
    u32 nmemo = 0;
    auto hash_of = [&](u32 ea, u32 n) {
        u64 memo_key = ((u64)ea << 32) ^ n;
        for (u32 m = 0; m < nmemo; m++) if (memo[m].key == memo_key) return memo[m].h;
        u64 th = time_usec();
        u64 h = hash_range(gmem(ea), n, n > 65536 ? 64 : 8);
        g_prof.t_hash += time_usec() - th;
        g_prof.hashed += n;
        if (nmemo < 16) memo[nmemo++] = {memo_key, h};
        return h;
    };
    // raw bytes into the stream ring: offset of the copy, or -1
    auto stream = [&](u32 ea, u32 n) -> long long {
        GLintptr roff;
        u8* rp = ring_alloc(n, &roff);
        if (!rp) return -1;
        u64 tc = time_usec();
        memcpy(rp, gmem(ea), n);
        g_prof.t_conv += time_usec() - tc;
        return (long long)roff;
    };
    for (u32 i = 0; i < 16; i++) {
        u32 fmt = reg(0x1740 + i * 4);
        u32 type = fmt & 0xF, size = (fmt >> 4) & 0xF;
        if (!size || !(input_mask & (1u << i))) continue;   // disabled: type 0
        u32 stride = (fmt >> 8) & 0xFF, freq = fmt >> 16;
        bool modulo = ((reg(0x1FC0) >> i) & 1) != 0;
        u32 off = reg(0x1680 + i * 4);
        u32 base_ea = location_to_ea(off & 0x7FFFFFFF, off >> 31);
        u32 elem = type == 6 ? 4 : std::min(size, 4u) * (type == 2 ? 4 : type == 4 || type == 7 ? 1 : 2);
        u32 rmin = min_v, rmax = max_v;
        if (freq > 1) {
            if (modulo) { rmin = 0; rmax = std::min(freq, nverts) - 1; } else { rmin = min_v / freq; rmax = max_v / freq; }
        }
        if (!stride) rmin = rmax = 0;
        u32 src = base_ea + rmin * stride, bytes = (rmax - rmin) * stride + elem;
        long long base = -1;   // byte offset of the attribute's record rmin
        bool in_ring = true;
        if (stride && freq <= 1) {
            u64 key = mix64(((u64)base_ea << 32) ^ (stride << 8) ^ elem) ^ 0x7f4a7c15ull;
            RawRegion& re = g_raw_regions[key];
            if (re.base_ea != base_ea || re.stride != stride || re.elem != elem) {
                heap_free(g_heap, re.off, re.bytes);
                re = RawRegion{};
                re.base_ea = base_ea; re.stride = stride; re.elem = elem;
            }
            re.last_frame = g_frame;
            auto region_bytes = [&](u32 count) { return (count - 1) * stride + elem; };
            auto upload = [&](u32 first, u32 count) {   // a new block with records first .. first+count-1
                u32 n = region_bytes(count), ea = base_ea + first * stride;
                heap_free(g_heap, re.off, re.bytes);
                re.off = heap_alloc(g_heap, n);
                re.bytes = n;
                re.first = first; re.count = count;
                if (re.off == ~0u) return;
                u64 tc = time_usec();
                glBindBuffer(GL_COPY_READ_BUFFER, g_ring.buf);
                glBindBuffer(GL_COPY_WRITE_BUFFER, g_heap.buf);
                for (u32 done = 0; done < n;) {   // in pieces of at most one ring segment
                    u32 k = std::min(n - done, StreamRing::SEG);
                    GLintptr roff;
                    u8* rp = ring_alloc(k, &roff);
                    if (!rp) break;
                    memcpy(rp, gmem(ea + done), k);
                    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, roff, (GLintptr)re.off + done, k);
                    done += k;
                }
                g_prof.t_conv += time_usec() - tc;
                g_prof.conv_verts += count;
            };
            bool covers = re.off != ~0u && min_v >= re.first && max_v < re.first + re.count;
            if (covers) {
                // small regions that changed recently (ring buffers rewritten between draws) are checked every draw
                bool small = re.count * stride <= 16384 && g_frame - re.last_change < 60;
                if (small || re.checked_frame != g_frame) {
                    u64 h = hash_of(base_ea + re.first * stride, region_bytes(re.count));
                    if (h != re.hash) { upload(re.first, re.count); re.hash = h; g_prof.conv_changed += re.count; re.last_change = g_frame; }
                    re.checked_frame = g_frame;
                }
            } else if (re.off == ~0u && re.seen_frame && re.seen_frame != g_frame) {
                // seen in an earlier frame: give the region its own block
                re.hash = hash_of(base_ea + min_v * stride, region_bytes(nverts));
                upload(min_v, nverts);
                re.checked_frame = re.last_change = g_frame;
                g_prof.conv_new += nverts;
            } else if (re.off != ~0u) {
                // grow to the union when the ranges are close, else start over at this draw's range
                u32 nf = std::min(re.first, min_v), ne = std::max(re.first + re.count, max_v + 1);
                if (ne - nf > 4 * std::max(re.count, nverts) + 4096) { nf = min_v; ne = max_v + 1; }
                re.hash = hash_of(base_ea + nf * stride, region_bytes(ne - nf));
                upload(nf, ne - nf);
                re.checked_frame = re.last_change = g_frame;
                g_prof.conv_new += re.count;
            } else {
                re.seen_frame = g_frame;   // first sighting (ring-buffer data at a new address every frame): streamed
            }
            if (re.off != ~0u && min_v >= re.first && max_v < re.first + re.count) {
                base = (long long)re.off + (long long)(rmin - re.first) * stride;
                in_ring = false;
            }
        }
        if (base < 0) {
            base = stream(src, bytes);
            g_prof.conv_new += rmax - rmin + 1;
            if (base < 0) { LOG_W("rsx: vertex data of %u bytes does not fit the stream ring", bytes); continue; }
        }
        va[i * 4] = (int)(base - (long long)rmin * stride);   // record 0 (may lie before the buffer start)
        va[i * 4 + 1] = (int)stride;
        va[i * 4 + 2] = (int)(type | (size << 4) | (modulo ? 256 : 0) | (in_ring ? 512 : 0));
        va[i * 4 + 3] = (int)freq;
    }
    if (p->loc_va >= 0 && (g_no_state_cache || memcmp(p->u_va, va, sizeof(va)) != 0)) {
        u64 tb = time_usec();
        memcpy(p->u_va, va, sizeof(va));
        glUniform4iv(p->loc_va, 16, va);
        g_prof.t_bind += time_usec() - tb;
        g_prof.binds++;
    }
}

void apply_state() {
    static u32 last[13];
    const u32 cur[13] = {reg(0x0310), reg(0x0314), reg(0x0318), reg(0x0320), reg(0x031C), reg(0x0A74), reg(0x0A6C),
                         reg(0x0A70), reg(0x0324), reg(0x183C), reg(0x1830), reg(0x1834), reg(0x1D88) & 0x1000};
    if (g_state_valid && memcmp(cur, last, sizeof(cur)) == 0) return;
    memcpy(last, cur, sizeof(cur));
    g_state_valid = true;
    // blending
    if (reg(0x0310) & 1) {
        glEnable(GL_BLEND);
        u32 sf = reg(0x0314), df = reg(0x0318), eq = reg(0x0320);
        glBlendFuncSeparate(sf & 0xFFFF, df & 0xFFFF, sf >> 16, df >> 16);
        glBlendEquationSeparate(eq & 0xFFFF ? eq & 0xFFFF : GL_FUNC_ADD, eq >> 16 ? eq >> 16 : GL_FUNC_ADD);
        u32 bc = reg(0x031C);
        glBlendColor(((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f);
    } else {
        glDisable(GL_BLEND);
    }
    // depth
    if (reg(0x0A74) & 1) {
        glEnable(GL_DEPTH_TEST);
        u32 f = reg(0x0A6C);
        glDepthFunc(f ? f : GL_LESS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(reg(0x0A70) & 1 ? GL_TRUE : GL_FALSE);
    // colour mask: A R G B in bytes 3..0 (non-zero byte = enabled)
    u32 cm = reg(0x0324);
    glColorMask((cm >> 16) & 0xFF ? GL_TRUE : GL_FALSE, (cm >> 8) & 0xFF ? GL_TRUE : GL_FALSE, cm & 0xFF ? GL_TRUE : GL_FALSE,
                (cm >> 24) & 0xFF ? GL_TRUE : GL_FALSE);
    // culling: with a top-left window origin the clip->GL mapping flips Y, which reverses the winding;
    // with a bottom-left origin (SHADER_WINDOW bit 12) the two flips cancel out
    if (reg(0x183C) & 1) {
        glEnable(GL_CULL_FACE);
        u32 cf = reg(0x1830);
        glCullFace(cf ? cf : GL_BACK);
        bool flipped = !((reg(0x1D88) >> 12) & 1);
        // shadow maps cull front faces as requested, like the cube map faces (which are right as is). Taking their
        // winding the other way round (an old experiment against dark smears around the wheels) dropped the shadows
        // of thin structures such as the start gantry.
        static const bool shadow_flip = getenv("F1CE_SHADOW_CULL_FLIP") != nullptr;   // debug: the old experiment
        if (shadow_flip && reg(0x0324) == 0 && cf == GL_FRONT) flipped = !flipped;
        GLenum front = reg(0x1834) == GL_CW ? GL_CW : GL_CCW;
        glFrontFace(flipped ? (front == GL_CW ? GL_CCW : GL_CW) : front);
    } else {
        glDisable(GL_CULL_FACE);
    }
}

void draw(bool indexed) {
    u64 t0 = time_usec();
    if (g_no_state_cache) { g_tgt_valid = g_state_valid = false; g_cur_prog = 0; }
    Surface* target = bind_target();
    if (!target) return;
    std::vector<float> fp_consts;
    Program* p = get_program(fp_consts);
    if (p->job) {
        if (!p->job->done) return;   // still compiling
        if (g_bin_loads_frame != g_frame) { g_bin_loads_frame = g_frame; g_bin_loads = 0; }
        if (g_bin_loads >= 2) return;   // ~10 ms per binary load: at most two per frame
        g_bin_loads++;
        GLint ok = 0;
        if (!p->job->bin.empty()) {
            glProgramBinary(p->prog, p->job->format, p->job->bin.data(), (GLsizei)p->job->bin.size());
            glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
        }
        if (!ok) LOG_E("GL link error: %s", p->job->log.c_str());
        p->job.reset();
        setup_program(*p);
    }
    if (g_cur_prog != p->prog) { glUseProgram(p->prog); g_cur_prog = p->prog; }
    {   // debug: F1CE_FC_LOG=<fp offset hex> logs that program's first constants every 30 frames
        static const u32 fc_fp = getenv("F1CE_FC_LOG") ? (u32)strtoul(getenv("F1CE_FC_LOG"), nullptr, 16) : ~0u;
        static u64 last = ~0ull;
        if (reg(0x08E4) == fc_fp && g_frame % 30 == 0 && last != g_frame && fp_consts.size() >= 4) {
            last = g_frame;
            LOG_I("fc log frame %llu: FC0 = %g %g %g %g", (unsigned long long)g_frame, fp_consts[0], fp_consts[1], fp_consts[2], fp_consts[3]);
        }
    }
    u64 t1 = time_usec();
    g_prof.program += t1 - t0;
    bool first_of_prog;
    {
        static std::unordered_map<GLuint, int> seen;
        first_of_prog = seen[p->prog] == 0;
        if (seen[p->prog] == 0) {
            std::string fc;
            for (size_t i = 0; i < fp_consts.size(); i++) { char b[24]; snprintf(b, sizeof(b), "%s%g", i % 4 ? "," : " | ", fp_consts[i]); fc += b; }
            LOG_I("rsx draw: prog %u fp @%08x hash %016llx consts:%s", p->prog, reg(0x08E4), (unsigned long long)p->fh, fc.c_str());
        }
        if (seen[p->prog]++ < 3)
            LOG_I("rsx draw: prog %u -> surface %08x (%ux%u) blend=%x sf=%08x df=%08x cmask=%08x alpha_test=%x fc0=%g,%g,%g,%g", p->prog,
                  target->offset, target->width, target->height, reg(0x0310), reg(0x0314), reg(0x0318), reg(0x0324),
                  reg(0x0304), fp_consts.size() > 0 ? fp_consts[0] : 0.f, fp_consts.size() > 1 ? fp_consts[1] : 0.f,
                  fp_consts.size() > 2 ? fp_consts[2] : 0.f, fp_consts.size() > 3 ? fp_consts[3] : 0.f);
    }
    if (first_of_prog)
        for (u32 u = 0; u < 4; u++)   // vertex texture units (0x0900 + 0x20 * u)
            if (reg(0x090C + u * 0x20) & 0x80000000)
                LOG_I("   vtex %u: offset=%08x format=%08x addr=%08x ctl0=%08x ctl3=%08x filter=%08x rect=%08x", u,
                      reg(0x0900 + u * 0x20), reg(0x0904 + u * 0x20), reg(0x0908 + u * 0x20), reg(0x090C + u * 0x20),
                      reg(0x0910 + u * 0x20), reg(0x0914 + u * 0x20), reg(0x0918 + u * 0x20));
    if (first_of_prog)
        for (size_t i = 0; i + 3 < fp_consts.size(); i += 4)
            LOG_I("   FC[%zu] = %g %g %g %g", i / 4, fp_consts[i], fp_consts[i + 1], fp_consts[i + 2], fp_consts[i + 3]);
    if ((reg(0x08E4) & ~3u) == g_watch_fp) {
        static bool offs_logged = false;
        if (!offs_logged) {
            offs_logged = true;
            for (u32 k = 0; k < 0x600; k += 16)   // raw program words (halfword-swapped as the decoder sees them)
                LOG_I("watch fp: +%03x %08x %08x %08x %08x", k, fp_word(mem::RSX_LOCAL + g_watch_fp + k),
                      fp_word(mem::RSX_LOCAL + g_watch_fp + k + 4), fp_word(mem::RSX_LOCAL + g_watch_fp + k + 8),
                      fp_word(mem::RSX_LOCAL + g_watch_fp + k + 12));
            u32 sz;
            u64 fh = hash_fp(mem::RSX_LOCAL + g_watch_fp, sz) ^ ((u64)(reg(0x1D60) & 0x4E) << 56);
            auto fit = g_fp_cache.find(fh);
            LOG_I("watch fp: program size %x hash %016llx", sz, (unsigned long long)fh);
            if (fit != g_fp_cache.end()) {
                std::string o;
                for (u32 k = 0; k < fit->second.second.const_offsets.size(); k++) { char b[16]; snprintf(b, sizeof(b), " %u:%03x", k, fit->second.second.const_offsets[k]); o += b; }
                LOG_I("watch fp: constant offsets%s", o.c_str());
            }
        }
        static int wl = 0;
        if (wl++ < 40 && fp_consts.size() >= 60)
            LOG_I("watch fp: draw frame %llu FC11=%g,%g,%g FC12=%g,%g,%g FC13=%g FC14=%g,%g,%g", (unsigned long long)g_frame,
                  fp_consts[44], fp_consts[45], fp_consts[46], fp_consts[48], fp_consts[49], fp_consts[50], fp_consts[52],
                  fp_consts[56], fp_consts[57], fp_consts[58]);
    }
    if (p->loc_c >= 0 && p->c_ver != vp_ver) {
        u32 lo = 468, hi = 0;
        for (u32 k = 0; k < 468; k++)
            if (vp_slot_ver[k] > p->c_ver) { lo = std::min(lo, k); hi = k; }
        if (lo <= hi) glUniform4fv(p->loc_c + (GLint)lo, (GLsizei)(hi - lo + 1), vp_consts + lo * 4);
        p->c_ver = vp_ver;
    }
    if (p->loc_fc >= 0 && (g_no_state_cache || p->u_fc.size() != fp_consts.size() ||
                           memcmp(p->u_fc.data(), fp_consts.data(), fp_consts.size() * 4) != 0)) {
        glUniform4fv(p->loc_fc, (GLsizei)(fp_consts.size() / 4), fp_consts.data());
        p->u_fc = fp_consts;
    }
    {
        const float scale[4] = {regf(0x0A20), regf(0x0A24), regf(0x0A28), regf(0x0A2C)};
        const float offset[4] = {regf(0x0A30), regf(0x0A34), regf(0x0A38), regf(0x0A3C)};
        const float surface[4] = {(float)target->width, (float)target->height, (reg(0x1D88) >> 12) & 1 ? 1.0f : 0.0f, 0};
        const float alpha[4] = {(reg(0x0304) & 1) ? 1.0f : 0.0f, (float)((s32)reg(0x0308) - 0x200), (reg(0x030C) & 255) / 255.0f,
                                  0.0f};
        // FOG_MODE: LINEAR 0x2601, EXP 0x0800, EXP2 0x0801, EXP_ABS 0x0802, EXP2_ABS 0x0803, LINEAR_ABS 0x0804
        u32 fm = reg(0x08CC);
        float mode = fm == 0x0800 ? 1.f : fm == 0x0801 ? 2.f : fm == 0x0802 ? 4.f : fm == 0x0803 ? 5.f : fm == 0x0804 ? 3.f : 0.f;
        const float fog[4] = {mode, regf(0x08D0), regf(0x08D4), 0};
        uniform_cached(p->loc_scale, p->u_scale, scale, glUniform4fv, 1);
        uniform_cached(p->loc_offset, p->u_offset, offset, glUniform4fv, 1);
        uniform_cached(p->loc_surface, p->u_surface, surface, glUniform4fv, 1);
        uniform_cached(p->loc_alpha, p->u_alpha, alpha, glUniform4fv, 1);
        uniform_cached(p->loc_fog, p->u_fog, fog, glUniform4fv, 1);
        if (p->loc_res_scale >= 0 && p->u_res != (float)target->scale) { p->u_res = (float)target->scale; glUniform1fv(p->loc_res_scale, 1, &p->u_res); }
        float ui = target->scale > 1 && !(reg(0x0A74) & 1) ? 1.0f : 0.0f;   // (see sample_unit: no depth test)
        if (p->loc_ui_clamp >= 0 && p->u_ui != ui) { p->u_ui = ui; glUniform1fv(p->loc_ui_clamp, 1, &p->u_ui); }
    }
    u64 ta0 = time_usec();
    g_prof.t_pre += ta0 - t1;
    apply_state();
    g_prof.t_apply += time_usec() - ta0;

    // textures
    float flips[16] = {}, depth_funcs[16] = {}, tex_scale[32], remaps[16];
    bool reads_gamma = false;   // the program samples a texture decoded from sRGB
    bool reads_raw_target = false;   // ... or a colour render target without decode (a copy of raw colour)
    std::fill(remaps, remaps + 16, (float)0xaae4);
    std::fill(tex_scale, tex_scale + 32, 1.0f);
    u64 tu0 = time_usec();   // (the whole unit loop: a clock read per unit cost ~1 ms per frame)
    for (u32 u = 0; u < 16; u++) {
        if (!(reg(0x1A0C + u * 0x20) & 0x80000000)) continue;   // texture control0 enable
        // the unit is selected only before GL calls on it (uploads and binds act on the active unit): with the memo
        // below, a unit whose texture is already bound costs no GL call
        static const bool no_cube = getenv("F1CE_NO_CUBE") != nullptr;   // debug: sample cube maps as their +X face
        // TEXTURE_ADDRESS bits 20-23: gamma (sRGB decode per channel R, G, B, A); bits 12-15: unsigned remap
        // (1 = BIASED: x * 2 - 1, the game's normal maps). Without them normal-mapped surfaces were lit with wrong
        // normals (tyres, car body) and colour textures were too bright in the shaders' linear maths.
        u32 conv = 0;
        {
            u32 a = reg(0x1A08 + u * 0x20);
            static const bool no_gamma = getenv("F1CE_NO_TEXGAMMA") != nullptr;   // debug
            static const bool no_bias = getenv("F1CE_NO_TEXBIAS") != nullptr;     // debug
            if (!no_gamma) conv |= (a >> 20) & 0xF;
            // only a decoded colour channel makes the output sRGB: the cars' shadow decals decode alpha only (gamma
            // 8) and subtract from the scene; encoding their colour (0.2 -> 0.48) made the shadows black
            static const bool alpha_gamma_encodes = getenv("F1CE_ALPHA_GAMMA_ENCODE") != nullptr;   // debug: old rule
            if ((conv & (alpha_gamma_encodes ? 0xF : 0x7)) && (p->tex_used >> u & 1)) reads_gamma = true;
            if (!no_bias && ((a >> 12) & 0xF) == 1) conv |= 16;
        }
        if ((reg(0x1A04 + u * 0x20) & 4) && !no_cube) {   // cube map: the program declares a samplerCube here
            static GLuint bound_cube[16];
            active_unit(u);
            GLuint ct = cube_texture(u);
            if (bound_cube[u] != ct) { glBindTexture(GL_TEXTURE_CUBE_MAP, ct); bound_cube[u] = ct; }
            remaps[u] = (float)((reg(0x1A10 + u * 0x20) & 0xFFFF) | (conv << 16));
            continue;
        }
        bool from_surface;
        int depth_func;
        // per-unit memo: a frame binds the same textures with the same registers thousands of times, and each
        // lookup went through the surface, depth and texture maps (and the wrap / filter maps below)
        struct UnitMemo { u32 r[7]; u64 frame = ~0ull; u32 epoch = 0, pver = ~0u; GLuint t = 0; bool surf = false; int dfunc = 0; };
        static UnitMemo memo[16];
        static const bool no_memo = getenv("F1CE_TEX_NOMEMO") != nullptr;   // debug
        UnitMemo& um = memo[u];
        const u32 ur[7] = {reg(0x1A00 + u * 0x20), reg(0x1A04 + u * 0x20), reg(0x1A08 + u * 0x20), reg(0x1A0C + u * 0x20),
                           reg(0x1A14 + u * 0x20), reg(0x1A18 + u * 0x20), reg(0x1840 + u * 4)};
        bool memo_hit = !no_memo && um.frame == g_frame && um.epoch == g_surf_epoch && memcmp(um.r, ur, sizeof(ur)) == 0;
        GLuint t;
        if (memo_hit) g_prof.memo_hits++; else g_prof.memo_miss++;
        if (memo_hit) {
            t = um.t; from_surface = um.surf; depth_func = um.dfunc;
            if (from_surface && resolve_for(t)) bind_target();   // antialiased surfaces: resolved when drawn since
        } else {
            active_unit(u);
            t = upload_texture(u, &from_surface, &depth_func);
            // not kept when a depth and a colour surface share the offset (the newer one is chosen per draw)
            bool ambiguous = depth_func == 9 || (from_surface && (depth_func == 0 || depth_func == 12) && g_depths.count(ur[0]));
            memcpy(um.r, ur, sizeof(ur));
            um.frame = ambiguous ? ~0ull : g_frame;
            um.epoch = g_surf_epoch;
            um.pver = ~0u;
            um.t = t; um.surf = from_surface; um.dfunc = depth_func;
        }
        flips[u] = from_surface ? 1.0f : 0.0f;
        // a raw read of a colour target at least as large as the destination is a copy (motion blur, the video's
        // copy to the display buffer, the heat haze that refracts the scene into a 640x360 buffer: encoding it
        // washed the haze out into white clouds); a smaller one feeds a material (the mirror glass shades the
        // 256x128 rear view with fog, exposure and tone mapping, and its result is encoded like any other material)
        if (from_surface && depth_func == 0 && !(conv & 0xF) && (p->tex_used >> u & 1) &&
            (reg(0x1A18 + u * 0x20) >> 16) >= target->width && (reg(0x1A18 + u * 0x20) & 0xFFFF) >= target->height)
            reads_raw_target = true;
        if (reg(0x1A04 + u * 0x20) & 0x4000) {   // CELL_GCM_TEXTURE_UN: coordinates in texels
            u32 rect = reg(0x1A18 + u * 0x20);
            tex_scale[u * 2] = 1.0f / (float)std::max(1u, rect >> 16);
            tex_scale[u * 2 + 1] = 1.0f / (float)std::max(1u, rect & 0xFFFF);
            static const bool un_flip = getenv("F1CE_UN_FLIP") != nullptr;   // debug experiment
            if (un_flip) flips[u] = 1.0f - flips[u];
        }
        depth_funcs[u] = (float)depth_func;
        static const bool no_pcf = getenv("F1CE_NO_PCF") != nullptr;   // debug
        if (depth_func >= 1 && depth_func <= 8 && !no_pcf && ((reg(0x1A14 + u * 0x20) >> 24) & 0xF) != 1)
            depth_funcs[u] = (float)(depth_func + 16);   // TEXTURE_FILTER mag 2 = linear: percentage-closer filtering
        {   // TEXTURE_CONTROL1 channel remap, for every format (as on the RSX): the uploads store each format in its
            // native A/R/G/B order. It was applied to A8R8G8B8 and B8 only, and the crowd's G8B8 data table (remap
            // 0xaaab: colour = G, alpha = B) then fed the wrong byte to the shader: figures without clothes.
            u32 bf = (reg(0x1A04 + u * 0x20) >> 8) & 0x9F;
            (void)bf;
            // Depth read as colour too: the decode gives the A8R8G8B8 view of D24S8 (A = depth high byte, R = mid,
            // G = low) and the game's 0xaa93 remap moves it to R/G/B, from which the motion-blur pass rebuilds depth;
            // without the remap it used the low bytes and smeared the far scenery as if it were close
            static const bool no_remap = getenv("F1CE_NO_REMAP") != nullptr;   // debug
            if (no_remap) {}
            else if (depth_func == 0 || depth_func == 12 || depth_func == 9)
                remaps[u] = (float)(reg(0x1A10 + u * 0x20) & 0xFFFF);
            if (depth_func == 0 || depth_func == 12) remaps[u] = (float)((u32)remaps[u] | (conv << 16));
        }
        if (first_of_prog)
            LOG_I("   unit %u: offset=%08x fmt=%02x %ux%u %s zfunc=%d addr=%08x ctl0=%08x filter=%08x remap=%08x", u, reg(0x1A00 + u * 0x20),
                  (reg(0x1A04 + u * 0x20) >> 8) & 0xFF, reg(0x1A18 + u * 0x20) >> 16, reg(0x1A18 + u * 0x20) & 0xFFFF,
                  depth_func ? "DEPTH" : from_surface ? "surface" : "memory", depth_func, reg(0x1A08 + u * 0x20),
                  reg(0x1A0C + u * 0x20), reg(0x1A14 + u * 0x20), reg(0x1A10 + u * 0x20));
        // redundant state is skipped: the unit's binding and each texture's wrap modes are remembered
        if (g_bound_tex[u] != t) { active_unit(u); bind_tex2d(t); }
        {   // debug: F1CE_DUMP_TEX_PROG=<prog> saves the textures of that program's draws in the traced frame
            static const int dump_prog = getenv("F1CE_DUMP_TEX_PROG") ? atoi(getenv("F1CE_DUMP_TEX_PROG")) : -1;
            static const long long tf = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
            if ((int)p->prog == dump_prog && trace_on(tf)) {
                active_unit(u);
                GLint tw = 0, th = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
                if (tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                    std::vector<u32> px((size_t)tw * th);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
                    char name[96];
                    snprintf(name, sizeof(name), "frame_t%u_%08x_%05llu_%dx%d.argb", u, reg(0x1A00 + u * 0x20),
                             (unsigned long long)g_frame, tw, th);
                    if (FILE* f = fopen(name, "wb")) { fwrite(px.data(), 4, px.size(), f); fclose(f); }
                }
            }
        }
        if (memo_hit && um.pver == g_texparam_ver) continue;   // sampling state as at the memo's last bind
        active_unit(u);   // (glTexParameter below acts on the unit's texture)
        u32 addr = reg(0x1A08 + u * 0x20) & 0xF0F;
        auto wit = g_wrap_of.find(t);
        if (wit == g_wrap_of.end() || wit->second != addr) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(addr & 0xF));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((addr >> 8) & 0xF));
            g_wrap_of[t] = addr;
            g_texparam_ver++;
        }
        // sampling state from TEXTURE_FILTER (bits 0-12 LOD bias, signed 1/256 steps; 16-19 min filter; 24-27 mag
        // filter) and CONTROL0 (bits 4-6 max anisotropy, 7-18 max LOD, 19-30 min LOD, 1/256 steps). The game uses a
        // -1.5 bias: without it the car liveries and tyres sampled mip levels that are too small.
        u64 fs = ((u64)reg(0x1A14 + u * 0x20) << 32) | (reg(0x1A0C + u * 0x20) & 0x7FFFFFF0);
        auto fit = g_filter_of.find(t);
        static const bool no_texfilter = getenv("F1CE_NO_TEXFILTER") != nullptr;   // debug
        if (!no_texfilter && !from_surface && (fit == g_filter_of.end() || fit->second != fs)) {
            u32 filt = (u32)(fs >> 32), ctl0 = (u32)fs;
            s32 bias = (s32)(filt << 19) >> 19;
            static const int tf_mask = getenv("F1CE_TF_MASK") ? atoi(getenv("F1CE_TF_MASK")) : 15;   // debug bisect
            if (tf_mask & 1) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias / 256.0f);
            if (tf_mask & 2) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, ((ctl0 >> 19) & 0xFFF) / 256.0f);
            if (tf_mask & 2) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, ((ctl0 >> 7) & 0xFFF) / 256.0f);
            static const float aniso_levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
            if (tf_mask & 4) glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY*/, aniso_levels[(ctl0 >> 4) & 7]);
            static const GLenum min_modes[8] = {GL_LINEAR_MIPMAP_LINEAR, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                                GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR,
                                                GL_LINEAR_MIPMAP_LINEAR};
            GLint levels = 0;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &levels);
            GLenum minf = min_modes[(filt >> 16) & 7];
            // textures addressed in texels (format bit 0x40: the crowd's G8B8 data table) keep linear filtering:
            // nearest sampling at integer texel coordinates picked neighbouring texels and the crowd disappeared
            bool texel_coords = (reg(0x1A04 + u * 0x20) & 0x4000) != 0;
            static const bool texel_nearest = getenv("F1CE_TEXEL_NEAREST") != nullptr;   // debug: keep the game's filter
            if (texel_coords && !texel_nearest) minf = GL_LINEAR, filt = (filt & ~0x0F000000u) | 0x02000000u;
            if (levels == 0) minf = (minf == GL_NEAREST || minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
            if (tf_mask & 8) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
            if (tf_mask & 8) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ((filt >> 24) & 0xF) == 1 ? GL_NEAREST : GL_LINEAR);
            g_filter_of[t] = fs;
            g_texparam_ver++;
        }
        um.pver = g_texparam_ver;
    }
    g_prof.t_upload += time_usec() - tu0;
    static const bool skip_vtex = getenv("F1CE_SKIP_VTEX") != nullptr;   // debug: drop draws that use vertex textures
    if (skip_vtex && p->uses_vtex) return;
    static const std::string skip_progs = getenv("F1CE_SKIP_PROG") ? "," + std::string(getenv("F1CE_SKIP_PROG")) + "," : "";
    if (!skip_progs.empty() && skip_progs.find("," + std::to_string(p->prog) + ",") != std::string::npos) return;   // debug
    for (u32 u = 0; u < 4; u++) {   // vertex textures (TXL in vertex programs) on GL units 16..19
        if (!(reg(0x090C + u * 0x20) & 0x80000000)) continue;
        active_unit(16 + u);
        bind_tex2d(upload_vertex_texture(u));
    }
    active_unit(0);
    uniform_cached(p->loc_flip, p->u_flip, flips, glUniform1fv, 16);
    uniform_cached(p->loc_tex_scale, p->u_tex_scale, tex_scale, glUniform2fv, 16);
    uniform_cached(p->loc_depth, p->u_depth, depth_funcs, glUniform1fv, 16);
    uniform_cached(p->loc_remap, p->u_remap, remaps, glUniform1fv, 16);

    u64 t2 = time_usec();
    g_prof.textures += t2 - t1;
    // gather indices: the converted list (restart index marked, quads turned into triangles, rebased to the lowest
    // referenced vertex) is cached per contiguous source range and checked with a content hash, like vertex data
    glBindVertexArray(g_vao);
    constexpr u32 RESTART = 0xFFFFFFFFu;
    static const GLenum modes[11] = {0, GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES,
                                     GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES, GL_TRIANGLES, GL_TRIANGLE_FAN};
    if (g_prim < 1 || g_prim > 10) return;
    GLenum mode = modes[g_prim];
    u32 min_v = ~0u, max_v = 0, idx_count = 0;
    GLintptr idx_offset = 0;   // index lists stay absolute: every draw uses base vertex -min_v
    u64 ti0 = time_usec();
    {
        auto& ranges = indexed ? g_index_ranges : g_array_ranges;
        u32 first = ranges[0].first, count = 0;   // the game splits long draws into 256-element commands
        bool contiguous = true;
        for (auto& r : ranges) { contiguous &= r.first == first + count; count += r.count; }
        u32 ifmt = reg(0x1820);
        bool u16idx = ((ifmt >> 4) & 0xF) == 1;
        bool restart = indexed && (reg(0x1DAC) & 1);
        u32 rv = reg(0x1DB0);
        u32 ea = indexed ? location_to_ea(reg(0x181C), (ifmt & 0xF) != 0) : 0;
        u32 isz = u16idx ? 2 : 4;
        IdxEntry* ie = nullptr;
        bool need = true;
        u64 h = 0;
        static const bool idx_nocache = getenv("F1CE_IDX_NOCACHE") != nullptr;   // debug
        static const bool no_idx_region = getenv("F1CE_IDX_NOREGION") != nullptr;   // debug
        bool region = false;
        // regions are per 16K-index chunk of the buffer, so draws far apart in one buffer don't fight over a region
        u32 chunk = first >> 14;
        if (indexed && contiguous && g_prim != 8 && g_prim != 9 && !idx_nocache && !no_idx_region && count &&
            ((first + count - 1) >> 14) == chunk) {
            u32 flags = (u16idx ? 1u : 0u) | (restart ? 2u | (rv << 8) : 0u) | (chunk << 24);
            IdxRegion& ir = g_idx_regions[mix64(((u64)ea << 32) ^ flags) ^ 0x2545F491ull];
            if (ir.ea != ea || ir.flags != flags) { heap_free(g_iheap, ir.off, ir.bytes); ir = IdxRegion{}; ir.ea = ea; ir.flags = flags; }
            ir.last_frame = g_frame;
            if (ir.changes >= 3) goto idx_per_draw;
            auto build = [&](u32 f, u32 n) {   // convert raw indices f .. f+n-1 into the region
                ir.first = f; ir.count = n;
                ir.cpu.resize(n);
                const u8* q = gmem(ea + f * isz);
                for (u32 k = 0; k < n; k++) {
                    u32 v;
                    if (u16idx) { u16 x; memcpy(&x, q + k * 2, 2); v = __builtin_bswap16(x); }
                    else { u32 x; memcpy(&x, q + k * 4, 4); v = __builtin_bswap32(x); }
                    if (restart && (v == rv || (u16idx && v == 0xFFFF && rv >= 0xFFFF))) v = RESTART;
                    ir.cpu[k] = v;
                }
                idx_store(ir.cpu.data(), n, ir.off, ir.bytes);   // (full arena: off stays ~0u, the draws stream)
                ir.hash = hash_range(q, n * isz, 8);
                ir.checked_frame = g_frame;
                ir.minmax.clear();
                g_prof.idx_built += n;
            };
            bool covers = !ir.cpu.empty() && first >= ir.first && first + count <= ir.first + ir.count;
            if (covers) {
                bool small = ir.count * isz <= 16384 && ir.changes > 0;   // rewritten before: check every draw
                if (small || ir.checked_frame != g_frame) {
                    u64 hh = hash_range(gmem(ea + ir.first * isz), ir.count * isz, 8);
                    g_prof.hashed += ir.count * isz;
                    if (hh != ir.hash) {
                        if (++ir.changes >= 3) goto idx_per_draw;
                        build(ir.first, ir.count);
                        g_prof.ir_changed += ir.count;
                    }
                    ir.checked_frame = g_frame;
                }
            } else {
                u32 nf = first, ne = first + count;
                if (!ir.cpu.empty()) {   // grow to the union (bounded by the chunk)
                    nf = std::min(ir.first, first);
                    ne = std::max(ir.first + ir.count, first + count);
                }
                build(nf, ne - nf);
                g_prof.ir_grow += ne - nf;
            }
            u64 mk = ((u64)first << 32) | count;
            auto mm = ir.minmax.find(mk);
            if (mm == ir.minmax.end()) {
                u32 lo = ~0u, hi = 0;
                for (u32 k = first - ir.first, e = k + count; k < e; k++) {
                    u32 v = ir.cpu[k];
                    if (v == RESTART) continue;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                mm = ir.minmax.emplace(mk, std::make_pair(lo, hi)).first;
            }
            min_v = mm->second.first;
            max_v = mm->second.second;
            if (min_v > max_v) return;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return; }
            idx_count = count;
            if (ir.off != ~0u) idx_offset = (GLintptr)ir.off + (GLintptr)(first - ir.first) * 4;
            else idx_offset = idx_stream(&ir.cpu[first - ir.first], count);
            region = true;
            need = false;
        }
    idx_per_draw:
        if (!region && contiguous && !idx_nocache) {
            u32 id_src = indexed ? ea + first * isz : first;
            u32 id_flags = g_prim | (indexed ? 0x100 : 0) | (u16idx ? 0x200 : 0) | (restart ? 0x400 | (rv << 16) : 0);
            u64 key = mix64(((u64)id_src << 32) ^ count) ^ mix64(id_flags);
            ie = &g_idx_cache[key];
            bool cached = ie->off != ~0u;
            if (cached && (ie->id_src != id_src || ie->id_count != count || ie->id_flags != id_flags)) ie->hash = 0;   // collision
            ie->id_src = id_src; ie->id_count = count; ie->id_flags = id_flags;
            if (cached && !indexed && ie->hash == 1) need = false;   // array draws: the list depends on the range only
            else if (!indexed) h = 1;
            else if (cached && count * isz > 16384 && ie->checked_frame == g_frame) {
                need = false;   // large index lists are checked once per frame (the same mesh is drawn in several passes)
            } else {
                h = hash_range(gmem(ea + first * isz), count * isz, 8);
                g_prof.hashed += count * isz;
                need = !cached || h != ie->hash;
                ie->checked_frame = g_frame;
            }
            if (cached) ie->last_frame = g_frame;
        }
        if (need) {
            // one pass: byte swap, restart index, vertex range (the lists stay absolute: drawn with base vertex -min_v)
            static std::vector<u32> indices;
            indices.resize(count);
            u32* o = indices.data();
            u32 rv16 = (u16idx && rv >= 0xFFFF) ? 0xFFFF : rv;
            for (auto& r : ranges) {
                if (indexed) {
                    const u8* q = gmem(ea + r.first * isz);
                    if (u16idx)
                        for (u32 i = 0; i < r.count; i++) {
                            u16 x; memcpy(&x, q + i * 2, 2);
                            u32 v = __builtin_bswap16(x);
                            if (restart && (v == rv || v == rv16)) { *o++ = RESTART; continue; }
                            min_v = std::min(min_v, v); max_v = std::max(max_v, v);
                            *o++ = v;
                        }
                    else
                        for (u32 i = 0; i < r.count; i++) {
                            u32 x; memcpy(&x, q + i * 4, 4);
                            u32 v = __builtin_bswap32(x);
                            if (restart && v == rv) { *o++ = RESTART; continue; }
                            min_v = std::min(min_v, v); max_v = std::max(max_v, v);
                            *o++ = v;
                        }
                } else if (r.count) {
                    for (u32 i = 0; i < r.count; i++) *o++ = r.first + i;
                    min_v = std::min(min_v, r.first); max_v = std::max(max_v, r.first + r.count - 1);
                }
            }
            indices.resize(o - indices.data());
            if (min_v > max_v) return;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return; }
            if (g_prim == 8 || g_prim == 9) {   // quads / quad strips as triangle lists
                std::vector<u32> tris;
                size_t seg = 0;
                for (size_t i = 0; i <= indices.size(); i++) {
                    if (i < indices.size() && indices[i] != RESTART) continue;
                    size_t n = i - seg;
                    if (g_prim == 8)
                        for (size_t q = seg; q + 3 < seg + n; q += 4) {
                            u32 a = indices[q], b = indices[q + 1], c = indices[q + 2], d = indices[q + 3];
                            tris.insert(tris.end(), {a, b, c, a, c, d});
                        }
                    else
                        for (size_t q = seg; q + 3 < seg + n; q += 2) {
                            u32 a = indices[q], b = indices[q + 1], c = indices[q + 2], d = indices[q + 3];
                            tris.insert(tris.end(), {a, b, d, a, d, c});
                        }
                    seg = i + 1;
                }
                indices.swap(tris);
            }
            idx_count = (u32)indices.size();
            g_prof.idx_built += idx_count; g_prof.idx_old += idx_count;
            // first sightings are streamed, as for vertex data
            if (ie && ie->off == ~0u && !(ie->seen_frame && ie->seen_frame != g_frame && ie->hash == h)) {
                if (ie->seen_frame != g_frame || ie->hash != h) ie->seen_frame = g_frame;
                ie->hash = h;
                ie->last_frame = g_frame;
                ie = nullptr;
            }
            if (ie && idx_store(indices.data(), idx_count, ie->off, ie->bytes)) {
                ie->hash = h; ie->count = idx_count; ie->min_v = min_v; ie->max_v = max_v;
                ie->last_change = ie->last_frame = g_frame;
                idx_offset = (GLintptr)ie->off;
            } else {
                idx_offset = idx_stream(indices.data(), idx_count);   // (arena full: also cached lists stream)
            }
        } else if (!region) {
            idx_count = ie->count;
            min_v = ie->min_v;
            max_v = ie->max_v;
            idx_offset = (GLintptr)ie->off;
        }
        if (!idx_count) return;
    }
    u32 nverts = max_v - min_v + 1;
    g_prof.t_index += time_usec() - ti0;

    u32 vtx_shift = 0;
    u32 input_mask = reg(0x1FF0);
    if (g_vtx_gpu) setup_attrs_gpu(p, min_v, max_v);
    else {
    // attribute array state of the single VAO, so unchanged attributes cost no GL calls; the buffers of all
    // attributes are bound with one call
    GLuint* attr_buf = g_attr_buf;
    static bool attr_on[16], attr_default[16];
    GLuint want_buf[16];
    GLintptr want_off[16];
    memcpy(want_buf, attr_buf, sizeof(want_buf));
    memcpy(want_off, g_attr_off, sizeof(want_off));
    struct Memo { u64 key, h; };
    Memo memo[16];
    u32 nmemo = 0;
    u32 active = 0;   // attributes fetched by this draw
    for (u32 i = 0; i < 16; i++) {
        u32 fmt = reg(0x1740 + i * 4);
        u32 size = (fmt >> 4) & 0xF;
        if (!size || !(input_mask & (1u << i))) {
            if (attr_on[i] || !attr_default[i]) {
                glDisableVertexAttribArray(i);
                glVertexAttrib4f(i, 0, 0, 0, 1);
                attr_on[i] = false;
                attr_default[i] = true;
            }
            continue;
        }
        u32 stride = (fmt >> 8) & 0xFF;
        // converted vertex data is cached per (source range, format, vertex range); a content hash of the raw
        // bytes detects changes, so static geometry is converted once instead of every draw
        u32 off = reg(0x1680 + i * 4);
        u32 base_ea = location_to_ea(off & 0x7FFFFFFF, off >> 31);
        // raw records referenced by this draw: vertices min_v..max_v, or for instanced attributes (frequency > 1)
        // index / freq (divide) or index % freq (modulo, bit set in 0x1FC0)
        u32 freq = fmt >> 16, rmin = min_v, rmax = max_v;
        bool modulo = ((reg(0x1FC0) >> i) & 1) != 0;
        if (freq > 1) {
            if (modulo) { rmin = 0; rmax = std::min(freq, nverts) - 1; } else { rmin = min_v / freq; rmax = max_v / freq; }
        }
        u32 src = base_ea + (stride ? rmin * stride : 0);
        u32 bytes = stride ? (rmax - rmin + 1) * stride : 16;
        static const bool vtx_nocache = getenv("F1CE_VTX_NOCACHE") != nullptr;   // debug
        bool cacheable = bytes <= 64u << 20 && !vtx_nocache;
        GLuint buf = g_vbo[i];
        bool fresh = true, same_size = false, streamed = false;
        want_off[i] = 0;
        static const bool no_regions = getenv("F1CE_VTX_NOREGION") != nullptr;   // debug: range-keyed cache only
        if (cacheable && stride && freq <= 1 && !no_regions) {
            u64 key = mix64(((u64)base_ea << 32) ^ (fmt & 0xFFFF)) ^ 0x5bd1e995ull;
            RegionEntry& re = g_regions[key];
            if (re.base_ea != base_ea || re.fmt != (fmt & 0xFFFF)) re = RegionEntry{re.buf, base_ea, fmt & 0xFFFF};
            re.last_frame = g_frame;
            auto raw_hash = [&](u32 first, u32 count) {
                u32 ea = base_ea + first * stride, n = count * stride;
                u64 memo_key = ((u64)ea << 32) ^ n ^ (1ull << 63);
                for (u32 m = 0; m < nmemo; m++) if (memo[m].key == memo_key) return memo[m].h;
                u64 th = time_usec();
                u64 h = hash_range(gmem(ea), n, n > 65536 ? 64 : 8);
                g_prof.t_hash += time_usec() - th;
                g_prof.hashed += n;
                if (nmemo < 16) memo[nmemo++] = {memo_key, h};
                return h;
            };
            auto upload = [&](GLuint target, u32 first, u32 count, GLenum usage) {
                u64 tc = time_usec();
                std::vector<float> data((size_t)count * 4);
                convert_attr(i, first, count, data.data());
                glBindBuffer(GL_ARRAY_BUFFER, target);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * 4), data.data(), usage);
                g_prof.t_conv += time_usec() - tc;
                g_prof.conv_verts += count;
            };
            bool covers = re.buf && min_v >= re.first && max_v < re.first + re.count;
            if (covers) {
                // small regions that changed recently (ring buffers rewritten between draws) are checked every draw
                bool small = re.count * stride <= 16384 && g_frame - re.last_change < 60;
                if (small || re.checked_frame != g_frame) {
                    u64 h = raw_hash(re.first, re.count);
                    if (h != re.hash) { upload(re.buf, re.first, re.count, GL_DYNAMIC_DRAW); re.hash = h; g_prof.conv_changed += re.count; re.last_change = g_frame; }
                    re.checked_frame = g_frame;
                }
                buf = re.buf;
                want_off[i] = (GLintptr)(min_v - re.first) * 16;
            } else if (!re.buf && re.seen_frame && re.seen_frame != g_frame) {
                // seen in an earlier frame: give the region its own buffer
                glGenBuffers(1, &re.buf);
                re.first = min_v; re.count = nverts;
                re.hash = raw_hash(re.first, re.count);
                upload(re.buf, re.first, re.count, GL_DYNAMIC_DRAW);
                re.checked_frame = g_frame; re.last_change = g_frame;
                buf = re.buf;
                g_prof.conv_new += nverts;
            } else if (re.buf) {
                // grow to the union when the ranges are close, else start over at this draw's range
                u32 nf = std::min(re.first, min_v), ne = std::max(re.first + re.count, max_v + 1);
                if (ne - nf > 4 * std::max(re.count, nverts) + 4096) { nf = min_v; ne = max_v + 1; }
                re.first = nf; re.count = ne - nf;
                re.hash = raw_hash(re.first, re.count);
                upload(re.buf, re.first, re.count, GL_DYNAMIC_DRAW);
                re.checked_frame = g_frame; re.last_change = g_frame;
                buf = re.buf;
                want_off[i] = (GLintptr)(min_v - re.first) * 16;
                g_prof.conv_new += re.count;
            } else {
                // first sighting (ring-buffer data lives at a new address every frame): streamed, no buffer yet
                re.seen_frame = g_frame;
                GLintptr roff;
                if (u8* rp = ring_alloc(nverts * 16, &roff)) {
                    u64 tc = time_usec();
                    convert_attr(i, min_v, nverts, (float*)rp);
                    g_prof.t_conv += time_usec() - tc;
                    g_prof.conv_verts += nverts;
                    buf = g_ring.buf;
                    want_off[i] = roff;
                } else {
                    upload(g_vbo[i], min_v, nverts, GL_STREAM_DRAW);
                    buf = g_vbo[i];
                }
                g_prof.conv_new += nverts;
            }
            fresh = false;
        } else if (cacheable) {
            u64 key = mix64(((u64)src << 32) ^ nverts) ^ mix64(((u64)min_v << 32) ^ (fmt & 0xFFFF) ^ ((u64)freq << 16) ^ ((u64)modulo << 31));
            VtxEntry& ve = g_vtx_cache[key];
            if (ve.buf && (ve.id_src != src || ve.id_fmt != (fmt & 0xFFFF) || ve.id_min != min_v || ve.id_freq != freq ||
                           ve.id_mod != modulo)) {   // another draw with a colliding key: convert afresh
                ve.hash = ve.hash_sparse = 0;
                ve.nverts = 0;
            }
            ve.id_src = src; ve.id_fmt = fmt & 0xFFFF; ve.id_min = min_v; ve.id_freq = freq; ve.id_mod = modulo;
            const u8* p = gmem(src);
            // large static data (unchanged for 60 frames: track geometry) is re-checked every 16 frames only,
            // staggered per entry; small buffers are always checked (UI quads come from ring buffers that are
            // rewritten after long pauses, and a skipped check drew stale vertices: flickering splash/menus)
            static const bool no_static = getenv("F1CE_VTX_NOSTATIC") != nullptr;   // debug
            bool skip_check = !no_static && bytes > 65536 && ve.buf && ve.nverts == nverts && g_frame - ve.last_change > 60 && (g_frame + key) % 16 != 0;
            // large ranges: sample one 8-byte word every 64 bytes per frame, full hash every 30 frames
            bool full = bytes <= 65536 || !ve.buf || (g_frame + key) % 30 == 0;
            u32 step = full ? 8 : 64;
            // interleaved buffers: the attributes of a draw usually share one range, hashed once per draw
            u64 h;
            u64 memo_key = ((u64)src << 32) ^ bytes ^ ((u64)step << 56);
            const Memo* mit = nullptr;
            for (u32 m = 0; m < nmemo; m++) if (memo[m].key == memo_key) mit = &memo[m];
            if (skip_check) {
                h = full ? ve.hash : ve.hash_sparse;
            } else if (mit) {
                h = mit->h;
            } else {
                u64 th = time_usec();
                h = hash_range(p, bytes, step);
                g_prof.t_hash += time_usec() - th;
                memo[nmemo++] = {memo_key, h};
                g_prof.hashed += bytes / step * 8;
            }
            // data seen for the first time (most dynamic data lives at a new ring-buffer address every frame) is
            // streamed; it gets its own buffer when the same data comes back in a later frame
            if (!ve.buf && !(ve.seen_frame && ve.seen_frame != g_frame && ve.hash == h && ve.nverts == nverts)) {
                if (ve.seen_frame != g_frame || ve.hash != h) ve.seen_frame = g_frame;
                ve.hash = h; ve.hash_sparse = 0; ve.nverts = nverts; ve.last_frame = ve.last_change = g_frame;
                streamed = true;
                g_prof.conv_new += nverts;
            } else {
                same_size = ve.buf && ve.nverts == nverts;
                if (!ve.buf) glGenBuffers(1, &ve.buf);
                buf = ve.buf;
                u64& stored = full ? ve.hash : ve.hash_sparse;
                fresh = !same_size || stored != h;
                if (fresh) { (full ? ve.hash_sparse : ve.hash) = 0; ve.last_change = g_frame; g_prof.conv_changed += nverts; }   // the other hash is stale
                stored = h;
                ve.nverts = nverts;
                ve.last_frame = g_frame;
            }
        }
        GLintptr roff;
        u8* rp = fresh && (streamed || !cacheable) && stride ? ring_alloc(nverts * 16, &roff) : nullptr;
        if (rp) {
            (cacheable ? g_prof.conv_verts : g_prof.conv_inst) += nverts;
            u64 tc = time_usec();
            convert_attr(i, min_v, nverts, (float*)rp);
            g_prof.t_conv += time_usec() - tc;
            buf = g_ring.buf;
            want_off[i] = roff;
            fresh = false;
        }
        if (fresh) {
            (cacheable ? g_prof.conv_verts : g_prof.conv_inst) += nverts;
            u64 tc = time_usec();
            std::vector<float> data((size_t)nverts * 4);
            if (stride) convert_attr(i, min_v, nverts, data.data());
            else for (u32 v = 0; v < nverts; v++) fetch_attr(i, 0, &data[(size_t)v * 4]);
            glBindBuffer(GL_ARRAY_BUFFER, buf);
            // same size as the buffer's current store (dynamic data rewritten in place): no reallocation
            GLsizeiptr sz = (GLsizeiptr)(data.size() * 4);
            static const bool no_subdata = getenv("F1CE_VTX_NOSUBDATA") != nullptr;   // debug
            if (cacheable && !streamed && same_size && !no_subdata) glBufferSubData(GL_ARRAY_BUFFER, 0, sz, data.data());
            else glBufferData(GL_ARRAY_BUFFER, sz, data.data(), cacheable && !streamed ? GL_STATIC_DRAW : GL_STREAM_DRAW);
            g_prof.t_conv += time_usec() - tc;
        }
        want_buf[i] = buf;
        active |= 1u << i;
        if (!attr_on[i]) {
            glEnableVertexAttribArray(i);
            attr_on[i] = true;
        }
    }
    // the offsets' common part goes into the base vertex (vtx_shift records): a mesh whose attributes share their
    // region start keeps the same bindings across its draws, and glBindVertexBuffers (~1.6 us per call in the AMD
    // driver, called on almost every draw while the offsets moved with each draw's first vertex) is skipped
    vtx_shift = ~0u;
    for (u32 i = 0; i < 16; i++)
        if (active >> i & 1) vtx_shift = std::min(vtx_shift, (u32)(want_off[i] / 16));
    if (vtx_shift == ~0u) vtx_shift = 0;
    for (u32 i = 0; i < 16; i++)
        if (active >> i & 1) want_off[i] -= (GLintptr)vtx_shift * 16;
    {
        u32 lo = 16, hi = 0;
        for (u32 i = 0; i < 16; i++)
            if (want_buf[i] != attr_buf[i] || want_off[i] != g_attr_off[i]) { lo = std::min(lo, i); hi = i; }
        if (lo <= hi) {
            static const GLsizei strides[16] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
            u64 tb = time_usec();
            glBindVertexBuffers(lo, (GLsizei)(hi - lo + 1), want_buf + lo, want_off + lo, strides);
            g_prof.t_bind += time_usec() - tb;
            g_prof.binds++;
            memcpy(attr_buf + lo, want_buf + lo, (hi - lo + 1) * sizeof(GLuint));
            memcpy(g_attr_off + lo, want_off + lo, (hi - lo + 1) * sizeof(GLintptr));
        }
    }
    }   // CPU attribute path
    if (first_of_prog) {
        LOG_I("rsx draw diag: prim=%u verts=%u..%u idx=%zu mask=%x vp_scale=%f,%f,%f off=%f,%f,%f surf=%ux%u blend=%x depth=%x cmask=%08x",
              g_prim, min_v, max_v, (size_t)idx_count, input_mask, regf(0x0A20), regf(0x0A24), regf(0x0A28), regf(0x0A30),
              regf(0x0A34), regf(0x0A38), target->width, target->height, reg(0x0310), reg(0x0A74), reg(0x0324));
        for (u32 i = 0; i < 16; i++) {
            u32 fmt = reg(0x1740 + i * 4);
            if (!((fmt >> 4) & 0xF) || !(input_mask & (1u << i))) continue;
            float v0[4], v1[4];
            fetch_attr(i, min_v, v0);
            fetch_attr(i, min_v + 1, v1);
            LOG_I("   attr%u fmt=%08x off=%08x v0=(%g %g %g %g) v1=(%g %g %g %g)", i, fmt, reg(0x1680 + i * 4), v0[0], v0[1],
                  v0[2], v0[3], v1[0], v1[1], v1[2], v1[3]);
        }
        LOG_I("   cull: enable=%x face=%x front=%x  c[467]=%g %g %g %g", reg(0x183C), reg(0x1830), reg(0x1834), vp_consts[467 * 4], vp_consts[467 * 4 + 1], vp_consts[467 * 4 + 2], vp_consts[467 * 4 + 3]);
        LOG_I("   frequency divider operation (1fc0)=%08x", reg(0x1FC0));
        LOG_I("   depth: test=%x func=%x mask=%x  c[466]=%g %g %g %g", reg(0x0A74), reg(0x0A6C), reg(0x0A70), vp_consts[466 * 4],
              vp_consts[466 * 4 + 1], vp_consts[466 * 4 + 2], vp_consts[466 * 4 + 3]);
        for (u32 k = 256; k < 260; k++)
            LOG_I("   c[%u] = %g %g %g %g", k, vp_consts[k * 4], vp_consts[k * 4 + 1], vp_consts[k * 4 + 2], vp_consts[k * 4 + 3]);
        LOG_I("   shader_window(1d88)=%08x clip=%08x %08x", reg(0x1D88), reg(0x0200), reg(0x0204));
    }
    u64 t3 = time_usec();
    g_prof.vertices += t3 - t2;
    g_prof.draws++;
    g_prof.verts += nverts;
    static bool restart_on = false;
    if (!restart_on) {
        glEnable(GL_PRIMITIVE_RESTART);
        glPrimitiveRestartIndex(RESTART);
        restart_on = true;
    }
    // debug: F1CE_DUMP_ON_PROG=<prog> dumps unit 0's surface before and the target after that program's first
    // draw of every 300th frame (frame_s<off>_in / _out)
    static const int dump_prog = getenv("F1CE_DUMP_ON_PROG") ? atoi(getenv("F1CE_DUMP_ON_PROG")) : -1;
    static u64 dumped_frame = ~0ull;
    bool dump_now = (int)p->prog == dump_prog && g_frame % 300 == 0 && dumped_frame != g_frame;
    if (dump_now) {
        dumped_frame = g_frame;
        dump_surface(reg(0x1A00), "_in");
        dump_surface(target->offset, "_pre");
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    }
    static const bool no_srgb = getenv("F1CE_NO_TEXGAMMA") != nullptr;   // debug (with the texture decode)
    {   // debug: F1CE_SRGB_LOG logs changes of SHADER_PACKER
        static const bool sl = getenv("F1CE_SRGB_LOG") != nullptr;
        static u32 last = ~0u, n = 0;
        if (sl && reg(0x1FD8) != last && n++ < 300)
            LOG_I("srgb: frame %llu 0x1FD8=%x fp=%08x target=%08x", (unsigned long long)g_frame, reg(0x1FD8), reg(0x08E4), target->offset);
        last = reg(0x1FD8);
    }
    // SHADER_PACKER bit 0 is set for the whole game, yet the menu video (YUV planes, no gamma bits) and its copy
    // to the display buffer must not be encoded (washed-out video): encoding is applied to draws that sample
    // sRGB-decoded textures, which keeps both chains consistent (3D passes write encoded colour that the post
    // passes read back with gamma bits; the video chain stays raw). Passes that copy a render target read without
    // decode (motion blur: a gamma-flagged distortion map + the raw scene) are not encoded either, or the blurred
    // layer comes out brighter than the scene.
    // cube map faces stay linear: the car shaders read the cube without decode and add the reflection to linear
    // lighting (encoded faces made the reflections far too strong, most visibly on cars in shadow)
    bool cube_face = g_cube_faces.count(target->offset) != 0;
    {   // debug: F1CE_SRGB_TARGET_OFF=<offset hex> writes that target without sRGB encoding (experiments)
        static const u32 off_t = getenv("F1CE_SRGB_TARGET_OFF") ? (u32)strtoul(getenv("F1CE_SRGB_TARGET_OFF"), nullptr, 16) : ~0u;
        if (target->offset == off_t) cube_face = true;
    }
    bool encode = (reg(0x1FD8) & 1) && reads_gamma && !reads_raw_target && !cube_face && !no_srgb;
    // The encode happens in the fragment shader and blending works on the encoded values, as on the RSX (its
    // generation blends in sRGB space). GL_FRAMEBUFFER_SRGB blends in linear space: multiplicative decals such as the
    // rubber marks on the racing line came out much darker. F1CE_SRGB_FB=1 uses the framebuffer path (comparison).
    static const bool fb_encode = getenv("F1CE_SRGB_FB") != nullptr;
    if (fb_encode) {
        srgb_write(encode);
    } else {
        srgb_write(false);
        const float alpha[4] = {(reg(0x0304) & 1) ? 1.0f : 0.0f, (float)((s32)reg(0x0308) - 0x200), (reg(0x030C) & 255) / 255.0f,
                                encode ? 1.0f : 0.0f};
        uniform_cached(p->loc_alpha, p->u_alpha, alpha, glUniform4fv, 1);
    }
    {   // ANTI_ALIASING_CONTROL bit 0 (AA) + bit 4 (alpha to coverage): the windscreen and other see-through materials
        static int a2c = -1;
        int want = target->ms_active && (reg(0x1D7C) & 0x11) == 0x11;
        if (want != a2c) { if (want) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); a2c = want; }
    }
    {   // debug: F1CE_RSX_DRAW_STALL=<frame>:<us per draw>:<frames> slows every draw (a renderer stuck in a long list)
        static const char* ds = getenv("F1CE_RSX_DRAW_STALL");
        static const u64 ds_frame = ds ? strtoull(ds, nullptr, 10) : 0;
        static const u32 ds_us = ds && strchr(ds, ':') ? (u32)atoi(strchr(ds, ':') + 1) : 0;
        static const char* ds3 = ds && strchr(ds, ':') ? strchr(strchr(ds, ':') + 1, ':') : nullptr;
        static const u64 ds_n = ds3 ? strtoull(ds3 + 1, nullptr, 10) : 1;
        if (ds && g_frame >= ds_frame && g_frame < ds_frame + ds_n) sleep_usec(ds_us);
    }
    u64 tdr = time_usec();
    glDrawElementsBaseVertex(mode, (GLsizei)idx_count, GL_UNSIGNED_INT, (const void*)idx_offset, g_vtx_gpu ? 0 : (GLint)vtx_shift - (GLint)min_v);
    if (g_idx_rebind) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_iheap.buf); g_idx_rebind = false; }
    g_prof.t_draw += time_usec() - tdr;
    if (target->main_ea) target->wb_dirty = true;
    g_write_seq++;
    if (reg(0x0324)) target->write_seq = g_write_seq;   // colour mask
    if (target->ms_active) target->ms_dirty = true;
    if (reg(0x0324) && target->width == target->height) g_square_write_seq = g_write_seq;
    if (g_cur_depth && (reg(0x0A74) & 1) && (reg(0x0A70) & 1)) g_cur_depth->write_seq = g_write_seq;   // depth test + mask
    g_prof.submit += time_usec() - t3;
    if (dump_now) { dump_surface(target->offset, "_out"); glBindFramebuffer(GL_FRAMEBUFFER, target->fbo); }
    {   // debug: F1CE_TRACE_FRAME=<n> logs every draw of frame n
        static const long long trace_frame = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
        if (trace_on(trace_frame)) {
            std::string tex;
            for (u32 u = 0; u < 16; u++)
                if (reg(0x1A0C + u * 0x20) & 0x80000000) {
                    char b[48];
                    snprintf(b, sizeof(b), " t%u=%08x/%02x/g%x", u, reg(0x1A00 + u * 0x20), (reg(0x1A04 + u * 0x20) >> 8) & 0xFF, (reg(0x1A08 + u * 0x20) >> 20) & 0xF);
                    tex += b;
                }
            static int ndraw = 0;
            if (ndraw == 0) {   // non-zero registers of the upper method range (state not otherwise logged)
                std::string rs;
                for (u32 r = 0x1D00; r < 0x2000; r += 4)
                    if (reg(r)) { char b[24]; snprintf(b, sizeof(b), " %04x=%08x", r, reg(r)); rs += b; }
                LOG_I("trace regs:%s", rs.c_str());
            }
            static const int fc_prog = getenv("F1CE_TRACE_FC") ? atoi(getenv("F1CE_TRACE_FC")) : -1;   // debug
            if ((int)p->prog == fc_prog) {
                std::string fc;
                for (size_t i = 0; i < fp_consts.size(); i++) { char b[24]; snprintf(b, sizeof(b), "%s%g", i % 4 ? "," : " | ", fp_consts[i]); fc += b; }
                LOG_I("trace fc prog %u:%s", p->prog, fc.c_str());
                LOG_I("trace vp c[464..467] = %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", vp_consts[464 * 4], vp_consts[464 * 4 + 1],
                      vp_consts[464 * 4 + 2], vp_consts[464 * 4 + 3], vp_consts[465 * 4], vp_consts[465 * 4 + 1], vp_consts[465 * 4 + 2],
                      vp_consts[465 * 4 + 3], vp_consts[466 * 4], vp_consts[466 * 4 + 1], vp_consts[466 * 4 + 2], vp_consts[466 * 4 + 3],
                      vp_consts[467 * 4], vp_consts[467 * 4 + 1], vp_consts[467 * 4 + 2], vp_consts[467 * 4 + 3]);
            }
            for (u32 a = 0; a < 16; a++) {   // raw words of the first vertex of byte attributes (colours)
                u32 fmt = reg(0x1740 + a * 4);
                if ((fmt & 0xF) != 4 || !((fmt >> 4) & 0xF)) continue;
                u32 off = reg(0x1680 + a * 4);
                u32 ea = location_to_ea(off & 0x7FFFFFFF, off >> 31) + min_v * ((fmt >> 8) & 0xFF);
                LOG_I("trace attr%u fmt=%08x first vertices: %08x %08x %08x", a, fmt, ld32(ea), ld32(ea + ((fmt >> 8) & 0xFF)),
                      ld32(ea + 2 * ((fmt >> 8) & 0xFF)));
            }
            LOG_I("trace draw %d: fp=%08x prog %u -> %08x (%ux%u pitch %x) blend=%x sf=%08x df=%08x cmask=%08x depth=%x/%x aa=%x poff=%x/%08x/%08x stencil=%x/%08x/%08x cull=%x/%x/%x prim=%u n=%zu%s", ndraw, reg(0x08E4), p->prog,
                  target->offset, target->width, target->height, reg(0x020C) & 0xFFFF, reg(0x0310), reg(0x0314), reg(0x0318), reg(0x0324), reg(0x0A74), reg(0x0A70), reg(0x1D7C), reg(0x0A68), reg(0x0A78), reg(0x0A7C), reg(0x0348), reg(0x0350), reg(0x0364), reg(0x183C), reg(0x1830), reg(0x1834), g_prim,
                  (size_t)idx_count, tex.c_str());
            // also save the target after each draw (frame_s<off>_d<k>); F1CE_TRACE_DUMP=<from>,<to> limits the draws
            static const char* td = getenv("F1CE_TRACE_DUMP");
            static const int td_from = td && strchr(td, ',') ? atoi(td) : 0;
            static const int td_to = td && strchr(td, ',') ? atoi(strchr(td, ',') + 1) : 1 << 30;
            // F1CE_TRACE_DUMP_FP=<fp ctl hex>,... instead dumps the draws of those fragment programs (draw numbers vary)
            static const std::string td_fp = getenv("F1CE_TRACE_DUMP_FP") ? "," + std::string(getenv("F1CE_TRACE_DUMP_FP")) + "," : "";
            char fpk[16];
            snprintf(fpk, sizeof(fpk), ",%08x,", reg(0x08E4));
            if ((td && ndraw >= td_from && ndraw <= td_to) || (!td_fp.empty() && td_fp.find(fpk) != std::string::npos)) {
                char tag[16];
                snprintf(tag, sizeof(tag), "_d%04d", ndraw);
                dump_surface(target->offset, tag);
                glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
            }
            ndraw++;
        }
    }
    g_draws++;
    if (first_of_prog) {
        GLenum err = glGetError();
        u8 px[4] = {};
        srgb_write(false);
        glReadPixels((GLint)target->width / 2, (GLint)target->height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        LOG_I("   after draw: glError=%x centre pixel rgba=%u,%u,%u,%u (mode %x, %zu indices)", err, px[0], px[1], px[2], px[3],
              mode, (size_t)idx_count);
    }
}

void clear(u32 mask) {
    Surface* s = bind_target();
    if (!s) return;
    if (s->ms_active) s->ms_dirty = true;
    static const long long trace_frame = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
    if (trace_on(trace_frame))
        LOG_I("trace clear: %08x (%ux%u pitch %x) mask=%02x color=%08x", s->offset, s->width, s->height, reg(0x020C) & 0xFFFF, mask, reg(0x1D90));
    GLbitfield bits = 0;
    if (mask & 0xF0) {
        u32 c = reg(0x1D90);
        glColorMask((mask >> 4) & 1, (mask >> 5) & 1, (mask >> 6) & 1, (mask >> 7) & 1);
        glClearColor(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (mask & 1) {
        glDepthMask(GL_TRUE);
        u32 z = reg(0x1D8C);
        glClearDepth((z >> 8) / 16777215.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask & 2) {
        glClearStencil(reg(0x1D8C) & 0xFF);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    srgb_write(false);
    glClear(bits);
    if (s->main_ea) s->wb_dirty = true;
    g_write_seq++;
    if (mask & 0xF0) s->write_seq = g_write_seq;
    if ((mask & 0xF0) && s->width == s->height) g_square_write_seq = g_write_seq;
    if ((mask & 3) && g_cur_depth) g_cur_depth->write_seq = g_write_seq;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_state_valid = false;
}

GLuint g_present_tex = 0;
}  // namespace

// ------------------------------------------------------------------ public interface
void init() {
    // GL objects are created lazily on the RSX thread (first method call)
}

static void gl_init_once() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!host::gl_begin() || !load_gl()) {
        LOG_W("rsx: OpenGL unavailable, rendering disabled");
        return;
    }
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    for (GLuint i = 0; i < 16; i++) {   // attribute i reads float4 records from vertex buffer binding i
        glVertexAttribFormat(i, 4, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribBinding(i, i);
    }
    glGenBuffers(16, g_vbo);
    glGenBuffers(1, &g_ibo);
    ring_init();
    heap_init();
    iarena_init();
    precompile_programs();
    host::input_clock_reset();
    if (g_preloaded.empty()) start_preload();   // no database yet: the binaries of earlier sessions, in the background
    else if (FILE* uf = fopen((shader_cache_dir() + "/used.txt").c_str(), "r")) {   // (keys already listed)
        char line[64];
        while (fgets(line, sizeof(line), uf)) if (u64 k = strtoull(line, nullptr, 16)) g_used_keys.insert(k);
        fclose(uf);
    }
    glGenTextures(1, &g_present_tex);
    gl_ok = true;
    LOG_I("rsx: OpenGL renderer ready (%s)", (const char*)glGetString(GL_RENDERER));
}

// 2D objects as bound by libgcm: subchannel 1 = NV0039 (memory to memory), 3 = NV3062 (2D surface),
// 5 = NV308A (inline image: used to patch fragment-program constants in local memory)
u32 sreg(u32 sub, u32 mm) { return R[((sub << 13) | mm) >> 2]; }

void method_2d(u32 sub, u32 mm, u32 arg) {
    if (sub == 5 && mm >= 0x400 && mm < 0x2000) {          // NV308A COLOR[i]
        u32 fmt = sreg(3, 0x300), pitch_out = sreg(3, 0x304) >> 16;
        u32 bpp = fmt == 0xB || fmt == 0xA ? 4 : 2;           // Y32 / A8R8G8B8 : R5G6B5
        u32 point = sreg(5, 0x304), x = point & 0xFFFF, y = point >> 16;   // POINT (0x300 = colour format)
        u32 idx = (mm - 0x400) >> 2;
        u32 dst = sreg(3, 0x30C) + y * pitch_out + (x + idx) * bpp;
        u32 ea = location_to_ea(dst, sreg(3, 0x188) == CTX_MAIN);
        if (bpp == 4) st32(ea, arg); else st16(ea, (u16)arg);
        static const long long trace_frame = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
        if (trace_on(trace_frame) && idx == 0) LOG_I("trace NV308A -> %08x", dst);
        if (dst - g_watch_fp < 0x800) {
            static int wl = 0;
            if (wl++ < 300) LOG_I("watch fp: NV308A +%03x = %08x (%g)", dst - g_watch_fp, arg, *(float*)&arg);
        }
        static int logged = 0;
        if (logged++ < 64)
            LOG_I("rsx: NV308A fmt %x pitch %u point %u,%u [%u] -> %08x%s = %08x", fmt, pitch_out, x, y, idx, dst,
                  sreg(3, 0x188) == CTX_MAIN ? "(main)" : "", arg);
        return;
    }
    if (sub == 1 && mm == 0x328) {                         // NV0039 BUFFER_NOTIFY: run the transfer
        u32 src = sreg(1, 0x30C), dst = sreg(1, 0x310);
        s32 pitch_in = (s32)sreg(1, 0x314), pitch_out = (s32)sreg(1, 0x318);
        u32 len = sreg(1, 0x31C), lines = sreg(1, 0x320);
        bool src_main = sreg(1, 0x184) == CTX_MAIN, dst_main = sreg(1, 0x188) == CTX_MAIN;
        static int logged = 0;
        if (logged++ < 40)
            LOG_I("rsx: NV0039 copy %08x%s -> %08x%s len %u x %u pitch %d/%d", src, src_main ? "(main)" : "", dst,
                  dst_main ? "(main)" : "", len, lines, pitch_in, pitch_out);
        if (!dst_main && dst < g_watch_fp + 0x800 && dst + len * std::max(1u, lines) > g_watch_fp)
            LOG_I("watch fp: NV0039 copy %08x%s -> %08x len %u x %u", src, src_main ? "(main)" : "", dst, len, lines);
        if (!pitch_in) pitch_in = (s32)len;
        if (!pitch_out) pitch_out = (s32)len;
        u32 sea = location_to_ea(src, src_main), dea = location_to_ea(dst, dst_main);
        for (u32 l = 0; l < lines; l++)
            memmove(gmem(dea + l * pitch_out), gmem(sea + l * pitch_in), len);
        // render target copied: copy the GL surface too (the memory copy above has stale contents)
        auto it = src_main ? g_surfaces.end() : g_surfaces.find(src);
        if (it != g_surfaces.end() && it->second.tex && !dst_main) {
            Surface& s = it->second;
            u32 w = std::min(len / 4, s.width), h = std::min(lines, s.height);
            auto dit = g_surfaces.find(dst);
            Surface* d = dit != g_surfaces.end() && dit->second.tex ? &dit->second : get_surface(dst, w, h, s.format);
            u32 cw = std::min(w, d->width), ch = std::min(h, d->height);
            resolve_ms(s);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->fbo); g_tgt_valid = false;
            glDisable(GL_SCISSOR_TEST);
            srgb_write(false);
            glBlitFramebuffer(0, 0, (GLint)(cw * s.scale), (GLint)(ch * s.scale), 0, 0, (GLint)(cw * d->scale), (GLint)(ch * d->scale),
                              GL_COLOR_BUFFER_BIT, s.scale == d->scale ? GL_NEAREST : GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, 0); g_tgt_valid = false;
            d->last_use = g_frame;
        }
    }
}

// Occlusion counting (ZPASS_PIXEL_CNT reports): the game measures how many pixels of light sources pass the depth
// test (glare and flares) between SET_ZPASS_PIXEL_COUNT_ENABLE toggles and reads cumulative counts with GET_REPORT.
// Each enabled span is a GL samples-passed query; reports are written at the next flip, when the results are in.
struct ZPassState {
    bool enabled = false;
    GLuint active = 0;                 // query of the span in progress
    std::vector<GLuint> spans;         // finished spans since the last clear, in order
    std::vector<GLuint> free_q;
    struct Pending { u32 ea; std::vector<GLuint> spans; };
    std::vector<Pending> pending;
    std::vector<std::vector<GLuint>> retired;   // span lists of cleared counters, recycled after the flip
} g_zpass;
GLuint zpass_query() {
    if (g_zpass.free_q.empty()) { GLuint q; glGenQueries(1, &q); return q; }
    GLuint q = g_zpass.free_q.back();
    g_zpass.free_q.pop_back();
    return q;
}
void zpass_close_span() {
    if (!g_zpass.active) return;
    glEndQuery(GL_SAMPLES_PASSED);
    g_zpass.spans.push_back(g_zpass.active);
    g_zpass.active = 0;
}
void zpass_enable(bool on) {
    if (on == g_zpass.enabled) return;
    g_zpass.enabled = on;
    if (on) { g_zpass.active = zpass_query(); glBeginQuery(GL_SAMPLES_PASSED, g_zpass.active); }
    else zpass_close_span();
}
// writes the pending reports (blocking on their queries) and recycles the queries
void zpass_resolve() {
    std::unordered_map<GLuint, u32> result;
    auto value = [&](GLuint q) {
        auto it = result.find(q);
        if (it != result.end()) return it->second;
        GLuint v = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &v);
        return result[q] = v;
    };
    for (auto& p : g_zpass.pending) {
        u32 total = 0;
        for (GLuint q : p.spans) total += value(q);
        total /= res_scale() * res_scale();   // (the counted targets are scaled local ones)
        st32(p.ea + 8, total);
        static const bool zlog = getenv("F1CE_ZPASS_LOG") != nullptr;   // debug
        if (zlog && g_frame % 300 == 0) LOG_I("zpass report @%08x = %u (%zu spans)", p.ea, total, p.spans.size());
    }
    g_zpass.pending.clear();
    for (auto& l : g_zpass.retired) g_zpass.free_q.insert(g_zpass.free_q.end(), l.begin(), l.end());
    g_zpass.retired.clear();
}

// Presentation (roadmap step 3) when the image is scaled (F1CE_SCALE > 1) or FXAA is on (F1CE_FXAA=1): a full-window
// pass instead of the blit. Downscaling samples a mip chain of the display buffer trilinearly at the level that
// matches the window (the linear blit skipped pixels from 4K to 1080p: supersampling without its smoothing); FXAA
// smooths the remaining edges on the sampled level. Otherwise the original blit is kept.
bool fxaa_on() { static const bool v = getenv("F1CE_FXAA") && atoi(getenv("F1CE_FXAA")) != 0; return v; }

// the window area the image goes to: the guest image's aspect ratio, centred (black bars), unless F1CE_STRETCH=1
void present_rect(const Surface& s, GLint ww, GLint wh, GLint* x, GLint* y, GLint* w, GLint* h) {
    static const bool stretch = getenv("F1CE_STRETCH") && atoi(getenv("F1CE_STRETCH")) != 0;
    *x = 0; *y = 0; *w = ww; *h = wh;
    if (stretch || !s.width || !s.height) return;
    double a = (double)s.width / s.height;
    if ((double)ww / wh > a) { *w = (GLint)(wh * a + 0.5); *x = (ww - *w) / 2; }
    else { *h = (GLint)(ww / a + 0.5); *y = (wh - *h) / 2; }
}

bool present_pass(Surface& s, GLint ww, GLint wh) {
    static GLuint prog = 0;
    static GLint loc_p = -1, loc_win = -1;
    static bool failed = false;
    if (failed || !glGenerateMipmap) return false;
    if (!prog) {
        const std::string vs =
            "#version 330 core\n"
            "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
        const std::string fs =
            "#version 330 core\n"
            "uniform sampler2D u_src;\n"
            "uniform vec4 u_p;     // x = mip level, y = FXAA on, zw = 1 / size of that level\n"
            "uniform vec4 u_win;   // xy = origin, zw = size of the image area in the window\n"
            "out vec4 o;\n"
            "vec3 tap(vec2 uv) { return textureLod(u_src, uv, u_p.x).rgb; }\n"
            "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
            "void main() {\n"
            " vec2 uv = (gl_FragCoord.xy - u_win.xy) / u_win.zw, r = u_p.zw;\n"
            " vec3 m = tap(uv);\n"
            " if (u_p.y < 0.5) { o = vec4(m, 1.0); return; }\n"
            " vec3 nw = tap(uv + vec2(-1.0, -1.0) * r), ne = tap(uv + vec2(1.0, -1.0) * r);\n"
            " vec3 sw = tap(uv + vec2(-1.0, 1.0) * r), se = tap(uv + vec2(1.0, 1.0) * r);\n"
            " float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);\n"
            " float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
            " vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
            " float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
            " dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * r;\n"
            " vec3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));\n"
            " vec3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));\n"
            " float lb = luma(b);\n"
            " o = vec4(lb < lmin || lb > lmax ? a : b, 1.0);\n"
            "}\n";
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        glDeleteShader(v);
        glDeleteShader(f);
        if (!ok) { LOG_W("rsx: presentation shader failed: the blit is used"); failed = true; return false; }
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "u_src"), 0);
        loc_p = glGetUniformLocation(prog, "u_p");
        loc_win = glGetUniformLocation(prog, "u_win");
        LOG_I("rsx: presentation pass (scale %u, FXAA %s)", res_scale(), fxaa_on() ? "on" : "off");
    }
    const u32 pw = s.width * s.scale, ph = s.height * s.scale;
    GLint rx, ry, rw, rh;
    present_rect(s, ww, wh, &rx, &ry, &rw, &rh);
    float level = std::max(0.0f, std::log2(std::min((float)pw / (float)rw, (float)ph / (float)rh)));
    active_unit(0);
    bind_tex2d(s.tex);
    if (level > 0.0f) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }
    float lw = std::max(1.0f, std::floor(pw / std::exp2(std::floor(level)))), lh = std::max(1.0f, std::floor(ph / std::exp2(std::floor(level))));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);   // (the bars)
    glViewport(rx, ry, rw, rh);
    srgb_write(false);
    glUseProgram(prog);
    glUniform4f(loc_p, level, fxaa_on() ? 1.0f : 0.0f, 1.0f / lw, 1.0f / lh);
    if (loc_win >= 0) glUniform4f(loc_win, (float)rx, (float)ry, (float)rw, (float)rh);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (level > 0.0f) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    // the draws' cached GL state is no longer what is set
    g_tgt_valid = g_state_valid = false;
    g_cur_prog = prog;
    return true;
}

u64 frame_number() { return g_frame; }
bool report(u32 type, u32 ea) {
    if (type != 1 || !gl_ok) return false;   // CELL_GCM_ZPASS_PIXEL_CNT
    if (g_zpass.enabled) {   // split the running span so the report covers everything drawn so far
        zpass_close_span();
        g_zpass.active = zpass_query();
        glBeginQuery(GL_SAMPLES_PASSED, g_zpass.active);
    }
    g_zpass.pending.push_back({ea, g_zpass.spans});
    return true;
}
void method(u32 m, u32 arg) {
    m &= 0xFFFC;
    // fast path: most methods only set state (read at draw time); the ones handled below are flagged in a table
    static const auto special = [] {
        std::array<bool, 0x800> t{};
        for (u32 x : {0x1D7Cu, 0x1D84u, 0x17C8u, 0x1E9Cu, 0x1EFCu, 0x1D94u, 0x1808u, 0x1814u, 0x1824u}) t[x >> 2] = true;
        for (u32 x = 0x0B80; x < 0x0C00; x += 4) t[x >> 2] = true;   // TRANSFORM_PROGRAM
        for (u32 x = 0x1F00; x < 0x1F80; x += 4) t[x >> 2] = true;   // TRANSFORM_CONSTANT
        return t;
    }();
    static bool inited = false;
    if (__builtin_expect(inited, 1) && m < 0x2000 && !special[m >> 2]) { R[m >> 2] = arg; return; }
    gl_init_once();
    inited = true;
    if (m == 0x1D7C || m == 0x1D84) {   // debug: distinct values (AA control / zpass enable)
        static std::set<u64> seen;
        static std::mutex mu;
        std::lock_guard lk(mu);
        if (seen.size() < 64 && seen.insert(((u64)m << 32) | arg).second) LOG_I("rsx: method %04x = %08x (frame %llu)", m, arg, (unsigned long long)g_frame);
    }
    if (m == 0x1D7C) R[m >> 2] = arg;   // ANTI_ALIASING_CONTROL (bit 4: alpha to coverage)
    if (gl_ok && m == 0x1D7C) { zpass_enable(arg & 1); return; }   // NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE
    if (gl_ok && m == 0x17C8 && arg == 1) {                         // NV4097_CLEAR_REPORT_VALUE (ZPASS_PIXEL_CNT)
        bool on = g_zpass.enabled;
        zpass_enable(false);
        g_zpass.retired.push_back(std::move(g_zpass.spans));
        g_zpass.spans.clear();
        zpass_enable(on);
        return;
    }
    static const bool sub_stats = getenv("F1CE_SUBCH_STATS") != nullptr;   // debug (a map update + log per method)
    if ((m >> 13) && sub_stats) {   // other subchannels (2D objects): method histogram
        static std::map<u32, u64> hist;
        static u64 total = 0;
        if ((m & 0x1FFF) == 0) LOG_I("rsx: subchannel %u SET_OBJECT %08x", m >> 13, arg);
        hist[m]++;
        if (++total % 2000 == 1) {
            std::string h;
            for (auto& [k, v] : hist) { char b[32]; snprintf(b, sizeof(b), " %u:%04x=%llu", k >> 13, k & 0x1FFF, (unsigned long long)v); h += b; }
            LOG_I("rsx: subchannel methods:%s", h.c_str());
        }
    }
    u32 prev = R[m >> 2];
    R[m >> 2] = arg;
    if (!gl_ok) return;
    if (m >> 13) { method_2d(m >> 13, m & 0x1FFF, arg); return; }
    if (m >= 0x0B80 && m < 0x0C00) {                    // TRANSFORM_PROGRAM[32]
        if (vp_load_pos < 512 * 4) vp_words[vp_load_pos++] = arg;
        return;
    }
    if (m >= 0x1F00 && m < 0x1F80) {                    // TRANSFORM_CONSTANT[32]
        {   // debug: F1CE_TRACE_VPC=<slot> logs writes to that vertex constant during the traced frame
            static const int vpc = getenv("F1CE_TRACE_VPC") ? atoi(getenv("F1CE_TRACE_VPC")) : -1;
            static const long long tf = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
            if ((int)(vp_const_pos >> 2) == vpc && (long long)g_frame == tf) {
                float f; memcpy(&f, &arg, 4);
                LOG_I("trace vpc[%d].%c = %g (draws so far %llu)", vpc, "xyzw"[vp_const_pos & 3], f, (unsigned long long)g_draws);
            }
        }
        if (vp_const_pos < 468 * 4) {
            vp_slot_ver[vp_const_pos >> 2] = ++vp_ver;
            memcpy(&vp_consts[vp_const_pos++], &arg, 4);
        }
        return;
    }
    switch (m) {
        case 0x1E9C: vp_load_pos = (arg & 511) * 4; break;       // TRANSFORM_PROGRAM_LOAD
        case 0x1EFC: vp_const_pos = (arg % 468) * 4; break;     // TRANSFORM_CONSTANT_LOAD
        case 0x1D94: clear(arg); break;                          // CLEAR_SURFACE
        case 0x1808:                                             // SET_BEGIN_END
            if (arg) {
                g_prim = arg;
                g_array_ranges.clear();
                g_index_ranges.clear();
            } else {
                if (!g_index_ranges.empty()) draw(true);
                else if (!g_array_ranges.empty()) draw(false);
                g_array_ranges.clear();
                g_index_ranges.clear();
            }
            break;
        case 0x1814: g_array_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;   // DRAW_ARRAYS
        case 0x1824: g_index_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;   // DRAW_INDEX_ARRAY
        default: (void)prev; break;
    }
}

void flip(u32 ea, u32 pitch, u32 width, u32 height) {
    gl_init_once();
    if (gl_ok) resolve_all_ms();
    {   // debug: F1CE_RSX_STALL=<frame>:<ms>[:<count>] stalls the renderer (tests the PPU throttle / FIFO robustness)
        static const char* st = getenv("F1CE_RSX_STALL");
        static const u64 st_frame = st ? strtoull(st, nullptr, 10) : 0;
        static const u32 st_ms = st && strchr(st, ':') ? (u32)atoi(strchr(st, ':') + 1) : 0;
        static const char* st3 = st && strchr(st, ':') ? strchr(strchr(st, ':') + 1, ':') : nullptr;
        static const u64 st_count = st3 ? strtoull(st3 + 1, nullptr, 10) : 1;
        if (st && g_frame >= st_frame && g_frame < st_frame + st_count) sleep_usec(st_ms * 1000ull);
    }
    if (!g_start_us) g_start_us = time_usec();
    {   // debug: F1CE_GL_FINISH=1 waits for the GPU at each flip and logs the average wait (GPU-bound check)
        static const bool fin = getenv("F1CE_GL_FINISH") != nullptr;
        static u64 wait_us = 0, n = 0;
        if (fin) {
            u64 t = time_usec();
            glFinish();
            wait_us += time_usec() - t;
            if (++n % 300 == 0) { LOG_I("perf gpu: glFinish at flip waits %.1f ms per frame", wait_us / 300.0 / 1e3); wait_us = 0; }
        }
    }
    {   // debug: F1CE_FLIP_LOG=<first>,<count> logs the displayed buffer and the targets drawn/cleared per frame
        static const char* fl = getenv("F1CE_FLIP_LOG");
        static u64 first = fl ? strtoull(fl, nullptr, 10) : 0, count = fl && strchr(fl, ',') ? strtoull(strchr(fl, ',') + 1, nullptr, 10) : 0;
        if (fl && g_frame >= first && g_frame < first + count) {
            std::string d;
            for (auto& [off, n] : g_frame_targets) { char b[32]; snprintf(b, sizeof(b), " %08x:%u", off, n); d += b; }
            LOG_I("flip %llu: show %08x | drawn%s", (unsigned long long)g_frame, ea - mem::RSX_LOCAL, d.c_str());
        }
        g_frame_targets.clear();
    }
    g_frame++;
    if (g_frame % 300 == 0) LOG_I("rsx flip: display buffer offset %08x", ea - mem::RSX_LOCAL);
    if (!gl_ok) { host::present(ea, pitch, width, height); return; }
    u32 offset = ea - mem::RSX_LOCAL;
    int ww, wh;
    host::gl_drawable_size(&ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); g_tgt_valid = false;
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    srgb_write(false);
    glClear(GL_COLOR_BUFFER_BIT);
    // debug: F1CE_DUMP_SURFACES=off1,off2,... dumps those surfaces every 300 frames (frame_s<off>_<n>_WxH.argb)
    static const char* dump_list = getenv("F1CE_DUMP_SURFACES");
    if (dump_list && g_frame % 300 == 0) {
        std::string l = dump_list;
        size_t pos = 0;
        while (pos < l.size()) {
            size_t e = l.find(',', pos);
            u32 off = (u32)strtoul(l.substr(pos, e == std::string::npos ? std::string::npos : e - pos).c_str(), nullptr, 16);
            dump_surface(off, "");
            if (e == std::string::npos) break;
            pos = e + 1;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0); g_tgt_valid = false;
    }
    auto it = g_surfaces.find(offset);
    if (it != g_surfaces.end()) {
        Surface& s = it->second;
        // the vertex shader already maps RSX top-left window coords to GL (top row = top of image)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); g_tgt_valid = false;
        srgb_write(false);
        const u32 pw = s.width * s.scale, ph = s.height * s.scale;
        if (!((res_scale() > 1 || fxaa_on()) && present_pass(s, ww, wh))) {
            GLint rx, ry, rw, rh;
            present_rect(s, ww, wh, &rx, &ry, &rw, &rh);
            if (rw != ww || rh != wh) {   // black bars around the image
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                g_state_valid = false;
            }
            glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, rx, ry, rx + rw, ry + rh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        {   // debug: F1CE_DUMP_WINDOW=<n> saves the window (after presentation) every n-th frame as frame_w_<frame>_WxH.argb
            static const int dw = getenv("F1CE_DUMP_WINDOW") ? atoi(getenv("F1CE_DUMP_WINDOW")) : 0;
            if (dw > 0 && g_frame % (u64)dw == 0) {
                std::vector<u32> px((size_t)ww * wh), fl(px.size());
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glReadPixels(0, 0, ww, wh, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
                for (GLint y = 0; y < wh; y++) memcpy(&fl[(size_t)y * ww], &px[(size_t)(wh - 1 - y) * ww], (size_t)ww * 4);
                char name[96];
                snprintf(name, sizeof(name), "frame_w_%05llu_%dx%d.argb", (unsigned long long)g_frame, (int)ww, (int)wh);
                if (FILE* f = fopen(name, "wb")) { fwrite(fl.data(), 4, fl.size(), f); fclose(f); }
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);   // (frame dumps below read the display buffer)
        static const char* dump = getenv("F1CE_DUMP_FRAMES");
        if (dump) {
            std::vector<u32> px((size_t)pw * ph);
            srgb_write(false);
            glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
            std::vector<u32> flipped(px.size());
            for (u32 y = 0; y < ph; y++)
                memcpy(&flipped[(size_t)y * pw], &px[(size_t)(ph - 1 - y) * pw], pw * 4);
            host::dump_frame(flipped.data(), pw, ph);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0); g_tgt_valid = false;
    }
    zpass_resolve();
    for (auto& [k, sf] : g_surfaces)   // main-memory render targets: finish last frame's readback, start this one
        if (sf.main_ea) { write_back_pending(&sf); write_back_start(&sf); }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); g_tgt_valid = false;
    u64 ts = time_usec();
    host::gl_swap();
    g_prof.other += time_usec() - ts;
    {
        static u64 window_start = time_usec(), window_frames = 0;
        window_frames++;
        if (time_usec() - window_start >= 5000000) {
            u64 now = time_usec(), wall = now - window_start;
            double nf = (double)window_frames;
            window_frames = 0;
            static u64 idle_last = 0;
            u64 idle = g_rsx_idle_us - idle_last;
            idle_last = g_rsx_idle_us;
            LOG_I("perf vtx: converted %.0f cached-range verts + %.0f instanced verts per frame, hashed %.1f MB vertex + %.1f MB texture data (new %.0f, changed %.0f), %.0f indices rebuilt (region grow %.0f changed %.0f, old path %.0f) | ms: ring wait %.1f (%.0f segments) index %.1f hash %.1f convert+upload %.1f bind %.1f (%.0f calls) draw call %.1f",
                  g_prof.conv_verts / nf, g_prof.conv_inst / nf, g_prof.hashed / nf / 1e6, g_prof.tex_hashed / nf / 1e6,
                  g_prof.conv_new / nf, g_prof.conv_changed / nf, g_prof.idx_built / nf, g_prof.ir_grow / nf, g_prof.ir_changed / nf, g_prof.idx_old / nf, g_prof.t_ringwait / nf / 1e3, g_prof.ring_segs / nf, g_prof.t_index / nf / 1e3, g_prof.t_hash / nf / 1e3, g_prof.t_conv / nf / 1e3, g_prof.t_bind / nf / 1e3, g_prof.binds / nf, g_prof.t_draw / nf / 1e3);
            LOG_I("perf: %.1f fps | per frame ms: program %.1f textures %.1f (uniforms %.1f state %.1f uploads %.1f) vertices %.1f submit %.1f swap %.1f, fifo idle %.1f | %llu draws %llu verts",
                  nf * 1e6 / wall, g_prof.program / nf / 1e3, g_prof.textures / nf / 1e3, g_prof.t_pre / nf / 1e3, g_prof.t_apply / nf / 1e3, g_prof.t_upload / nf / 1e3, g_prof.vertices / nf / 1e3, g_prof.submit / nf / 1e3,
                  g_prof.other / nf / 1e3, idle / nf / 1e3, (unsigned long long)(g_prof.draws / nf), (unsigned long long)(g_prof.verts / nf));
            LOG_I("perf texture units: %.0f memo hits, %.0f misses per frame", g_prof.memo_hits / nf, g_prof.memo_miss / nf);
            if (g_prof.tex_decoded)
                LOG_I("perf textures: %.2f MB decoded per frame, %.2f ms (upload + dispatch)", g_prof.tex_decoded / nf / 1048576.0, g_prof.t_texdec / nf / 1e3);
            if (g_vtx_gpu)
                LOG_I("perf vertex heap: %.1f MB in use, %zu raw regions, %u failed allocations, %zu blocks pending", g_heap.used / 1048576.0,
                      g_raw_regions.size(), g_heap.failed, g_heap.pending.size());
            g_prof = {};
            window_start = now;
        }
    }
    {   // test aid: F1CE_QUIT_AFTER_3D=<s>: the first frame with >= 800 draws starts a timer; after <s> seconds
        // the displayed frame and the 3D target 0x740000 are saved (frame_s*_quit) and the process exits
        static const double quit_after = getenv("F1CE_QUIT_AFTER_3D") ? atof(getenv("F1CE_QUIT_AFTER_3D")) : -1;
        static u64 last_draws = 0, start_us = 0;
        u64 frame_draws = g_draws - last_draws;
        last_draws = g_draws;
        if (quit_after >= 0) {
            if (!start_us && frame_draws >= 800) {
                start_us = time_usec();
                LOG_I("rsx: 3D scene detected at frame %llu (%llu draws), %.1f s after the first frame", (unsigned long long)g_frame,
                      (unsigned long long)frame_draws, (time_usec() - g_start_us) / 1e6);
            }
            if (start_us && time_usec() - start_us >= (u64)(quit_after * 1e6)) {
                dump_surface(offset, "_quit");
                dump_surface(0x740000, "_quit");
                // debug: F1CE_SCAN_WORD=<hex>[,<hex>] searches committed guest memory for those words (context printed)
                if (const char* e = getenv("F1CE_SCAN_WORD")) {
                    std::vector<u32> words;
                    for (const char* p = e; *p;) { words.push_back((u32)strtoul(p, nullptr, 16)); while (*p && *p != ',') p++; if (*p) p++; }
                    int hits = 0;
                    // the committed guest ranges (core.cpp mem::init): ELF + main memory, HLE heap, user memory, RSX
                    // local memory, stacks; read through host pointers (the values are big-endian)
                    const u32 ranges[][2] = {{0x10000, 0x11000000}, {mem::MAIN_HEAP, mem::MAIN_HEAP + mem::MAIN_HEAP_SIZE},
                                             {mem::USER_BASE, mem::USER_BASE + mem::USER_SIZE},
                                             {mem::RSX_LOCAL, mem::RSX_LOCAL + mem::RSX_LOCAL_SIZE}};
                    for (auto& r : ranges) {
                        const u32* base = (const u32*)gmem(r[0]);
                        for (u32 i = 0; i < (r[1] - r[0]) / 4 && hits < 200; i++) {
                            u32 v = __builtin_bswap32(base[i]);
                            // F1CE_SCAN_FLOAT=<value>: also floats within 0.1% (plain or halfword-swapped as in FPs)
                            static const float sf = getenv("F1CE_SCAN_FLOAT") ? (float)atof(getenv("F1CE_SCAN_FLOAT")) : 0.0f;
                            if (sf != 0.0f) {
                                for (u32 x : {v, (v << 16) | (v >> 16)}) {
                                    float f; memcpy(&f, &x, 4);
                                    if (std::fabs(f - sf) <= std::fabs(sf) * 0.001f) {
                                        hits++;
                                        u32 q = r[0] + i * 4, c = q & ~15u;
                                        LOG_I("scan float %g at %08x%s: %08x %08x %08x %08x | %08x %08x %08x %08x", f, q, x == v ? "" : " (swapped)",
                                              ld32(c - 16), ld32(c - 12), ld32(c - 8), ld32(c - 4), ld32(c), ld32(c + 4), ld32(c + 8), ld32(c + 12));
                                    }
                                }
                            }
                            for (u32 wv : words)
                                if (v == wv) {
                                    hits++;
                                    u32 q = r[0] + i * 4, c = q & ~15u;
                                    LOG_I("scan %08x at %08x: %08x %08x %08x %08x | %08x %08x %08x %08x", wv, q, ld32(c - 16),
                                          ld32(c - 12), ld32(c - 8), ld32(c - 4), ld32(c), ld32(c + 4), ld32(c + 8), ld32(c + 12));
                                }
                        }
                    }
                    LOG_I("scan done: %d hits", hits);
                }
                LOG_I("rsx: programs: %llu compiled, %llu from the disk cache, %.0f ms total", (unsigned long long)g_compiled,
                      (unsigned long long)g_cache_hits, g_compile_us / 1000.0);
                LOG_I("rsx: F1CE_QUIT_AFTER_3D reached at frame %llu, exiting", (unsigned long long)g_frame);
                fflush(stdout);
                fflush(stderr);
                _exit(0);
            }
        }
    }
    if (g_frame % 600 == 1) LOG_I("rsx: programs: %llu compiled, %llu from the disk cache, %.0f ms total", (unsigned long long)g_compiled,
                                  (unsigned long long)g_cache_hits, g_compile_us / 1000.0);
    if (g_frame % 600 == 1) LOG_I("rsx: frame %llu, %llu draws, %zu programs, %zu textures, %zu surfaces, %zu vertex + %zu index buffers",
                                  (unsigned long long)g_frame, (unsigned long long)g_draws, g_programs.size(),
                                  g_textures.size(), g_surfaces.size(), g_vtx_cache.size(), g_idx_cache.size());
    if (g_frame % 60 == 0) { evict_buffers(); evict_raw_regions(); }
    heap_end_frame(g_heap);
    heap_end_frame(g_iheap);
}
}  // namespace rsx

