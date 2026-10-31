// AltiVec/VMX semantics for the Cell PPU.
//
// Register layout: a v128 holds the big-endian register byte-reversed, so element i of
// size s (n = 16/s elements, big-endian numbering) lives in host element n-1-i.
// Element-wise operations therefore map directly onto SSE.
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace vmx {

FORCE_INLINE __m128i rev_mask() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }

// BE element accessors
FORCE_INLINE u32& W(v128& v, int i) { return v.w[3 - i]; }
FORCE_INLINE u16& H(v128& v, int i) { return v.h[7 - i]; }
FORCE_INLINE u8& B(v128& v, int i) { return v.b[15 - i]; }
FORCE_INLINE u32 W(const v128& v, int i) { return v.w[3 - i]; }
FORCE_INLINE u16 H(const v128& v, int i) { return v.h[7 - i]; }
FORCE_INLINE u8 B(const v128& v, int i) { return v.b[15 - i]; }
FORCE_INLINE float F(const v128& v, int i) { return v.f[3 - i]; }

// ---------------------------------------------------------------- loads / stores
FORCE_INLINE void lvx(v128& d, u32 ea) {
    d.pi = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)gmem(ea & ~15u)), rev_mask());
}
FORCE_INLINE void stvx(const v128& s, u32 ea) {
    WATCH_STORE(ea & ~15u, s.d[1], 16);
    _mm_storeu_si128((__m128i*)gmem(ea & ~15u), _mm_shuffle_epi8(s.pi, rev_mask()));
}
FORCE_INLINE void lvsl(v128& d, u32 ea) { u32 sh = ea & 15; for (int i = 0; i < 16; i++) B(d, i) = (u8)(sh + i); }
FORCE_INLINE void lvsr(v128& d, u32 ea) { u32 sh = ea & 15; for (int i = 0; i < 16; i++) B(d, i) = (u8)(16 - sh + i); }
FORCE_INLINE void lvebx(v128& d, u32 ea) { B(d, ea & 15) = ld8(ea); }
FORCE_INLINE void lvehx(v128& d, u32 ea) { ea &= ~1u; H(d, (ea & 15) >> 1) = ld16(ea); }
FORCE_INLINE void lvewx(v128& d, u32 ea) { ea &= ~3u; W(d, (ea & 15) >> 2) = ld32(ea); }
FORCE_INLINE void stvebx(const v128& s, u32 ea) { st8(ea, B(s, ea & 15)); }
FORCE_INLINE void stvehx(const v128& s, u32 ea) { ea &= ~1u; st16(ea, H(s, (ea & 15) >> 1)); }
FORCE_INLINE void stvewx(const v128& s, u32 ea) { ea &= ~3u; st32(ea, W(s, (ea & 15) >> 2)); }
FORCE_INLINE void lvlx(v128& d, u32 ea) {
    u32 sh = ea & 15; v128 t; t.pi = _mm_setzero_si128();
    for (u32 i = 0; i < 16 - sh; i++) B(t, i) = ld8(ea + i);
    d = t;
}
FORCE_INLINE void lvrx(v128& d, u32 ea) {
    u32 sh = ea & 15; v128 t; t.pi = _mm_setzero_si128();
    for (u32 i = 0; i < sh; i++) B(t, 16 - sh + i) = ld8((ea & ~15u) + i);
    d = t;
}
FORCE_INLINE void stvlx(const v128& s, u32 ea) { u32 sh = ea & 15; for (u32 i = 0; i < 16 - sh; i++) st8(ea + i, B(s, i)); }
FORCE_INLINE void stvrx(const v128& s, u32 ea) { u32 sh = ea & 15; for (u32 i = 0; i < sh; i++) st8((ea & ~15u) + i, B(s, 16 - sh + i)); }

