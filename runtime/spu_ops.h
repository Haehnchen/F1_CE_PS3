// SPU instruction semantics (helpers + one handler per instruction), shared by the interpreter (spu.cpp) and
// the ahead-of-time translated blocks (generated/spu_aot*.cpp), which call the handlers with constant words.
#pragma once
#include "spu.h"

#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <vector>
#include <algorithm>
#include <atomic>
#include <type_traits>

using u128 = unsigned __int128;

namespace {

FORCE_INLINE __m128i rev() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
FORCE_INLINE u128 q(const v128& v) { u128 x; memcpy(&x, &v, 16); return x; }
FORCE_INLINE void setq(v128& v, u128 x) { memcpy(&v, &x, 16); }
FORCE_INLINE u8& Bb(v128& v, int i) { return v.b[15 - i]; }      // BE byte i
FORCE_INLINE u8 Bb(const v128& v, int i) { return v.b[15 - i]; }
FORCE_INLINE s32 sext(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }

// instruction fields
FORCE_INLINE int RT(u32 w) { return w & 0x7F; }
FORCE_INLINE int RA(u32 w) { return (w >> 7) & 0x7F; }
FORCE_INLINE int RB(u32 w) { return (w >> 14) & 0x7F; }
FORCE_INLINE int RRR_RT(u32 w) { return (w >> 21) & 0x7F; }
FORCE_INLINE int RRR_RC(u32 w) { return w & 0x7F; }
FORCE_INLINE s32 I7(u32 w) { return sext((w >> 14) & 0x7F, 7); }
FORCE_INLINE s32 I10(u32 w) { return sext((w >> 14) & 0x3FF, 10); }
FORCE_INLINE s32 I16(u32 w) { return sext((w >> 7) & 0xFFFF, 16); }
FORCE_INLINE u32 U16(u32 w) { return (w >> 7) & 0xFFFF; }
FORCE_INLINE u32 I18(u32 w) { return (w >> 7) & 0x3FFFF; }
FORCE_INLINE u32 I8(u32 w) { return (w >> 14) & 0xFF; }


// Handlers are templates over the register context S: SPUThread (interpreter, single-block translations) or SpuLocal
// (cluster translations: a register file on the host stack that the compiler keeps in host registers, see below).
// SPU_AOT_FAST (generated translation units): no debug hooks in loads/stores, handlers forced inline
#ifdef SPU_AOT_FAST
#define SPU_DEBUG_HOOKS 0
#define OP(name) template <class S> [[maybe_unused]] static inline __attribute__((always_inline)) void op_##name(S& s, u32 w)
#else
#define SPU_DEBUG_HOOKS 1
#define OP(name) template <class S> [[maybe_unused]] static inline void op_##name(S& s, u32 w)
#endif
#define R(n) s.gpr[n]

// Register context of cluster translations: gpr holds local copies of the SPU registers the cluster uses (copied in
// on entry, back on exit and around channel/stop instructions, which run on the thread itself). It never escapes,
// so stores into the local store cannot alias it and register values stay in host registers between instructions.
struct SpuLocal {
    v128 gpr[128];
    SPUThread& t;
    u8* ls;
    u32 pc = 0;
    bool* line_code;
    u64& code_writes;
    u32& srr0;
    bool& interrupts;
    explicit SpuLocal(SPUThread& th)
        : t(th), ls(th.ls), line_code(th.line_code), code_writes(th.code_writes), srr0(th.srr0),
          interrupts(th.interrupts) {}
    u32 pref(int r) const { return gpr[r].w[3]; }
    void set_pref(int r, u32 v) { gpr[r].pi = _mm_setzero_si128(); gpr[r].w[3] = v; }
};

// ------------------------------------------------------------------ memory
void trace_ls(SPUThread& s, const char* op, u32 a, int rt) {
    if (s.trace_budget == 0) return;
    s.trace_budget--;
    LOG_I("SPU %s: %s ls[%05x] r%d=%08x %08x %08x %08x at %05x", s.name.c_str(), op, a & 0x3FFF0, rt, s.gpr[rt].w[3],
          s.gpr[rt].w[2], s.gpr[rt].w[1], s.gpr[rt].w[0], s.pc);
}
template <class S> FORCE_INLINE void lq(S& s, int rt, u32 a) {
    R(rt).pi = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(s.ls + (a & 0x3FFF0))), rev());
    if constexpr (std::is_same_v<S, SPUThread>)
        if (SPU_DEBUG_HOOKS && (a & 0x3FFF0) < s.trace_ls_limit) trace_ls(s, "load ", a, rt);
}
// debug: F1CE_SPU_LS_WATCH=<ls addr>[,<ls addr>...] logs SPU stores to those quadwords (and DMA GETs covering them)
std::vector<u32> g_ls_watch = [] {
    std::vector<u32> v;
    if (const char* e = getenv("F1CE_SPU_LS_WATCH"))
        for (const char* p = e; *p;) { v.push_back((u32)strtoul(p, nullptr, 16) & 0x3FFF0); while (*p && *p != ',') p++; if (*p) p++; }
    return v;
}();
void ls_watch_hit(SPUThread& s, u32 a, int rt) {
    static std::atomic<int> n{0};
    if (n++ < 300)
        LOG_I("ls watch %s store [%05x] = %08x %08x %08x %08x pc=%05x", s.name.c_str(), a, s.gpr[rt].w[3], s.gpr[rt].w[2],
              s.gpr[rt].w[1], s.gpr[rt].w[0], s.pc);
}
template <class S> FORCE_INLINE void stq(S& s, int rt, u32 a) {
    if constexpr (std::is_same_v<S, SPUThread>) {
        if (SPU_DEBUG_HOOKS && (a & 0x3FFF0) < s.trace_ls_limit) trace_ls(s, "store", a, rt);
        if (SPU_DEBUG_HOOKS && __builtin_expect(!g_ls_watch.empty(), 0))
            for (u32 x : g_ls_watch) if (x == (a & 0x3FFF0)) ls_watch_hit(s, x, rt);
        if (SPU_DEBUG_HOOKS && __builtin_expect(s.lsw_base != 0, 0) && (a & 0x3FFF0) - s.lsw_base < 0x800) {
            u32 k = ((a & 0x3FFF0) - s.lsw_base) >> 4;
            s.lsw_pc[k] = s.pc;
            s.lsw_val[k] = s.gpr[rt].w[3];
        }
    }
    __m128i v = _mm_shuffle_epi8(R(rt).pi, rev());
    __m128i* p = (__m128i*)(s.ls + (a & 0x3FFF0));
    if (__builtin_expect(s.line_code[(a >> 7) & 2047], 0)) {
        // store into decoded code: invalidate only when the bytes change. The job manager's interrupt handler
        // writes the same 80 register-save instructions onto its stack at every interrupt; counting those as code
        // changes revalidated every block of the SPU (and emptied its dispatch cache) after each interrupt.
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(p), v)) == 0xFFFF) return;
        _mm_storeu_si128(p, v);
        s.code_writes++;
        // the line's version is stale; the line counts as code again only when a block there is checked. Lines stayed
        // marked forever: the stack, where that handler generates its
        // code, then invalidated on every ordinary store (~7 million times per second per SPU)
        s.line_code[(a >> 7) & 2047] = false;
        return;
    }
    _mm_storeu_si128(p, v);
}
OP(lqd) { lq(s, RT(w), s.pref(RA(w)) + I10(w) * 16); }
OP(lqx) { lq(s, RT(w), s.pref(RA(w)) + s.pref(RB(w))); }
OP(lqa) { lq(s, RT(w), (u32)(I16(w) << 2)); }
OP(lqr) { lq(s, RT(w), s.pc + (I16(w) << 2)); }
OP(stqd) { stq(s, RT(w), s.pref(RA(w)) + I10(w) * 16); }
OP(stqx) { stq(s, RT(w), s.pref(RA(w)) + s.pref(RB(w))); }
OP(stqa) { stq(s, RT(w), (u32)(I16(w) << 2)); }
OP(stqr) { stq(s, RT(w), s.pc + (I16(w) << 2)); }

