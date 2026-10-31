// Cell SPU interpreter.
//
// Register layout: byte-reversed big-endian quadword (host byte j = BE byte 15-j). With this layout
// the host little-endian 128-bit integer equals the big-endian quadword value, so whole-register
// shifts/rotates are plain __int128 operations and element-wise ops map onto SSE directly.
#include "spu.h"

#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <vector>

#include "spu_ops.h"

#include <mutex>
#include <set>

namespace {
using Handler = void (*)(SPUThread&, u32);
Handler g_table[2048];

// ------------------------------------------------------------------ decode table
void build_table() {
    for (auto& h : g_table) h = op_invalid<SPUThread>;
    auto rr = [](u32 op, Handler h) { g_table[op] = h; };
    auto ri8 = [](u32 op, Handler h) { for (u32 i = 0; i < 2; i++) g_table[(op << 1) | i] = h; };
    auto ri16 = [](u32 op, Handler h) { for (u32 i = 0; i < 4; i++) g_table[(op << 2) | i] = h; };
    auto ri10 = [](u32 op, Handler h) { for (u32 i = 0; i < 8; i++) g_table[(op << 3) | i] = h; };
    auto ri18 = [](u32 op, Handler h) { for (u32 i = 0; i < 16; i++) g_table[(op << 4) | i] = h; };
    auto rrr = [](u32 op, Handler h) { for (u32 i = 0; i < 128; i++) g_table[(op << 7) | i] = h; };

    rr(0x000, op_stop); rr(0x001, op_nop); rr(0x002, op_nop); rr(0x003, op_nop); rr(0x00C, op_mfspr);
    rr(0x00D, op_rdch); rr(0x00F, op_rchcnt); rr(0x040, op_sf); rr(0x041, op_or_); rr(0x042, op_bg);
    rr(0x048, op_sfh); rr(0x049, op_nor); rr(0x053, op_absdb); rr(0x058, op_rot); rr(0x059, op_rotm);
    rr(0x05A, op_rotma); rr(0x05B, op_shl); rr(0x05C, op_roth); rr(0x05D, op_rothm); rr(0x05E, op_rotmah);
    rr(0x05F, op_shlh); rr(0x078, op_roti); rr(0x079, op_rotmi); rr(0x07A, op_rotmai); rr(0x07B, op_shli);
    rr(0x07C, op_rothi); rr(0x07D, op_rothmi); rr(0x07E, op_rotmahi); rr(0x07F, op_shlhi); rr(0x0C0, op_a);
    rr(0x0C1, op_and_); rr(0x0C2, op_cg); rr(0x0C8, op_ah); rr(0x0C9, op_nand); rr(0x0D3, op_avgb);
    rr(0x10C, op_mtspr); rr(0x10D, op_wrch); rr(0x128, op_biz); rr(0x129, op_binz); rr(0x12A, op_bihz);
    rr(0x12B, op_bihnz); rr(0x140, op_stopd); rr(0x144, op_stqx); rr(0x1A8, op_bi); rr(0x1A9, op_bisl);
    rr(0x1AA, op_iret); rr(0x1AB, op_bisled); rr(0x1AC, op_nop); rr(0x1B0, op_gb); rr(0x1B1, op_gbh);
    rr(0x1B2, op_gbb); rr(0x1B4, op_fsm); rr(0x1B5, op_fsmh); rr(0x1B6, op_fsmb); rr(0x1B8, op_frest);
    rr(0x1B9, op_frsqest); rr(0x1C4, op_lqx); rr(0x1CC, op_rotqbybi); rr(0x1CD, op_rotqmbybi);
    rr(0x1CF, op_shlqbybi); rr(0x1D4, op_cbx); rr(0x1D5, op_chx); rr(0x1D6, op_cwx); rr(0x1D7, op_cdx);
    rr(0x1D8, op_rotqbi); rr(0x1D9, op_rotqmbi); rr(0x1DB, op_shlqbi); rr(0x1DC, op_rotqby); rr(0x1DD, op_rotqmby);
    rr(0x1DF, op_shlqby); rr(0x1F0, op_orx); rr(0x1F4, op_cbd); rr(0x1F5, op_chd); rr(0x1F6, op_cwd); rr(0x1F7, op_cdd);
    rr(0x1F8, op_rotqbii); rr(0x1F9, op_rotqmbii); rr(0x1FB, op_shlqbii); rr(0x1FC, op_rotqbyi);
    rr(0x1FD, op_rotqmbyi); rr(0x1FF, op_shlqbyi); rr(0x201, op_nop); rr(0x240, op_cgt); rr(0x241, op_xor_);
    rr(0x248, op_cgth); rr(0x249, op_eqv); rr(0x250, op_cgtb); rr(0x253, op_sumb); rr(0x258, op_hgt);
    rr(0x2A5, op_clz); rr(0x2A6, op_xswd); rr(0x2AE, op_xshw); rr(0x2B4, op_cntb); rr(0x2B6, op_xsbh);
    rr(0x2C0, op_clgt); rr(0x2C1, op_andc); rr(0x2C2, op_fcgt); rr(0x2C3, dfcmp<1>); rr(0x2C4, op_fa);
    rr(0x2C5, op_fs); rr(0x2C6, op_fm); rr(0x2C8, op_clgth); rr(0x2C9, op_orc); rr(0x2CA, op_fcmgt);
    rr(0x2CB, dfcmp<3>); rr(0x2CC, op_dfa); rr(0x2CD, op_dfs); rr(0x2CE, op_dfm); rr(0x2D0, op_clgtb);
    rr(0x2D8, op_hlgt); rr(0x35C, dfmx<0>); rr(0x35D, dfmx<1>); rr(0x35E, dfmx<2>); rr(0x35F, dfmx<3>);
    rr(0x3C0, op_ceq); rr(0x3CE, op_mpyhhu); rr(0x340, op_addx); rr(0x341, op_sfx); rr(0x342, op_cgx);
    rr(0x343, op_bgx); rr(0x346, op_mpyhha); rr(0x34E, op_mpyhhau); rr(0x398, op_fscrrd); rr(0x3B8, op_fesd);
    rr(0x3B9, op_frds); rr(0x3BA, op_fscrwr); rr(0x3BF, op_dftsv); rr(0x3C2, op_fceq); rr(0x3C3, dfcmp<0>);
    rr(0x3C4, op_mpy); rr(0x3C5, op_mpyh); rr(0x3C6, op_mpyhh); rr(0x3C7, op_mpys); rr(0x3C8, op_ceqh);
    rr(0x3CA, op_fcmeq); rr(0x3CB, dfcmp<2>); rr(0x3CC, op_mpyu); rr(0x3D0, op_ceqb); rr(0x3D4, op_fi);
    rr(0x3D8, op_heq);

    ri8(0x1D8, op_cflts); ri8(0x1D9, op_cfltu); ri8(0x1DA, op_csflt); ri8(0x1DB, op_cuflt);

    ri16(0x064, op_br); ri16(0x066, op_brsl); ri16(0x060, op_bra); ri16(0x062, op_brasl); ri16(0x042, op_brnz);
    ri16(0x040, op_brz); ri16(0x046, op_brhnz); ri16(0x044, op_brhz); ri16(0x061, op_lqa); ri16(0x067, op_lqr);
    ri16(0x041, op_stqa); ri16(0x047, op_stqr); ri16(0x083, op_ilh); ri16(0x082, op_ilhu); ri16(0x081, op_il);
    ri16(0x0C1, op_iohl); ri16(0x065, op_fsmbi);

    ri10(0x34, op_lqd); ri10(0x24, op_stqd); ri10(0x1C, op_ai); ri10(0x1D, op_ahi); ri10(0x0C, op_sfi);
    ri10(0x0D, op_sfhi); ri10(0x14, op_andi); ri10(0x15, op_andhi); ri10(0x16, op_andbi); ri10(0x04, op_ori);
    ri10(0x05, op_orhi); ri10(0x06, op_orbi); ri10(0x44, op_xori); ri10(0x45, op_xorhi); ri10(0x46, op_xorbi);
    ri10(0x74, op_mpyi); ri10(0x75, op_mpyui); ri10(0x7C, op_ceqi); ri10(0x7D, op_ceqhi); ri10(0x7E, op_ceqbi);
    ri10(0x4C, op_cgti); ri10(0x4D, op_cgthi); ri10(0x4E, op_cgtbi); ri10(0x5C, op_clgti); ri10(0x5D, op_clgthi);
    ri10(0x5E, op_clgtbi); ri10(0x7F, op_heqi); ri10(0x4F, op_hgti); ri10(0x5F, op_hlgti);

    ri18(0x21, op_ila); ri18(0x08, op_nop); ri18(0x09, op_nop);

    rrr(0x8, op_selb); rrr(0xB, op_shufb); rrr(0xC, op_mpya); rrr(0xD, op_fnms); rrr(0xE, op_fma); rrr(0xF, op_fms);
}

struct TableInit { TableInit() { build_table(); } } g_table_init;

// branches/stop manage pc themselves; everything else advances by 4
bool is_control(Handler h) {
    using T = SPUThread;
    return h == op_br<T> || h == op_bra<T> || h == op_brsl<T> || h == op_brasl<T> || h == op_brnz<T> || h == op_brz<T> ||
           h == op_brhnz<T> || h == op_brhz<T> || h == op_bi<T> || h == op_iret<T> || h == op_bisl<T> ||
           h == op_bisled<T> || h == op_biz<T> || h == op_binz<T> || h == op_bihz<T> || h == op_bihnz<T>;
}
bool g_control[2048];
struct ControlInit {
    ControlInit() { for (int i = 0; i < 2048; i++) g_control[i] = is_control(g_table[i]); }
} g_control_init;

}  // namespace