// ---------------------------------------------------------------- logical
FORCE_INLINE void vand(v128& d, const v128& a, const v128& b) { d.pi = _mm_and_si128(a.pi, b.pi); }
FORCE_INLINE void vandc(v128& d, const v128& a, const v128& b) { d.pi = _mm_andnot_si128(b.pi, a.pi); }
FORCE_INLINE void vor(v128& d, const v128& a, const v128& b) { d.pi = _mm_or_si128(a.pi, b.pi); }
FORCE_INLINE void vxor(v128& d, const v128& a, const v128& b) { d.pi = _mm_xor_si128(a.pi, b.pi); }
FORCE_INLINE void vnor(v128& d, const v128& a, const v128& b) { d.pi = _mm_xor_si128(_mm_or_si128(a.pi, b.pi), _mm_set1_epi32(-1)); }
FORCE_INLINE void vsel(v128& d, const v128& a, const v128& b, const v128& c) {
    d.pi = _mm_or_si128(_mm_andnot_si128(c.pi, a.pi), _mm_and_si128(c.pi, b.pi));
}

// ---------------------------------------------------------------- float
FORCE_INLINE void vaddfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_add_ps(a.ps, b.ps); }
FORCE_INLINE void vsubfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_sub_ps(a.ps, b.ps); }
FORCE_INLINE void vmaddfp(v128& d, const v128& a, const v128& b, const v128& c) { d.ps = _mm_fmadd_ps(a.ps, c.ps, b.ps); }
FORCE_INLINE void vnmsubfp(v128& d, const v128& a, const v128& b, const v128& c) { d.ps = _mm_fnmadd_ps(a.ps, c.ps, b.ps); }
FORCE_INLINE void vmaxfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_max_ps(a.ps, b.ps); }
FORCE_INLINE void vminfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_min_ps(a.ps, b.ps); }
FORCE_INLINE void vrefp(v128& d, const v128& b) { d.ps = _mm_div_ps(_mm_set1_ps(1.0f), b.ps); }
FORCE_INLINE void vrsqrtefp(v128& d, const v128& b) { d.ps = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(b.ps)); }
FORCE_INLINE void vexptefp(v128& d, const v128& b) { for (int i = 0; i < 4; i++) d.f[i] = std::exp2(b.f[i]); }
FORCE_INLINE void vlogefp(v128& d, const v128& b) { for (int i = 0; i < 4; i++) d.f[i] = std::log2(b.f[i]); }
FORCE_INLINE void vrfin(v128& d, const v128& b) { d.ps = _mm_round_ps(b.ps, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
FORCE_INLINE void vrfiz(v128& d, const v128& b) { d.ps = _mm_round_ps(b.ps, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); }
FORCE_INLINE void vrfip(v128& d, const v128& b) { d.ps = _mm_round_ps(b.ps, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC); }
FORCE_INLINE void vrfim(v128& d, const v128& b) { d.ps = _mm_round_ps(b.ps, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
FORCE_INLINE void vcfsx(v128& d, const v128& b, u32 sh) { d.ps = _mm_mul_ps(_mm_cvtepi32_ps(b.pi), _mm_set1_ps(1.0f / (float)(1u << sh))); }
FORCE_INLINE void vcfux(v128& d, const v128& b, u32 sh) { for (int i = 0; i < 4; i++) d.f[i] = (float)b.w[i] / (float)(1u << sh); }
FORCE_INLINE void vctsxs(v128& d, const v128& b, u32 sh) {
    for (int i = 0; i < 4; i++) {
        float x = b.f[i] * (float)(1u << sh);
        d.sw[i] = std::isnan(x) ? 0 : x >= 2147483647.0f ? INT32_MAX : x <= -2147483648.0f ? INT32_MIN : (s32)x;
    }
}
FORCE_INLINE void vctuxs(v128& d, const v128& b, u32 sh) {
    for (int i = 0; i < 4; i++) {
        float x = b.f[i] * (float)(1u << sh);
        d.w[i] = std::isnan(x) || x <= 0 ? 0 : x >= 4294967295.0f ? UINT32_MAX : (u32)x;
    }
}

// ---------------------------------------------------------------- compares
FORCE_INLINE void vcmpeqfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_cmpeq_ps(a.ps, b.ps); }
FORCE_INLINE void vcmpgtfp(v128& d, const v128& a, const v128& b) { d.ps = _mm_cmpgt_ps(a.ps, b.ps); }
FORCE_INLINE void vcmpgefp(v128& d, const v128& a, const v128& b) { d.ps = _mm_cmpge_ps(a.ps, b.ps); }
FORCE_INLINE void vcmpbfp(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 4; i++) {
        u32 r = 0;
        if (!(a.f[i] <= b.f[i])) r |= 0x80000000u;
        if (!(a.f[i] >= -b.f[i])) r |= 0x40000000u;
        d.w[i] = r;
    }
}
FORCE_INLINE void vcmpequw(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpeq_epi32(a.pi, b.pi); }
FORCE_INLINE void vcmpequh(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpeq_epi16(a.pi, b.pi); }
FORCE_INLINE void vcmpequb(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpeq_epi8(a.pi, b.pi); }
FORCE_INLINE void vcmpgtsw(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpgt_epi32(a.pi, b.pi); }
FORCE_INLINE void vcmpgtsh(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpgt_epi16(a.pi, b.pi); }
FORCE_INLINE void vcmpgtsb(v128& d, const v128& a, const v128& b) { d.pi = _mm_cmpgt_epi8(a.pi, b.pi); }
FORCE_INLINE void vcmpgtuw(v128& d, const v128& a, const v128& b) {
    __m128i s = _mm_set1_epi32((int)0x80000000);
    d.pi = _mm_cmpgt_epi32(_mm_xor_si128(a.pi, s), _mm_xor_si128(b.pi, s));
}
FORCE_INLINE void vcmpgtuh(v128& d, const v128& a, const v128& b) {
    __m128i s = _mm_set1_epi16((short)0x8000);
    d.pi = _mm_cmpgt_epi16(_mm_xor_si128(a.pi, s), _mm_xor_si128(b.pi, s));
}
FORCE_INLINE void vcmpgtub(v128& d, const v128& a, const v128& b) {
    __m128i s = _mm_set1_epi8((char)0x80);
    d.pi = _mm_cmpgt_epi8(_mm_xor_si128(a.pi, s), _mm_xor_si128(b.pi, s));
}
// Record form: CR6 = [all true, 0, all false, 0]
FORCE_INLINE void set_cr6(PPUContext& c, const v128& d) {
    bool all = d.d[0] == ~0ull && d.d[1] == ~0ull;
    bool none = d.d[0] == 0 && d.d[1] == 0;
    c.cr[24] = all; c.cr[25] = 0; c.cr[26] = none; c.cr[27] = 0;
}

// ---------------------------------------------------------------- integer
FORCE_INLINE void vadduwm(v128& d, const v128& a, const v128& b) { d.pi = _mm_add_epi32(a.pi, b.pi); }
FORCE_INLINE void vadduhm(v128& d, const v128& a, const v128& b) { d.pi = _mm_add_epi16(a.pi, b.pi); }
FORCE_INLINE void vaddubm(v128& d, const v128& a, const v128& b) { d.pi = _mm_add_epi8(a.pi, b.pi); }
FORCE_INLINE void vsubuwm(v128& d, const v128& a, const v128& b) { d.pi = _mm_sub_epi32(a.pi, b.pi); }
FORCE_INLINE void vsubuhm(v128& d, const v128& a, const v128& b) { d.pi = _mm_sub_epi16(a.pi, b.pi); }
FORCE_INLINE void vsububm(v128& d, const v128& a, const v128& b) { d.pi = _mm_sub_epi8(a.pi, b.pi); }
FORCE_INLINE void vaddshs(v128& d, const v128& a, const v128& b) { d.pi = _mm_adds_epi16(a.pi, b.pi); }
FORCE_INLINE void vsubshs(v128& d, const v128& a, const v128& b) { d.pi = _mm_subs_epi16(a.pi, b.pi); }
FORCE_INLINE void vaddsws(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 4; i++) { s64 r = (s64)a.sw[i] + b.sw[i]; d.sw[i] = (s32)std::clamp<s64>(r, INT32_MIN, INT32_MAX); }
}
FORCE_INLINE void vsubsws(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 4; i++) { s64 r = (s64)a.sw[i] - b.sw[i]; d.sw[i] = (s32)std::clamp<s64>(r, INT32_MIN, INT32_MAX); }
}
FORCE_INLINE void vmaxsw(v128& d, const v128& a, const v128& b) { d.pi = _mm_max_epi32(a.pi, b.pi); }
FORCE_INLINE void vminsw(v128& d, const v128& a, const v128& b) { d.pi = _mm_min_epi32(a.pi, b.pi); }
FORCE_INLINE void vmaxsh(v128& d, const v128& a, const v128& b) { d.pi = _mm_max_epi16(a.pi, b.pi); }
FORCE_INLINE void vminsh(v128& d, const v128& a, const v128& b) { d.pi = _mm_min_epi16(a.pi, b.pi); }
FORCE_INLINE void vavgub(v128& d, const v128& a, const v128& b) { d.pi = _mm_avg_epu8(a.pi, b.pi); }
FORCE_INLINE void vslw(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 4; i++) d.w[i] = a.w[i] << (b.w[i] & 31); }
FORCE_INLINE void vsrw(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 4; i++) d.w[i] = a.w[i] >> (b.w[i] & 31); }
FORCE_INLINE void vsraw(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 4; i++) d.sw[i] = a.sw[i] >> (b.w[i] & 31); }
FORCE_INLINE void vslh(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 8; i++) d.h[i] = (u16)(a.h[i] << (b.h[i] & 15)); }
FORCE_INLINE void vsrh(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 8; i++) d.h[i] = a.h[i] >> (b.h[i] & 15); }
FORCE_INLINE void vsrah(v128& d, const v128& a, const v128& b) { for (int i = 0; i < 8; i++) d.sh[i] = a.sh[i] >> (b.h[i] & 15); }
FORCE_INLINE void vrlw(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 4; i++) { u32 n = b.w[i] & 31; d.w[i] = n ? (a.w[i] << n) | (a.w[i] >> (32 - n)) : a.w[i]; }
}
FORCE_INLINE void vrlh(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 8; i++) { u32 n = b.h[i] & 15; d.h[i] = (u16)(n ? (a.h[i] << n) | (a.h[i] >> (16 - n)) : a.h[i]); }
}
FORCE_INLINE void vrlb(v128& d, const v128& a, const v128& b) {
    for (int i = 0; i < 16; i++) { u32 n = b.b[i] & 7; d.b[i] = (u8)(n ? (a.b[i] << n) | (a.b[i] >> (8 - n)) : a.b[i]); }
}
// whole-register shifts (by bits / octets), shift amount in the low-order BE byte of b
FORCE_INLINE void vsl(v128& d, const v128& a, const v128& b) {
    u32 n = b.b[0] & 7; unsigned __int128 x; memcpy(&x, &a, 16); x <<= n; memcpy(&d, &x, 16);
}
FORCE_INLINE void vsr(v128& d, const v128& a, const v128& b) {
    u32 n = b.b[0] & 7; unsigned __int128 x; memcpy(&x, &a, 16); x >>= n; memcpy(&d, &x, 16);
}
FORCE_INLINE void vslo(v128& d, const v128& a, const v128& b) {
    u32 n = (b.b[0] >> 3) & 15; unsigned __int128 x; memcpy(&x, &a, 16); x = n ? x << (n * 8) : x; memcpy(&d, &x, 16);
}
FORCE_INLINE void vsro(v128& d, const v128& a, const v128& b) {
    u32 n = (b.b[0] >> 3) & 15; unsigned __int128 x; memcpy(&x, &a, 16); x = n ? x >> (n * 8) : x; memcpy(&d, &x, 16);
}