// insertion controls: default pattern 0x10..0x1F with the selected element replaced
// (computed with vector compares: a variable byte index into the register would keep cluster registers in memory)
template <class S> FORCE_INLINE void gen_ctrl(S& s, int rt, u32 t, int size) {
    u32 pos = t & (16 - size) & 15;
    __m128i be = rev();   // BE byte index of each host lane
    __m128i rel = _mm_sub_epi8(be, _mm_set1_epi8((char)pos));
    __m128i in = _mm_cmpeq_epi8(_mm_min_epu8(rel, _mm_set1_epi8((char)(size - 1))), rel);   // pos <= i < pos + size
    // element bytes: byte 3 (size 1), bytes 2-3 (size 2), 0-3 (size 4), 0-7 (size 8) of the preferred slot
    __m128i val = _mm_add_epi8(rel, _mm_set1_epi8((char)(size == 1 ? 3 : size == 2 ? 2 : 0)));
    R(rt).pi = _mm_blendv_epi8(_mm_add_epi8(be, _mm_set1_epi8(0x10)), val, in);
}
OP(cbd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 1); }
OP(chd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 2); }
OP(cwd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 4); }
OP(cdd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 8); }
OP(cbx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 1); }
OP(chx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 2); }
OP(cwx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 4); }
OP(cdx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 8); }

// ------------------------------------------------------------------ immediates
OP(il) { R(RT(w)).pi = _mm_set1_epi32(I16(w)); }
OP(ilh) { R(RT(w)).pi = _mm_set1_epi16((short)U16(w)); }
OP(ilhu) { R(RT(w)).pi = _mm_set1_epi32((int)(U16(w) << 16)); }
OP(iohl) { R(RT(w)).pi = _mm_or_si128(R(RT(w)).pi, _mm_set1_epi32((int)U16(w))); }
OP(ila) { R(RT(w)).pi = _mm_set1_epi32((int)I18(w)); }
OP(fsmbi) {
    u32 m = U16(w);
    for (int i = 0; i < 16; i++) Bb(R(RT(w)), i) = (m >> (15 - i)) & 1 ? 0xFF : 0;
}