// ------------------------------------------------------------------ SPUThread
SPUThread::SPUThread() {
#ifdef _WIN32
    ls = static_cast<u8*>(_aligned_malloc(0x40000, 128));
#else
    ls = static_cast<u8*>(std::aligned_alloc(128, 0x40000));
#endif
    memset(ls, 0, 0x40000);
    memset(gpr, 0, sizeof(gpr));
}
SPUThread::~SPUThread() {
#ifdef _WIN32
    _aligned_free(ls);
#else
    std::free(ls);
#endif
}

// debug: F1CE_SPU_PC_LOG=<pc>[,<pc>...] logs r2, r3 and the quadword at r2 whenever an SPU executes those pcs
static std::vector<u8> g_pc_log = [] {
    std::vector<u8> v;
    if (const char* e = getenv("F1CE_SPU_PC_LOG")) {
        v.assign(0x10000, 0);
        for (const char* p = e; *p;) { v[((u32)strtoul(p, nullptr, 16) & 0x3FFFC) >> 2] = 1; while (*p && *p != ',') p++; if (*p) p++; }
    }
    return v;
}();
static void pc_log(SPUThread& s) {
    // F1CE_SPU_PC_LOG_R3=<r3>[,...]: only report when r3 matches, preceded by this thread's recent trace lines
    static const std::vector<u32> r3f = [] {
        std::vector<u32> v;
        if (const char* e = getenv("F1CE_SPU_PC_LOG_R3"))
            for (const char* p = e; *p;) { v.push_back((u32)strtoul(p, nullptr, 16)); while (*p && *p != ',') p++; if (*p) p++; }
        return v;
    }();
    u32 r2 = s.pref(2) & 0x3FFF0;
    char line[260];
    u32 m = s.pref(14) & ~15u;
    bool mem_ok = m >= 0x30000000 && m < 0x3A000000;   // user heap range in use (avoid probing unmapped pages)
    snprintf(line, sizeof(line), "%05x: r2=%05x r3=%05x r14=%05x r80=%05x [r2]=%08x %08x %08x %08x mem[r14]=%08x %08x %08x %08x", s.pc,
             s.pref(2), s.pref(3), s.pref(14), s.pref(80), s.ls32(r2), s.ls32(r2 + 4), s.ls32(r2 + 8), s.ls32(r2 + 12),
             mem_ok ? ld32(m) : 0, mem_ok ? ld32(m + 4) : 0, mem_ok ? ld32(m + 8) : 0, mem_ok ? ld32(m + 12) : 0);
    if (!r3f.empty()) {
        thread_local std::string ring[12];
        thread_local u32 pos = 0;
        ring[pos++ % 12] = line;
        if (std::find(r3f.begin(), r3f.end(), s.pref(3)) == r3f.end()) return;
        // F1CE_AUTO_WATCH: retarget the memory watchpoint to the source EA (r14 of the preceding component load) of
        // the first matching patch, so the writers of the next frames' values get logged
        if (getenv("F1CE_AUTO_WATCH")) {
            extern u32 g_watch_line;
            static bool set = false;
            // the preceding component load is the last ring entry before this one: take its r14 (source EA)
            const std::string& prev = ring[(pos + 10) % 12];
            size_t q = prev.find("r14=");
            if (!set && q != std::string::npos) {
                u32 src = (u32)strtoul(prev.c_str() + q + 4, nullptr, 16);
                if (src >= 0x38000000 && src < 0x39000000) {
                    set = true;
                    g_watch_line = src & ~127u;
                    LOG_I("auto watch: watching %08x (source of a constant patched into %05x)", g_watch_line, s.pref(3));
                }
            }
        }
        static std::atomic<int> hits{0};
        if (hits++ > 40) return;
        for (u32 k = 0; k < 12; k++) if (!ring[(pos + k) % 12].empty()) LOG_I("pc log %s %s", s.name.c_str(), ring[(pos + k) % 12].c_str());
        LOG_I("pc log ----");
        for (auto& r : ring) r.clear();
        return;
    }
    static std::atomic<int> n{0};
    if (n++ > 3000) return;
    LOG_I("pc log %s %05x: r2=%05x r3=%05x r14=%05x r80=%05x [r2]=%08x %08x %08x %08x", s.name.c_str(), s.pc, s.pref(2), s.pref(3),
          s.pref(14), s.pref(80), s.ls32(r2), s.ls32(r2 + 4), s.ls32(r2 + 8), s.ls32(r2 + 12));
}