// ---------------------------------------------------------------- multiply / sum
FORCE_INLINE void vmulouh(v128& d, const v128& a, const v128& b) {  // odd BE halfwords
    v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)H(a, 2 * i + 1) * H(b, 2 * i + 1); d = t;
}
FORCE_INLINE void vmuleuh(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)H(a, 2 * i) * H(b, 2 * i); d = t;
}
FORCE_INLINE void vmulosh(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)((s32)(s16)H(a, 2 * i + 1) * (s16)H(b, 2 * i + 1)); d = t;
}
FORCE_INLINE void vmulesh(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)((s32)(s16)H(a, 2 * i) * (s16)H(b, 2 * i)); d = t;
}
FORCE_INLINE void vmsumuhm(v128& d, const v128& a, const v128& b, const v128& c) {
    v128 t;
    for (int i = 0; i < 4; i++)
        W(t, i) = (u32)H(a, 2 * i) * H(b, 2 * i) + (u32)H(a, 2 * i + 1) * H(b, 2 * i + 1) + W(c, i);
    d = t;
}
FORCE_INLINE void vmsumshm(v128& d, const v128& a, const v128& b, const v128& c) {
    v128 t;
    for (int i = 0; i < 4; i++)
        W(t, i) = (u32)((s32)(s16)H(a, 2 * i) * (s16)H(b, 2 * i) + (s32)(s16)H(a, 2 * i + 1) * (s16)H(b, 2 * i + 1) + (s32)W(c, i));
    d = t;
}
FORCE_INLINE void vmladduhm(v128& d, const v128& a, const v128& b, const v128& c) {
    d.pi = _mm_add_epi16(_mm_mullo_epi16(a.pi, b.pi), c.pi);
}
FORCE_INLINE void vmhaddshs(v128& d, const v128& a, const v128& b, const v128& c) {
    for (int i = 0; i < 8; i++) d.sh[i] = (s16)std::clamp<s32>(((s32)a.sh[i] * b.sh[i] >> 15) + c.sh[i], INT16_MIN, INT16_MAX);
}
FORCE_INLINE void vsumsws(v128& d, const v128& a, const v128& b) {
    s64 s = (s64)(s32)W(b, 3);
    for (int i = 0; i < 4; i++) s += (s32)W(a, i);
    v128 t; t.pi = _mm_setzero_si128();
    W(t, 3) = (u32)(s32)std::clamp<s64>(s, INT32_MIN, INT32_MAX);
    d = t;
}