// ------------------------------------------------------------------ integer arithmetic
OP(a) { R(RT(w)).pi = _mm_add_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(ah) { R(RT(w)).pi = _mm_add_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(ai) { R(RT(w)).pi = _mm_add_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(ahi) { R(RT(w)).pi = _mm_add_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(sf) { R(RT(w)).pi = _mm_sub_epi32(R(RB(w)).pi, R(RA(w)).pi); }
OP(sfh) { R(RT(w)).pi = _mm_sub_epi16(R(RB(w)).pi, R(RA(w)).pi); }
OP(sfi) { R(RT(w)).pi = _mm_sub_epi32(_mm_set1_epi32(I10(w)), R(RA(w)).pi); }
OP(sfhi) { R(RT(w)).pi = _mm_sub_epi16(_mm_set1_epi16((short)I10(w)), R(RA(w)).pi); }
OP(addx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RA(w)).w[i] + R(RB(w)).w[i] + (R(RT(w)).w[i] & 1); R(RT(w)) = r;
}
OP(sfx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RB(w)).w[i] - R(RA(w)).w[i] - (1 - (R(RT(w)).w[i] & 1)); R(RT(w)) = r;
}
OP(cg) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((u64)R(RA(w)).w[i] + R(RB(w)).w[i]) >> 32; R(RT(w)) = r;
}
OP(bg) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RB(w)).w[i] >= R(RA(w)).w[i]; R(RT(w)) = r;
}
OP(cgx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((u64)R(RA(w)).w[i] + R(RB(w)).w[i] + (R(RT(w)).w[i] & 1)) >> 32; R(RT(w)) = r;
}
OP(bgx) {
    v128 r;
    for (int i = 0; i < 4; i++) {
        s64 d = (s64)R(RB(w)).w[i] - R(RA(w)).w[i] - (1 - (R(RT(w)).w[i] & 1));
        r.w[i] = d >= 0;
    }
    R(RT(w)) = r;
}
OP(mpy) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i]; R(RT(w)) = r; }
OP(mpyu) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (u32)(u16)R(RA(w)).w[i] * (u16)R(RB(w)).w[i]; R(RT(w)) = r; }
OP(mpyi) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * I10(w); R(RT(w)) = r; }
OP(mpyui) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (u32)(u16)R(RA(w)).w[i] * (u16)I10(w); R(RT(w)) = r; }
OP(mpya) {
    v128 r;
    for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i] + R(RRR_RC(w)).sw[i];
    R(RRR_RT(w)) = r;
}
OP(mpyh) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((R(RA(w)).w[i] >> 16) * (u16)R(RB(w)).w[i]) << 16; R(RT(w)) = r; }
OP(mpys) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = ((s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i]) >> 16; R(RT(w)) = r; }
OP(mpyhh) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)(R(RA(w)).w[i] >> 16) * (s16)(R(RB(w)).w[i] >> 16); R(RT(w)) = r; }
OP(mpyhha) { for (int i = 0; i < 4; i++) R(RT(w)).sw[i] += (s32)(s16)(R(RA(w)).w[i] >> 16) * (s16)(R(RB(w)).w[i] >> 16); }
OP(mpyhhu) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (R(RA(w)).w[i] >> 16) * (R(RB(w)).w[i] >> 16); R(RT(w)) = r; }
OP(mpyhhau) { for (int i = 0; i < 4; i++) R(RT(w)).w[i] += (R(RA(w)).w[i] >> 16) * (R(RB(w)).w[i] >> 16); }
OP(clz) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RA(w)).w[i] ? __builtin_clz(R(RA(w)).w[i]) : 32; R(RT(w)) = r; }
OP(cntb) { v128 r; for (int i = 0; i < 16; i++) r.b[i] = (u8)__builtin_popcount(R(RA(w)).b[i]); R(RT(w)) = r; }
OP(fsmb) {
    u32 m = s.pref(RA(w)) & 0xFFFF; v128 r;
    for (int i = 0; i < 16; i++) Bb(r, i) = (m >> (15 - i)) & 1 ? 0xFF : 0;
    R(RT(w)) = r;
}
OP(fsmh) {
    u32 m = s.pref(RA(w)) & 0xFF; v128 r;
    for (int i = 0; i < 8; i++) r.h[7 - i] = (m >> (7 - i)) & 1 ? 0xFFFF : 0;
    R(RT(w)) = r;
}
OP(fsm) {
    u32 m = s.pref(RA(w)) & 0xF; v128 r;
    for (int i = 0; i < 4; i++) r.w[3 - i] = (m >> (3 - i)) & 1 ? 0xFFFFFFFF : 0;
    R(RT(w)) = r;
}
OP(gb) { u32 m = 0; for (int i = 0; i < 4; i++) m |= (R(RA(w)).w[3 - i] & 1) << (3 - i); s.set_pref(RT(w), m); }
OP(gbh) { u32 m = 0; for (int i = 0; i < 8; i++) m |= (R(RA(w)).h[7 - i] & 1) << (7 - i); s.set_pref(RT(w), m); }
OP(gbb) { u32 m = 0; for (int i = 0; i < 16; i++) m |= (Bb(R(RA(w)), i) & 1) << (15 - i); s.set_pref(RT(w), m); }
OP(avgb) { R(RT(w)).pi = _mm_avg_epu8(R(RA(w)).pi, R(RB(w)).pi); }
OP(absdb) { v128 r; for (int i = 0; i < 16; i++) r.b[i] = (u8)std::abs((int)R(RA(w)).b[i] - (int)R(RB(w)).b[i]); R(RT(w)) = r; }
OP(sumb) {
    v128 r;
    for (int i = 0; i < 4; i++) {
        u32 sa = 0, sb = 0;
        for (int k = 0; k < 4; k++) { sa += R(RA(w)).b[i * 4 + k]; sb += R(RB(w)).b[i * 4 + k]; }
        r.w[i] = (sb << 16) | (sa & 0xFFFF);   // BE halfword 0 = sum of rb, halfword 1 = sum of ra
    }
    R(RT(w)) = r;
}
OP(xsbh) { v128 r; for (int i = 0; i < 8; i++) r.sh[i] = (s8)(R(RA(w)).h[i] & 0xFF); R(RT(w)) = r; }
OP(xshw) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s16)(R(RA(w)).w[i] & 0xFFFF); R(RT(w)) = r; }
OP(xswd) { v128 r; for (int i = 0; i < 2; i++) r.d[i] = (u64)(s64)(s32)(R(RA(w)).d[i] & 0xFFFFFFFF); R(RT(w)) = r; }