void SPUThread::flush_blocks() {
    for (auto& p : block_pages) p.reset();
    cluster_ver.clear();
    cluster_ok.clear();
    memset(fast, 0xFF, sizeof(fast));   // (no pc matches)
    std::fill(std::begin(region_writes), std::end(region_writes), 1ull);
    memset(region_links, 0, sizeof(region_links));
    blocks_on = true;
    memset(line_code, 0, sizeof(line_code));
}

// decodes the basic block at pc (reusing the slot's previous block object): up to 256 instructions, ending
// after a branch or stop
// Each slot keeps up to 8 variants: job code DMA'd into a shared LS buffer alternates between a few binaries, and a
// variant whose words still match the LS is revalidated instead of decoded again.
SPUThread::Block* SPUThread::build_block(u32 pc) {
    auto& slot = block_slot(pc);
    auto matches = [&](const Block& v) {
        if (v.start != pc || v.ops.empty()) return false;
        for (u32 i = 0; i < v.n; i++)
            if (ls32(pc + 4 * i) != v.ops[i].w) return false;
        return true;
    };
    // native code: the block's cluster when all of the cluster's words are present, else its own translation; the
    // block then guards the cluster's whole span
    auto mark = [&](Block* v) {
        static const bool no_cluster = getenv("F1CE_SPU_NOCLUSTER") != nullptr;   // debug: single-block translations
        v->native = v->aot >= 0 ? g_spu_aot[v->aot].fn : nullptr;
        v->count_n = v->n;
        v->line0 = (pc & 0x3FFFF) >> 7;
        v->line1 = ((pc + (v->n - 1) * 4) & 0x3FFFF) >> 7;
        v->lines = nullptr;
        v->nlines = 0;
        v->cluster = -1;
        if (v->aot >= 0 && g_spu_aot[v->aot].cluster >= 0 && !no_cluster) {
            u32 ci = (u32)g_spu_aot[v->aot].cluster;
            const SpuAotCluster& c = g_spu_aot_clusters[ci];
            if (cluster_ver.empty()) {
                cluster_ver.assign(g_spu_aot_cluster_count, 0);
                cluster_ok.assign(g_spu_aot_cluster_count, 0);
            }
            u64 ver = lines_ver(c.lines, c.nlines) + 1;   // + 1: 0 means never verified
            bool ok = cluster_ver[ci] == ver;
            if (!ok) {
                u64 h = 1469598103934665603ull;
                for (u32 m = 0; m < c.count; m++)
                    for (u32 i = 0, a = c.members[m * 2]; i < c.members[m * 2 + 1]; i++, a += 4) {
                        u32 w = ls32(a);
                        for (int k = 0; k < 4; k++) { h ^= (w >> (k * 8)) & 0xFF; h *= 1099511628211ull; }
                    }
                ok = h == c.hash;
                cluster_ver[ci] = ok ? ver : 0;
            }
            if (ok) {
                v->native = c.fn;
                v->count_n = 0;
                v->lines = c.lines;
                v->nlines = c.nlines;
                v->cluster = (s32)ci;
            }
        }
        v->home = v->lines ? v->lines[0] >> LINES_PER_REGION_SHIFT : v->line0 >> LINES_PER_REGION_SHIFT;
        if (v->lines) for (u32 i = 0; i < v->nlines; i++) link_region(v->lines[i], v->home);
        else for (u32 l = v->line0; ; l = (l + 1) & 2047) { link_region(l, v->home); if (l == v->line1) break; }
        v->ver_sum = block_ver(*v);
        return v;
    };
    if (slot && matches(*slot)) return mark(slot.get());
    if (slot) {
        // search the older variants; a hit moves to the front
        std::unique_ptr<Block>* prev = &slot->alt;
        for (u32 depth = 0; *prev; prev = &(*prev)->alt, depth++) {
            if (matches(**prev)) {
                std::unique_ptr<Block> hit = std::move(*prev);
                *prev = std::move(hit->alt);
                hit->alt = std::move(slot);
                slot = std::move(hit);
                return mark(slot.get());
            }
            if (depth == 6) { prev->reset(); break; }   // keep at most 8 variants
        }
        auto fresh = std::make_unique<Block>();
        fresh->alt = std::move(slot);
        slot = std::move(fresh);
    } else {
        slot = std::make_unique<Block>();
    }
    Block* b = slot.get();
    b->start = pc;
    b->n = 0;
    b->ops.clear();
    u32 a = pc;
    for (;;) {
        u32 w;
        memcpy(&w, ls + (a & 0x3FFFC), 4);
        w = __builtin_bswap32(w);
        u32 idx = w >> 21;
        b->ops.push_back({g_table[idx], w});
        b->n++;
        a = (a + 4) & 0x3FFFC;
        Handler h = g_table[idx];
        if (g_control[idx] || h == op_stop<SPUThread> || h == op_stopd<SPUThread> || h == op_invalid<SPUThread> || b->n == 256 ||
            a == 0)
            break;
    }
    // ahead-of-time version: binary search by key (pc + words)
    u64 key = 1469598103934665603ull;
    auto mix = [&](u32 v) { for (int k = 0; k < 4; k++) { key ^= (v >> (k * 8)) & 0xFF; key *= 1099511628211ull; } };
    mix(pc & 0x3FFFC);
    for (const BlockOp& o : b->ops) mix(o.w);
    b->aot = -1;
    {
        u32 lo = 0, hi = g_spu_aot_count;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            if (g_spu_aot[mid].key < key) lo = mid + 1; else hi = mid;
        }
        static const bool no_aot = getenv("F1CE_SPU_NOAOT") != nullptr;   // debug: interpret everything
        if (!no_aot && lo < g_spu_aot_count && g_spu_aot[lo].key == key) b->aot = (s32)lo;
    }
    static const char* dump_path = getenv("F1CE_SPU_DUMP_BLOCKS");
    if (dump_path && b->aot < 0) {   // record blocks without a native version for tools/spu_aot.py
        static std::mutex m;
        static std::set<u64> seen;
        std::lock_guard lk(m);
        if (seen.insert(key).second)
            if (FILE* f = fopen(dump_path, "a")) {
                fprintf(f, "%05x", pc & 0x3FFFC);
                for (const BlockOp& o : b->ops) fprintf(f, " %08x", o.w);
                fputc('\n', f);
                fclose(f);
            }
    }
    return mark(b);
}