// ---------------------------------------------------------------- permute / merge / splat
FORCE_INLINE void vperm(v128& d, const v128& a, const v128& b, const v128& c) {
    v128 t;
    for (int i = 0; i < 16; i++) { u32 k = B(c, i) & 31; B(t, i) = k < 16 ? B(a, k) : B(b, k - 16); }
    d = t;
}
FORCE_INLINE void vsldoi(v128& d, const v128& a, const v128& b, u32 sh) {
    v128 t;
    for (int i = 0; i < 16; i++) { u32 k = i + sh; B(t, i) = k < 16 ? B(a, k) : B(b, k - 16); }
    d = t;
}
FORCE_INLINE void vmrghw(v128& d, const v128& a, const v128& b) {
    v128 t; W(t, 0) = W(a, 0); W(t, 1) = W(b, 0); W(t, 2) = W(a, 1); W(t, 3) = W(b, 1); d = t;
}
FORCE_INLINE void vmrglw(v128& d, const v128& a, const v128& b) {
    v128 t; W(t, 0) = W(a, 2); W(t, 1) = W(b, 2); W(t, 2) = W(a, 3); W(t, 3) = W(b, 3); d = t;
}
FORCE_INLINE void vmrghh(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) { H(t, 2 * i) = H(a, i); H(t, 2 * i + 1) = H(b, i); } d = t;
}
FORCE_INLINE void vmrglh(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) { H(t, 2 * i) = H(a, i + 4); H(t, 2 * i + 1) = H(b, i + 4); } d = t;
}
FORCE_INLINE void vmrghb(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 8; i++) { B(t, 2 * i) = B(a, i); B(t, 2 * i + 1) = B(b, i); } d = t;
}
FORCE_INLINE void vmrglb(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 8; i++) { B(t, 2 * i) = B(a, i + 8); B(t, 2 * i + 1) = B(b, i + 8); } d = t;
}
FORCE_INLINE void vspltw(v128& d, const v128& b, u32 i) { d.pi = _mm_set1_epi32((int)W(b, i & 3)); }
FORCE_INLINE void vsplth(v128& d, const v128& b, u32 i) { d.pi = _mm_set1_epi16((short)H(b, i & 7)); }
FORCE_INLINE void vspltb(v128& d, const v128& b, u32 i) { d.pi = _mm_set1_epi8((char)B(b, i & 15)); }
FORCE_INLINE void vspltisw(v128& d, s32 v) { d.pi = _mm_set1_epi32(v); }
FORCE_INLINE void vspltish(v128& d, s32 v) { d.pi = _mm_set1_epi16((short)v); }
FORCE_INLINE void vspltisb(v128& d, s32 v) { d.pi = _mm_set1_epi8((char)v); }