// ------------------------------------------------------------------ logical
OP(and_) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(andc) { R(RT(w)).pi = _mm_andnot_si128(R(RB(w)).pi, R(RA(w)).pi); }
OP(andi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(andhi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(andbi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(or_) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(orc) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_xor_si128(R(RB(w)).pi, _mm_set1_epi32(-1))); }
OP(ori) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(orhi) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(orbi) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(xor_) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(xori) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(xorhi) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(xorbi) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(orx) { const v128& a = R(RA(w)); s.set_pref(RT(w), a.w[0] | a.w[1] | a.w[2] | a.w[3]); }
OP(nand) { R(RT(w)).pi = _mm_xor_si128(_mm_and_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(nor) { R(RT(w)).pi = _mm_xor_si128(_mm_or_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(eqv) { R(RT(w)).pi = _mm_xor_si128(_mm_xor_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(selb) {
    const v128& c = R(RRR_RC(w));
    R(RRR_RT(w)).pi = _mm_or_si128(_mm_and_si128(c.pi, R(RB(w)).pi), _mm_andnot_si128(c.pi, R(RA(w)).pi));
}
OP(shufb) {
    // byte-reversed layout: host lane j selects source byte (c & 31): index (c & 15) ^ 15 in a (bit 4 clear) or b;
    // control bytes with bit 7 set give 0x00 (10xxxxxx), 0xFF (110xxxxx) or 0x80 (111xxxxx)
    __m128i c = R(RRR_RC(w)).pi;
    __m128i idx = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(15)), _mm_set1_epi8(15));
    __m128i pa = _mm_shuffle_epi8(R(RA(w)).pi, idx), pb = _mm_shuffle_epi8(R(RB(w)).pi, idx);
    __m128i sel = _mm_blendv_epi8(pa, pb, _mm_slli_epi16(_mm_and_si128(c, _mm_set1_epi8(0x10)), 3));
    __m128i e = _mm_and_si128(c, _mm_set1_epi8((char)0xE0));
    __m128i special = _mm_or_si128(_mm_cmpeq_epi8(e, _mm_set1_epi8((char)0xC0)),
                                   _mm_and_si128(_mm_cmpeq_epi8(e, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0x80)));
    R(RRR_RT(w)).pi = _mm_blendv_epi8(sel, special, c);
}

// ------------------------------------------------------------------ shifts / rotates (per element)
OP(shl) { v128 r; for (int i = 0; i < 4; i++) { u32 n = R(RB(w)).w[i] & 63; r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] << n; } R(RT(w)) = r; }
OP(shli) { u32 n = I7(w) & 63; v128 r; for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] << n; R(RT(w)) = r; }
OP(shlh) { v128 r; for (int i = 0; i < 8; i++) { u32 n = R(RB(w)).h[i] & 31; r.h[i] = n > 15 ? 0 : (u16)(R(RA(w)).h[i] << n); } R(RT(w)) = r; }
OP(shlhi) { u32 n = I7(w) & 31; v128 r; for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : (u16)(R(RA(w)).h[i] << n); R(RT(w)) = r; }
FORCE_INLINE u32 rotl(u32 x, u32 n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
FORCE_INLINE u16 rotl16(u16 x, u32 n) { n &= 15; return (u16)(n ? (x << n) | (x >> (16 - n)) : x); }
OP(rot) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = rotl(R(RA(w)).w[i], R(RB(w)).w[i]); R(RT(w)) = r; }
OP(roti) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = rotl(R(RA(w)).w[i], (u32)I7(w)); R(RT(w)) = r; }
OP(roth) { v128 r; for (int i = 0; i < 8; i++) r.h[i] = rotl16(R(RA(w)).h[i], R(RB(w)).h[i]); R(RT(w)) = r; }
OP(rothi) { v128 r; for (int i = 0; i < 8; i++) r.h[i] = rotl16(R(RA(w)).h[i], (u32)I7(w)); R(RT(w)) = r; }
OP(rotm) { v128 r; for (int i = 0; i < 4; i++) { u32 n = (0 - R(RB(w)).w[i]) & 63; r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] >> n; } R(RT(w)) = r; }
OP(rotmi) { u32 n = (0 - I7(w)) & 63; v128 r; for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] >> n; R(RT(w)) = r; }
OP(rothm) { v128 r; for (int i = 0; i < 8; i++) { u32 n = (0 - R(RB(w)).h[i]) & 31; r.h[i] = n > 15 ? 0 : R(RA(w)).h[i] >> n; } R(RT(w)) = r; }
OP(rothmi) { u32 n = (0 - I7(w)) & 31; v128 r; for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : R(RA(w)).h[i] >> n; R(RT(w)) = r; }
OP(rotma) { v128 r; for (int i = 0; i < 4; i++) { u32 n = (0 - R(RB(w)).w[i]) & 63; r.sw[i] = R(RA(w)).sw[i] >> (n > 31 ? 31 : n); } R(RT(w)) = r; }
OP(rotmai) { u32 n = (0 - I7(w)) & 63; v128 r; for (int i = 0; i < 4; i++) r.sw[i] = R(RA(w)).sw[i] >> (n > 31 ? 31 : n); R(RT(w)) = r; }
OP(rotmah) { v128 r; for (int i = 0; i < 8; i++) { u32 n = (0 - R(RB(w)).h[i]) & 31; r.sh[i] = (s16)(R(RA(w)).sh[i] >> (n > 15 ? 15 : n)); } R(RT(w)) = r; }
OP(rotmahi) { u32 n = (0 - I7(w)) & 31; v128 r; for (int i = 0; i < 8; i++) r.sh[i] = (s16)(R(RA(w)).sh[i] >> (n > 15 ? 15 : n)); R(RT(w)) = r; }