void SPUThread::run() {
    halted = false;
    _mm_setcsr(_mm_getcsr() | 0x8040);   // flush denormals to zero (FTZ | DAZ), as the SPU does
    if (const char* e = getenv("F1CE_LS_WRITERS")) lsw_base = (u32)strtoul(e, nullptr, 16) & 0x3FFF0;
    if (!blocks_on) flush_blocks();   // host-side LS writes invalidate through write_ls/ls_written
    static const bool per_instruction = getenv("F1CE_SPU_NOBLOCK") || !g_pc_log.empty();
    while (!halted) {
        if (kernel_limit && pc < kernel_limit && on_kernel_call) {
            if (on_kernel_call(*this)) continue;
        }
        if (interrupts && (event_stat & event_mask)) {   // SPU interrupt: vector at LS 0
            srr0 = pc;
            pc = 0;
            interrupts = false;
        }
        if (per_instruction) {
            if (__builtin_expect(!g_pc_log.empty(), 0) && g_pc_log[(pc & 0x3FFFC) >> 2]) pc_log(*this);
            u32 w;
            memcpy(&w, ls + (pc & 0x3FFFC), 4);
            w = __builtin_bswap32(w);
            u32 idx = w >> 21;
            g_table[idx](*this, w);
            if (!g_control[idx]) pc += 4;
            if ((++instructions & 4095) == 0) pc_samples[(instructions >> 12) & 255] = pc;
            continue;
        }
        pc &= 0x3FFFC;
        if (__builtin_expect(pc_trace != nullptr, 0) && pc_trace->size() < 20000) pc_trace->push_back(pc);
        FastEnt& fe = fast[(pc >> 2) & 4095];
        if (fe.pc == pc) {
            const u64 epoch = region_writes[fe.home];
            if (fe.checked == epoch || (fe.cluster >= 0 && cluster_ok[fe.cluster] == epoch)) {
                fe.checked = epoch;
                st_fast++;
                u32 cn = fe.count_n;
                fe.fn(*this);
                u64 before_n = instructions;
                instructions += cn;
                if ((before_n >> 12) != (instructions >> 12)) pc_samples[(instructions >> 12) & 255] = pc;
                continue;
            }
        }
        Block* b = find_block(pc);
        st_slow++;
        if (!b || (b->checked != region_writes[b->home] && b->ver_sum != block_ver(*b))) { b = build_block(pc); st_build++; }
        const u64 epoch = region_writes[b->home];
        b->checked = epoch;
        // debug: F1CE_SPU_VERIFY=1 runs every block through this path and compares it with the LS
        static const bool verify = getenv("F1CE_SPU_VERIFY") != nullptr;
        if (__builtin_expect(verify, 0))
            for (u32 i = 0; i < b->n; i++)
                if (ls32(pc + 4 * i) != b->ops[i].w) {
                    LOG_E("SPU verify: %s block %05x word %u is %08x in LS, %08x in the block", name.c_str(), pc, i, ls32(pc + 4 * i), b->ops[i].w);
                    break;
                }
        if (b->native) {
            static const bool no_fast = getenv("F1CE_SPU_NOFAST") != nullptr;   // debug
            if (b->cluster >= 0) cluster_ok[b->cluster] = epoch;
            if (!no_fast && !verify) fe = {epoch, b->native, pc, b->home, b->cluster, b->count_n};
            b->native(*this);
            u64 before_n = instructions;
            instructions += b->count_n;
            if ((before_n >> 12) != (instructions >> 12)) pc_samples[(instructions >> 12) & 255] = pc;
            continue;
        }
        const BlockOp* op = b->ops.data();
        u32 n = b->n;
        for (u32 i = 0; i + 1 < n; i++) {   // all but the last op fall through
            op[i].h(*this, op[i].w);
            pc += 4;
            if (__builtin_expect(halted, 0)) break;
        }
        if (!halted) {
            const BlockOp& l = op[n - 1];
            l.h(*this, l.w);
            if (!g_control[l.w >> 21]) pc += 4;
        }
        u64 before = instructions;
        instructions += n;
        if ((before >> 12) != (instructions >> 12)) pc_samples[(instructions >> 12) & 255] = pc;
    }
}