// ---------------------------------------------------------------- pack / unpack
template <typename T> FORCE_INLINE T sat(s64 v) { return (T)std::clamp<s64>(v, std::numeric_limits<T>::min(), std::numeric_limits<T>::max()); }
FORCE_INLINE void vpkswss(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) { H(t, i) = (u16)sat<s16>((s32)W(a, i)); H(t, i + 4) = (u16)sat<s16>((s32)W(b, i)); } d = t;
}
FORCE_INLINE void vpkswus(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) { H(t, i) = sat<u16>((s32)W(a, i)); H(t, i + 4) = sat<u16>((s32)W(b, i)); } d = t;
}
FORCE_INLINE void vpkshss(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 8; i++) { B(t, i) = (u8)sat<s8>((s16)H(a, i)); B(t, i + 8) = (u8)sat<s8>((s16)H(b, i)); } d = t;
}
FORCE_INLINE void vpkshus(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 8; i++) { B(t, i) = sat<u8>((s16)H(a, i)); B(t, i + 8) = sat<u8>((s16)H(b, i)); } d = t;
}
FORCE_INLINE void vpkuwum(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 4; i++) { H(t, i) = (u16)W(a, i); H(t, i + 4) = (u16)W(b, i); } d = t;
}
FORCE_INLINE void vpkuhum(v128& d, const v128& a, const v128& b) {
    v128 t; for (int i = 0; i < 8; i++) { B(t, i) = (u8)H(a, i); B(t, i + 8) = (u8)H(b, i); } d = t;
}
FORCE_INLINE void vupkhsh(v128& d, const v128& b) { v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)(s32)(s16)H(b, i); d = t; }
FORCE_INLINE void vupklsh(v128& d, const v128& b) { v128 t; for (int i = 0; i < 4; i++) W(t, i) = (u32)(s32)(s16)H(b, i + 4); d = t; }
FORCE_INLINE void vupkhsb(v128& d, const v128& b) { v128 t; for (int i = 0; i < 8; i++) H(t, i) = (u16)(s16)(s8)B(b, i); d = t; }
FORCE_INLINE void vupklsb(v128& d, const v128& b) { v128 t; for (int i = 0; i < 8; i++) H(t, i) = (u16)(s16)(s8)B(b, i + 8); d = t; }

// ---------------------------------------------------------------- VSCR
FORCE_INLINE void mfvscr(PPUContext& c, v128& d) { d.pi = _mm_setzero_si128(); W(d, 3) = c.vscr; }
FORCE_INLINE void mtvscr(PPUContext& c, const v128& b) { c.vscr = W(b, 3); }

}  // namespace vmx