// ------------------------------------------------------------------ quadword shifts / rotates
FORCE_INLINE u128 qshl(u128 x, u32 n) { return n >= 128 ? 0 : x << n; }
FORCE_INLINE u128 qshr(u128 x, u32 n) { return n >= 128 ? 0 : x >> n; }
FORCE_INLINE u128 qrot(u128 x, u32 n) { n &= 127; return n ? (x << n) | (x >> (128 - n)) : x; }
OP(shlqbi) { setq(R(RT(w)), qshl(q(R(RA(w))), s.pref(RB(w)) & 7)); }
OP(shlqbii) { setq(R(RT(w)), qshl(q(R(RA(w))), (u32)I7(w) & 7)); }
OP(shlqby) { setq(R(RT(w)), qshl(q(R(RA(w))), (s.pref(RB(w)) & 31) * 8)); }
OP(shlqbyi) { setq(R(RT(w)), qshl(q(R(RA(w))), ((u32)I7(w) & 31) * 8)); }
OP(shlqbybi) { setq(R(RT(w)), qshl(q(R(RA(w))), ((s.pref(RB(w)) >> 3) & 31) * 8)); }
OP(rotqbi) { setq(R(RT(w)), qrot(q(R(RA(w))), s.pref(RB(w)) & 7)); }
OP(rotqbii) { setq(R(RT(w)), qrot(q(R(RA(w))), (u32)I7(w) & 7)); }
OP(rotqby) { setq(R(RT(w)), qrot(q(R(RA(w))), (s.pref(RB(w)) & 15) * 8)); }
OP(rotqbyi) { setq(R(RT(w)), qrot(q(R(RA(w))), ((u32)I7(w) & 15) * 8)); }
OP(rotqbybi) { setq(R(RT(w)), qrot(q(R(RA(w))), ((s.pref(RB(w)) >> 3) & 15) * 8)); }
OP(rotqmbi) { setq(R(RT(w)), qshr(q(R(RA(w))), (0 - s.pref(RB(w))) & 7)); }
OP(rotqmbii) { setq(R(RT(w)), qshr(q(R(RA(w))), (0 - (u32)I7(w)) & 7)); }
OP(rotqmby) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - s.pref(RB(w))) & 31) * 8)); }
OP(rotqmbyi) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - (u32)I7(w)) & 31) * 8)); }
OP(rotqmbybi) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - (s.pref(RB(w)) >> 3)) & 31) * 8)); }