// MFC command execution (synchronous)
u32 line_generation(u32 ea);       // core.cpp: write generation of a line (lost reservations)
void line_written(u32 ea);

// profiling: get, put, getllar, putllc, failed putllc; one cache line per SPU id so workers do not contend
struct alignas(64) DmaCounters { std::atomic<u64> c[5]; };
DmaCounters g_dma_counters[16];
u64 dma_count(int k) {
    u64 n = 0;
    for (auto& d : g_dma_counters) n += d.c[k].load(std::memory_order_relaxed);
    return n;
}

void SPUThread::dma(u32 cmd) {
    u32 lsa = mfc_lsa & 0x3FFFF, ea = mfc_eal, size = mfc_size;
    std::atomic<u64>* counts = g_dma_counters[id & 15].c;
    auto bump = [](std::atomic<u64>& n) { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
    switch (cmd & 0xFF) {
        case 0x40: case 0x41: case 0x42: case 0x44: bump(counts[0]); break;
        case 0x20: case 0x21: case 0x22: case 0x24: bump(counts[1]); break;
        case 0xD0: bump(counts[2]); break;
        case 0xB4: bump(counts[3]); break;
    }
    if (on_dma) on_dma(*this, cmd, lsa, ea, size);
    extern u32 g_watch_line;   // core.cpp (F1CE_WATCH_LINE; may be retargeted at run time by F1CE_AUTO_WATCH)
    // debug ring of recent DMA commands (printed with F1CE_WATCH_LINE hits when F1CE_WATCH_HISTORY is set)
    struct DmaRec { u32 cmd, ea, lsa, size, pc; };
    thread_local DmaRec hist[32];
    thread_local u32 hist_pos = 0;
    if (__builtin_expect(g_watch_line > 1, 0)) hist[hist_pos++ % 32] = {cmd & 0xFF, ea, lsa, size, pc};
    auto print_history = [&] {
        for (u32 k = 0; k < 32; k++) {
            const DmaRec& r = hist[(hist_pos + k) % 32];
            if (r.size || r.cmd) LOG_I("   dma history %s: cmd=%02x ea=%08x lsa=%05x size=%x pc=%05x", name.c_str(), r.cmd, r.ea, r.lsa, r.size, r.pc);
        }
    };
    // debug: F1CE_WATCH_LINE=<hex ea> logs every SPU access to that 128-byte line
    const u32 watch = g_watch_line > 1 ? g_watch_line : 0;
    extern double watch_after();
    if (watch && mfc_eal < watch + 128 && mfc_eal + std::max(mfc_size, 1u) > watch && time_usec() / 1e6 >= watch_after()) {
        static std::atomic<int> n{0};
        static const int max_n = getenv("F1CE_WATCH_MAX") ? atoi(getenv("F1CE_WATCH_MAX")) : 200;
        if (n++ < max_n) {
            const u8* src = ls + ((cmd & 0xFF) == 0xD0 ? 0 : (lsa & 0x3FF80));
            u32 c8 = cmd & 0xFF;
            bool write = c8 == 0x20 || c8 == 0xB4 || c8 == 0xB0 || c8 == 0xB8;
            auto w32 = [&](const u8* p, u32 o) { u32 v; memcpy(&v, p + o, 4); return __builtin_bswap32(v); };
            const u8* m = gmem(watch);
            LOG_I("watch %s cmd=%02x pc=%05x lsa=%05x | mem %08x %08x .. +20 %08x | %s %08x %08x .. +20 %08x", name.c_str(), c8,
                  pc, lsa, w32(m, 0), w32(m, 4), w32(m, 0x20), write ? "ls" : "--", write ? w32(src, 0) : 0,
                  write ? w32(src, 4) : 0, write ? w32(src, 0x20) : 0);
            static std::atomic<int> lswn{0};
            if (write && lsw_base && lsw_base >= lsa && lsw_base < lsa + size && lswn++ < 6)
                for (u32 off : {0x20u, 0x3B0u, 0x3D0u, 0x3F0u, 0x490u, 0x4B0u}) {
                    u32 k = (lsw_base - lsa + off) >> 4;   // slot of program offset `off` in the put
                    k = (lsa + off - lsw_base) >> 4;
                    LOG_I("ls writers %s: prog +%03x <- %s %08x (value %08x)", name.c_str(), off,
                          (lsw_pc[k] >> 28) == 0xD ? "dma get from" : "store at pc", (lsw_pc[k] >> 28) == 0xD ? (lsw_pc[k] << 4) : lsw_pc[k], lsw_val[k]);
                    // F1CE_WATCH_SRC=<prog offset hex>: then watch the main-memory source of that slot (its writers
                    // in the next frames get logged)
                    static const char* ws = getenv("F1CE_WATCH_SRC");
                    if (ws && off == (u32)strtoul(ws, nullptr, 16) && (lsw_pc[k] >> 28) == 0xD) {
                        g_watch_line = (lsw_pc[k] << 4) & ~127u;
                        LOG_I("watch src: now watching %08x", g_watch_line);
                    }
                }
            static std::atomic<int> hists{0};
            if (write && getenv("F1CE_WATCH_HISTORY") && hists++ < 3) print_history();
            static std::atomic<int> dumps{0};
            if (write && getenv("F1CE_WATCH_DUMP_LS") && dumps++ < 3) {   // snapshot of the writer's local store
                char fn[64];
                snprintf(fn, sizeof(fn), "spu_put_%s_%05x_%d.ls", name.c_str(), pc, dumps.load());
                if (FILE* f = fopen(fn, "wb")) { fwrite(ls, 1, 0x40000, f); fclose(f); }
            }
            if (c8 == 0x20 && size > 128) {   // large put spanning the line: the data that lands on the watched line
                const u8* d = ls + ((lsa + (watch - ea)) & 0x3FFFF);
                std::string h;
                for (u32 k = 0; k < 128; k += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", w32(d, k)); h += b; }
                LOG_I("watch %s put ea=%08x size=%x line data:%s", name.c_str(), ea, size, h.c_str());
            }
        }
    }
    if (!g_ls_watch.empty() && (cmd & 0xFC) == 0x40)
        for (u32 x : g_ls_watch)
            if (x >= lsa && x < lsa + size) {
                static std::atomic<int> n{0};
                if (n++ < 100) LOG_I("ls watch %s get ea=%08x size=%x -> lsa=%05x covers %05x (pc=%05x)", name.c_str(), ea, size, lsa, x, pc);
            }
    if (lsw_base && (cmd & 0xFC) == 0x40)
        for (u32 k = 0; k < 128; k++)
            if (lsw_base + k * 16 >= lsa && lsw_base + k * 16 < lsa + size) { lsw_pc[k] = 0xD0000000 | (ea >> 4); lsw_val[k] = pc; }
    bool is_list = (cmd & 0xF0) == 0x40 || (cmd & 0xF0) == 0x20 ? (cmd & 4) != 0 : false;
    if (!is_list && (cmd & 0xFF) != 0xC0 && (cmd & 0xFF) != 0xC8 && (cmd & 0xFF) != 0xCC && ea < 0x10000) {
        static const bool skip = getenv("F1CE_SPU_SKIP_NULL_DMA") != nullptr;
        LOG_E("SPU %s: MFC cmd %02x to invalid EA %08x at %05x (lsa=%05x size=%x tag=%u) - %s", name.c_str(), cmd, ea, pc,
              lsa, size, mfc_tag, skip ? "skipped" : "halting SPU");
        if (skip) return;
        static std::atomic<int> dumps{0};
        if (dumps++ < 2) {
            char fn[48];
            snprintf(fn, sizeof(fn), "spu_fault_%d.ls", dumps.load());
            if (FILE* f = fopen(fn, "wb")) { fwrite(ls, 1, 0x40000, f); fclose(f); }
            u32 line = gpr[81].w[3] & ~127u;
            if (line >= 0x10000 && line < 0x40000000)
                for (u32 k = 0; k < 64; k += 16)
                    LOG_E("  mem[r81+%02x] %08x %08x %08x %08x   ls[1e000+%02x] %08x %08x %08x %08x", k, ld32(line + k),
                          ld32(line + k + 4), ld32(line + k + 8), ld32(line + k + 12), k, ls32(0x1e000 + k),
                          ls32(0x1e004 + k), ls32(0x1e008 + k), ls32(0x1e00c + k));
            for (int r = 0; r < 128; r += 4)
                LOG_E("  r%-3d %08x  r%-3d %08x  r%-3d %08x  r%-3d %08x", r, gpr[r].w[3], r + 1, gpr[r + 1].w[3], r + 2,
                      gpr[r + 2].w[3], r + 3, gpr[r + 3].w[3]);
        }
        halted = true;
        return;
    }
    switch (cmd & 0xFF) {
        case 0x20: case 0x21: case 0x22: case 0x28: case 0x29: case 0x2A:   // PUT[B|F] (+ S variants)
            memcpy(gmem(ea), ls + lsa, size);
            break;
        case 0x40: case 0x41: case 0x42: case 0x48: case 0x49: case 0x4A:   // GET[B|F]
            write_ls(lsa, gmem(ea), size);
            break;
        case 0x24: case 0x25: case 0x26: case 0x44: case 0x45: case 0x46: {  // list PUT/GET
            u32 tag = mfc_tag & 31;
            stalled[tag] = {true, (cmd & 0xF0) == 0x40, mfc_eal & 0x3FFF8, 0, size / 8, lsa};
            run_list(tag);
            break;
        }
        case 0xD0: {   // GETLLAR
            std::lock_guard lk(atomic_lock(ea));
            ea &= ~127u;
            memcpy(res_data, gmem(ea), 128);
            write_ls(lsa & 0x3FF80, res_data, 128);
            res_addr = ea;
            res_gen = line_generation(ea);
            has_reservation = true;
            atomic_stat = 4;   // MFC_GETLLAR_SUCCESS
            break;
        }
        case 0xB4: {   // PUTLLC
            std::lock_guard lk(atomic_lock(ea));
            ea &= ~127u;
            static const bool no_gen = getenv("F1CE_NO_LINEGEN") != nullptr;   // debug: data comparison only
            bool ok = has_reservation && res_addr == ea && (no_gen || res_gen == line_generation(ea)) && same_bytes(gmem(ea), res_data, 128);
            if (ok) { memcpy(gmem(ea), ls + (lsa & 0x3FF80), 128); line_written(ea); }
            if (!ok) g_dma_counters[id & 15].c[4].fetch_add(1, std::memory_order_relaxed);
            has_reservation = false;
            atomic_stat = ok ? 0 : 1;   // MFC_PUTLLC_SUCCESS / FAILURE
            break;
        }
        case 0xB0: case 0xB8: {   // PUTLLUC / PUTQLLUC
            std::lock_guard lk(atomic_lock(ea));
            ea &= ~127u;
            memcpy(gmem(ea), ls + (lsa & 0x3FF80), 128);
            line_written(ea);
            if (has_reservation && res_addr == ea) has_reservation = false;
            atomic_stat = 2;   // MFC_PUTLLUC_SUCCESS
            break;
        }
        case 0xC0: case 0xC8: case 0xCC: break;   // barrier / eieio / sync
        case 0xA0: case 0xA1: case 0xA2: break;   // SNDSIG (unused here)
        default: LOG_W("SPU %s: unsupported MFC command %02x (lsa=%05x ea=%08x size=%x)", name.c_str(), cmd, lsa, ea, size);
    }
}

// Processes list elements until the end of the list or a stall-and-notify element. On a stall the
// element is transferred, the list is suspended and the Sn event is raised; MFC_WrListStallAck resumes it.
void SPUThread::run_list(u32 tag) {
    StalledList& L = stalled[tag];
    while (L.index < L.count) {
        u32 e0 = ls32(L.list + L.index * 8), e1 = ls32(L.list + L.index * 8 + 4);
        u32 ts = e0 & 0x7FFF;
        if (ts) {
            // the element's LS address keeps the EA's offset within the quadword (relevant for sizes < 16)
            u32 elsa = (L.lsa | (e1 & 15)) & 0x3FFFF;
            if (L.get) write_ls(elsa, gmem(e1), ts);
            else memcpy(gmem(e1), ls + elsa, ts);
            extern u32 g_watch_line;
            if (!L.get && g_watch_line < e1 + ts && e1 < g_watch_line + 128) {   // debug: F1CE_WATCH_LINE also covers list puts
                static std::atomic<int> n{0};
                if (n++ < 100) LOG_I("watch %s list put pc=%05x ea=%08x size=%x", name.c_str(), pc, e1, ts);
            }
        }
        L.lsa = (L.lsa + std::max<u32>(ts, 16) + 15) & 0x3FFF0;   // next element: + max(size, 16), rounded to a quadword
        L.index++;
        if (e0 & 0x80000000) {
            stall_stat |= 1u << tag;
            event_stat |= 0x2;   // Sn: list stall-and-notify
            return;
        }
    }
    L.active = false;
}

u32 spu_load_elf(SPUThread& spu, u32 elf) {
    u32 phoff = ld32(elf + 28);
    u16 phnum = ld16(elf + 44);
    for (u32 i = 0; i < phnum; i++) {
        u32 ph = elf + phoff + i * 32;
        u32 type = ld32(ph), off = ld32(ph + 4), va = ld32(ph + 8), fsz = ld32(ph + 16), msz = ld32(ph + 20);
        if (type != 1) continue;
        memcpy(spu.ls + va, gmem(elf + off), fsz);
        memset(spu.ls + va + fsz, 0, msz - fsz);
    }
    return ld32(elf + 24);
}

// Benchmark (F1CE_SPU_BENCH=1): a 10-instruction loop mixing integer, shuffle, load/store and branch, 20M times.
// reference shufb (scalar, from the SPU ISA) for the self-test below
static v128 shufb_ref(const v128& a, const v128& b, const v128& c) {
    v128 r;
    for (int i = 0; i < 16; i++) {
        u8 x = c.b[15 - i];
        u8 v;
        if (x & 0x80) v = (x & 0xC0) == 0x80 ? 0x00 : (x & 0xE0) == 0xC0 ? 0xFF : 0x80;
        else { u32 k = x & 31; v = k < 16 ? a.b[15 - k] : b.b[15 - (k - 16)]; }
        r.b[15 - i] = v;
    }
    return r;
}

void spu_bench() {
    {   // shufb self-test against the scalar reference
        auto t = std::make_unique<SPUThread>();
        u64 seed = 88172645463325252ull;
        auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        int bad = 0;
        for (int n = 0; n < 1000000 && bad < 5; n++) {
            for (int r = 1; r <= 3; r++) { t->gpr[r].d[0] = rnd(); t->gpr[r].d[1] = rnd(); }
            v128 ref = shufb_ref(t->gpr[1], t->gpr[2], t->gpr[3]);
            op_shufb(*t, (0xBu << 28) | (4u << 21) | (2u << 14) | (1u << 7) | 3u);   // shufb r4, r1, r2, r3
            if (memcmp(&ref, &t->gpr[4], 16)) bad++;
        }
        LOG_I("spu bench: shufb self-test %s", bad ? "FAILED" : "ok");
    }
    auto s = std::make_unique<SPUThread>();
    s->name = "bench";
    const u32 code[] = {
        0x40800001 | (0 << 7),     // il r1, 0 (patched below with ilhu/iohl)
        0x1C004081,                // ai r1, r1, 1          (placeholder, rewritten below)
    };
    (void)code;
    // il r2,0 ; ilhu r1,0x0131 ; iohl r1,0x2D00 (r1 = 20,000,000)
    u32 prog[] = {
        0x40800002,                // il r2, 0
        0x41009881,                // ilhu r1, 0x0131
        0x60969681 & 0xFF80007F,   // iohl r1, ... (set below)
        // loop @0x0C:
        0x1C004103,                // ai r3, r2, 1
        0x18008184,                // a r4, r3, r2
        0x24004004,                // stqd r4, 16(r0)
        0x34004005,                // lqd r5, 16(r0)
        0x08214186,                // or r6, r3, r5
        0xB0E18307,                // shufb r7, r6, r6, r3
        0x18018282,                // a r2, r5, r6
        0x1CFFC081,                // ai r1, r1, -1
        0x217FFC01,                // brnz r1, loop (-8 words: 0x2c -> 0x0c)
        0x00000000,                // stop
    };
    prog[2] = 0x60800001 | ((0x2D00u & 0xFFFF) << 7);   // iohl r1, 0x2D00
    for (u32 i = 0; i < sizeof(prog) / 4; i++) s->ls32(0x1000 + i * 4, prog[i]);
    s->pc = 0x1000;
    s->on_stop = [](SPUThread& t, u32) { t.halted = true; return true; };
    u64 t0 = time_usec();
    s->run();
    u64 dt = time_usec() - t0;
    LOG_I("spu bench: %llu instructions in %.3f s = %.0f MIPS (r1=%u)", (unsigned long long)s->instructions, dt / 1e6,
          s->instructions / (double)dt, s->pref(1));
}