// ------------------------------------------------------------------ compares
OP(ceq) { R(RT(w)).pi = _mm_cmpeq_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqh) { R(RT(w)).pi = _mm_cmpeq_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqb) { R(RT(w)).pi = _mm_cmpeq_epi8(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqi) { R(RT(w)).pi = _mm_cmpeq_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(ceqhi) { R(RT(w)).pi = _mm_cmpeq_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(ceqbi) { R(RT(w)).pi = _mm_cmpeq_epi8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(cgt) { R(RT(w)).pi = _mm_cmpgt_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgth) { R(RT(w)).pi = _mm_cmpgt_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgtb) { R(RT(w)).pi = _mm_cmpgt_epi8(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgti) { R(RT(w)).pi = _mm_cmpgt_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(cgthi) { R(RT(w)).pi = _mm_cmpgt_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(cgtbi) { R(RT(w)).pi = _mm_cmpgt_epi8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
FORCE_INLINE __m128i ucmpgt32(__m128i a, __m128i b) { __m128i s = _mm_set1_epi32((int)0x80000000); return _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
FORCE_INLINE __m128i ucmpgt16(__m128i a, __m128i b) { __m128i s = _mm_set1_epi16((short)0x8000); return _mm_cmpgt_epi16(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
FORCE_INLINE __m128i ucmpgt8(__m128i a, __m128i b) { __m128i s = _mm_set1_epi8((char)0x80); return _mm_cmpgt_epi8(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
OP(clgt) { R(RT(w)).pi = ucmpgt32(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgth) { R(RT(w)).pi = ucmpgt16(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgtb) { R(RT(w)).pi = ucmpgt8(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgti) { R(RT(w)).pi = ucmpgt32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(clgthi) { R(RT(w)).pi = ucmpgt16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(clgtbi) { R(RT(w)).pi = ucmpgt8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }

// halt-if (treated as fatal: indicates a failed SPU assertion)
template <class S> void halt(S& s, const char* what) { fatal("SPU %s: halt (%s) at %05x", s.name.c_str(), what, s.pc); }
OP(heq) { if (s.pref(RA(w)) == s.pref(RB(w))) halt(s, "heq"); }
OP(heqi) { if ((s32)s.pref(RA(w)) == I10(w)) halt(s, "heqi"); }
OP(hgt) { if ((s32)s.pref(RA(w)) > (s32)s.pref(RB(w))) halt(s, "hgt"); }
OP(hgti) { if ((s32)s.pref(RA(w)) > I10(w)) halt(s, "hgti"); }
OP(hlgt) { if (s.pref(RA(w)) > s.pref(RB(w))) halt(s, "hlgt"); }
OP(hlgti) { if (s.pref(RA(w)) > (u32)I10(w)) halt(s, "hlgti"); }

// ------------------------------------------------------------------ floating point
// SPU single precision is not IEEE: no infinities or NaNs (results saturate at +-FLT_MAX) and denormals are
// flushed to zero (FTZ/DAZ are set for SPU threads). Clamping +-inf keeps later 0 * x products at 0 instead of NaN.
FORCE_INLINE __m128 sat4(__m128 x) { return _mm_max_ps(_mm_min_ps(x, _mm_set1_ps(FLT_MAX)), _mm_set1_ps(-FLT_MAX)); }
OP(fa) { R(RT(w)).ps = sat4(_mm_add_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fs) { R(RT(w)).ps = sat4(_mm_sub_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fm) { R(RT(w)).ps = sat4(_mm_mul_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fma) { R(RRR_RT(w)).ps = sat4(_mm_fmadd_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fms) { R(RRR_RT(w)).ps = sat4(_mm_fmsub_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fnms) { R(RRR_RT(w)).ps = sat4(_mm_fnmadd_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fceq) { R(RT(w)).ps = _mm_cmpeq_ps(R(RA(w)).ps, R(RB(w)).ps); }
OP(fcgt) { R(RT(w)).ps = _mm_cmpgt_ps(R(RA(w)).ps, R(RB(w)).ps); }
FORCE_INLINE __m128 fabs4(__m128 x) { return _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
OP(fcmeq) { R(RT(w)).ps = _mm_cmpeq_ps(fabs4(R(RA(w)).ps), fabs4(R(RB(w)).ps)); }
OP(fcmgt) { R(RT(w)).ps = _mm_cmpgt_ps(fabs4(R(RA(w)).ps), fabs4(R(RB(w)).ps)); }
OP(frest) { R(RT(w)).ps = sat4(_mm_div_ps(_mm_set1_ps(1.0f), R(RA(w)).ps)); }   // 1/+-0 -> +-FLT_MAX
OP(frsqest) { R(RT(w)).ps = sat4(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(fabs4(R(RA(w)).ps)))); }
OP(fi) { R(RT(w)) = R(RB(w)); }   // estimates above are exact, interpolation is the identity
// 2^e: built from the exponent bits for normal results (the conversions run in inner loops)
FORCE_INLINE float pow2f(int e) {
    if (e >= -126 && e <= 127) { u32 bits = (u32)(e + 127) << 23; float f; memcpy(&f, &bits, 4); return f; }
    return std::ldexp(1.0f, e);
}
OP(csflt) { R(RT(w)).ps = _mm_mul_ps(_mm_cvtepi32_ps(R(RA(w)).pi), _mm_set1_ps(pow2f(-(int)(155 - I8(w))))); }
OP(cuflt) { float sc = pow2f(-(int)(155 - I8(w))); v128 r; for (int i = 0; i < 4; i++) r.f[i] = (float)R(RA(w)).w[i] * sc; R(RT(w)) = r; }
OP(cflts) {
    float sc = pow2f((int)(173 - I8(w))); v128 r;
    for (int i = 0; i < 4; i++) {
        double x = (double)R(RA(w)).f[i] * sc;
        r.sw[i] = std::isnan(x) ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (s32)x;
    }
    R(RT(w)) = r;
}
OP(cfltu) {
    float sc = pow2f((int)(173 - I8(w))); v128 r;
    for (int i = 0; i < 4; i++) {
        double x = (double)R(RA(w)).f[i] * sc;
        r.w[i] = std::isnan(x) || x <= 0 ? 0 : x >= 4294967295.0 ? UINT32_MAX : (u32)x;
    }
    R(RT(w)) = r;
}
// double precision: two BE doublewords; BE dword 0 = host d[1]
OP(dfa) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a += b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
OP(dfs) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a -= b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
OP(dfm) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a *= b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
template <int K, class S>
FORCE_INLINE void dfmx(S& s, u32 w) {   // K: 0 dfma, 1 dfms, 2 dfnms, 3 dfnma ; rt is also the addend
    v128 r;
    for (int i = 0; i < 2; i++) {
        double a, b, t;
        memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); memcpy(&t, &R(RT(w)).d[i], 8);
        double v = K == 0 ? std::fma(a, b, t) : K == 1 ? std::fma(a, b, -t) : K == 2 ? -std::fma(a, b, -t) : -std::fma(a, b, t);
        memcpy(&r.d[i], &v, 8);
    }
    R(RT(w)) = r;
}
template <int K, class S>
FORCE_INLINE void dfcmp(S& s, u32 w) {   // 0 eq, 1 gt, 2 mag eq, 3 mag gt
    v128 r;
    for (int i = 0; i < 2; i++) {
        double a, b;
        memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8);
        if (K >= 2) { a = std::fabs(a); b = std::fabs(b); }
        r.d[i] = ((K & 1) ? a > b : a == b) ? ~0ull : 0;
    }
    R(RT(w)) = r;
}
OP(dftsv) { R(RT(w)).pi = _mm_setzero_si128(); }
OP(fesd) {   // BE words 0,2 (floats) -> doubles
    v128 r;
    double hi = R(RA(w)).f[3], lo = R(RA(w)).f[1];
    memcpy(&r.d[1], &hi, 8); memcpy(&r.d[0], &lo, 8);
    R(RT(w)) = r;
}
OP(frds) {
    v128 r; r.pi = _mm_setzero_si128();
    double hi, lo; memcpy(&hi, &R(RA(w)).d[1], 8); memcpy(&lo, &R(RA(w)).d[0], 8);
    r.f[3] = (float)hi; r.f[1] = (float)lo;
    R(RT(w)) = r;
}
OP(fscrrd) { R(RT(w)).pi = _mm_setzero_si128(); }
OP(fscrwr) {}

// ------------------------------------------------------------------ branches (pc already points at this insn)
#define JUMP(t) do { s.pc = (t) & 0x3FFFC; return; } while (0)
// indirect branches carry interrupt enable (E, 0x10 in the RB field) / disable (D, 0x20) bits
template <class S> FORCE_INLINE void ints(S& s, u32 w) {
    u32 f = RB(w);
    if (f & 0x10) s.interrupts = true;
    if (f & 0x20) s.interrupts = false;
}
OP(br) { JUMP(s.pc + (I16(w) << 2)); }
OP(bra) { JUMP((u32)(I16(w) << 2)); }
OP(brsl) { u32 t = s.pc + (I16(w) << 2); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(brasl) { u32 t = (u32)(I16(w) << 2); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(brnz) { if (s.pref(RT(w)) != 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brz) { if (s.pref(RT(w)) == 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brhnz) { if ((s.pref(RT(w)) & 0xFFFF) != 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brhz) { if ((s.pref(RT(w)) & 0xFFFF) == 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(bi) { ints(s, w); JUMP(s.pref(RA(w))); }
OP(iret) { ints(s, w); JUMP(s.srr0); }
OP(bisl) { ints(s, w); u32 t = s.pref(RA(w)); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(bisled) { s.pc += 4; }   // no external events are ever pending
OP(biz) { if (s.pref(RT(w)) == 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(binz) { if (s.pref(RT(w)) != 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(bihz) { if ((s.pref(RT(w)) & 0xFFFF) == 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(bihnz) { if ((s.pref(RT(w)) & 0xFFFF) != 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(nop) {}

// ------------------------------------------------------------------ channels
enum {
    SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3, SPU_RdSigNotify2 = 4,
    SPU_WrDec = 7, SPU_RdDec = 8, SPU_RdEventMask = 11, SPU_RdMachStat = 13, SPU_WrSRR0 = 14, SPU_RdSRR0 = 15,
    MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18, MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21, MFC_WrTagMask = 22,
    MFC_WrTagUpdate = 23, MFC_RdTagStat = 24, MFC_RdListStallStat = 25, MFC_WrListStallAck = 26,
    MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28, SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
};
enum : u32 { EV_LR = 0x400, EV_TM = 0x20 };

u32 decrementer(SPUThread& s) { return s.dec_start - (u32)((time_usec() - s.dec_time) * 80); }

// reservation-lost detection: compare the reserved line with memory
bool reservation_lost(SPUThread& s) {
    return s.has_reservation && memcmp(gmem(s.res_addr), s.res_data, 128) != 0;
}

u32 read_event_stat(SPUThread& s, bool block) {
    for (;;) {
        if ((s.event_mask & EV_LR) && reservation_lost(s)) { s.event_stat |= EV_LR; s.has_reservation = false; }
        if ((s.event_mask & EV_TM) && (s32)decrementer(s) < 0) s.event_stat |= EV_TM;
        u32 v = s.event_stat & s.event_mask;
        if (v || !block) return v;
        sleep_usec(20);
    }
}

OP(rdch) {
    u32 ch = RA(w), v = 0;
    switch (ch) {
        case SPU_RdEventStat: v = read_event_stat(s, true); break;
        case SPU_RdSigNotify1: case SPU_RdSigNotify2: v = s.snr[ch - 3]; s.snr[ch - 3] = 0; break;
        case SPU_RdDec: v = decrementer(s); break;
        case SPU_RdEventMask: v = s.event_mask; break;
        case SPU_RdMachStat: v = (s.interrupts || s.report_ie) ? 1 : 0; break;   // bit 0: interrupts enabled
        case SPU_RdSRR0: v = s.srr0; break;
        case MFC_RdTagStat: v = s.tag_mask; break;          // all DMA completes synchronously
        case MFC_RdListStallStat: v = s.stall_stat; s.stall_stat = 0; break;
        case MFC_RdAtomicStat: v = s.atomic_stat; s.atomic_stat = 0; break;
        case SPU_RdInMbox: {
            std::unique_lock lk(s.mbox_m);
            if (!s.in_mbox_valid && s.event_completion_pending) {   // no PPU handler here: complete at once
                s.event_completion_pending = false;
                v = 0;
                break;
            }
            s.mbox_cv.wait(lk, [&] { return s.in_mbox_valid; });
            v = s.in_mbox;
            s.in_mbox_valid = false;
            s.mbox_cv.notify_all();
            break;
        }
        default: LOG_W("SPU %s: rdch %u at %05x", s.name.c_str(), ch, s.pc);
    }
    s.set_pref(RT(w), v);
}
OP(rchcnt) {
    u32 ch = RA(w), v = 1;
    switch (ch) {
        case SPU_RdEventStat: v = read_event_stat(s, false) ? 1 : 0; break;
        case SPU_RdInMbox: { std::lock_guard lk(s.mbox_m); v = s.in_mbox_valid; break; }
        case SPU_WrOutMbox: case SPU_WrOutIntrMbox: { std::lock_guard lk(s.mbox_m); v = !s.out_mbox_valid; break; }
        case SPU_RdSigNotify1: case SPU_RdSigNotify2: v = s.snr[ch - 3] != 0; break;
        case MFC_Cmd: v = 16; break;
        case MFC_RdListStallStat: v = s.stall_stat != 0; break;
        default: v = 1;
    }
    s.set_pref(RT(w), v);
}
OP(wrch) {
    u32 ch = RA(w), v = s.pref(RT(w));
    switch (ch) {
        case SPU_WrEventMask: s.event_mask = v; break;
        case SPU_WrEventAck: s.event_stat &= ~v; break;
        case SPU_WrDec: s.dec_start = v; s.dec_time = time_usec(); break;
        case SPU_WrSRR0: s.srr0 = v; break;
        case MFC_LSA: s.mfc_lsa = v; break;
        case MFC_EAH: s.mfc_eah = v; break;
        case MFC_EAL: s.mfc_eal = v; break;
        case MFC_Size: s.mfc_size = v; break;
        case MFC_TagID: s.mfc_tag = v; break;
        case MFC_Cmd: s.dma(v); break;
        case MFC_WrTagMask: s.tag_mask = v; break;
        case MFC_WrTagUpdate: break;
        case MFC_WrListStallAck:
            if (s.stalled[v & 31].active) s.run_list(v & 31);
            break;
        case SPU_WrOutIntrMbox:
            if (s.on_send_event) {   // handled like lv2 does for SPU threads: consume both mailboxes, reply
                u32 data1;
                {
                    std::lock_guard lk(s.mbox_m);
                    data1 = s.out_mbox_valid ? s.out_mbox : 0;
                    s.out_mbox_valid = false;
                }
                u32 r = s.on_send_event(s, v, data1);
                std::lock_guard lk(s.mbox_m);
                s.in_mbox = r;
                s.in_mbox_valid = true;
                s.event_completion_pending = s.event_needs_completion;
                s.mbox_cv.notify_all();
                break;
            }
            [[fallthrough]];
        case SPU_WrOutMbox: {
            static std::atomic<int> logged{0};
            if (logged++ < 30) LOG_I("SPU %s: wrch %u <- %08x at %05x", s.name.c_str(), ch, v, s.pc);
            std::unique_lock lk(s.mbox_m);
            s.mbox_cv.wait(lk, [&] { return !s.out_mbox_valid; });
            s.out_mbox = v;
            s.out_mbox_valid = true;
            s.mbox_cv.notify_all();
            break;
        }
        default: LOG_W("SPU %s: wrch %u <- %08x at %05x", s.name.c_str(), ch, v, s.pc);
    }
}
OP(mfspr) { s.set_pref(RT(w), 0); }
OP(mtspr) {}
OP(stop) {
    u32 code = w & 0x3FFF;
    if (!s.on_stop || !s.on_stop(s, code)) s.halted = true;
}
OP(stopd) {   // debug halt (failed assertions in SPU code): logged, it used to stop SPUs silently
    static std::atomic<int> n{0};
    if (n++ < 20) LOG_W("SPU %s: stopd at %05x (halted)", s.name.c_str(), s.pc);
    s.halted = true;
}
OP(invalid) { fatal("SPU %s: invalid instruction %08x at %05x", s.name.c_str(), w, s.pc); }


}  // namespace
