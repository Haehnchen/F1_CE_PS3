// Cell SPU interpreter: registers, local store, channels and MFC (DMA + atomics).
#pragma once
#include <memory>
#include <vector>
#include <algorithm>
#include "kernel.h"

#include <functional>

// ahead-of-time translated SPU blocks: key = FNV-1a 64 of the start pc and the instruction words
struct SpuAotBlock { u64 key; void (*fn)(struct SPUThread&); s32 cluster; };
extern const SpuAotBlock g_spu_aot[];
extern const u32 g_spu_aot_count;
// Linked blocks translated into one function that can be entered at any member (it dispatches on pc) and jumps
// between members directly. Usable only while every member's words are in the LS: hash = FNV-1a 64 over the
// members' words in table order; members = (pc, word count) pairs; [lo, hi) spans them.
struct SpuAotCluster {
    u64 hash; u32 lo, hi; const u32* members; u32 count; void (*fn)(struct SPUThread&);
    const u16* lines; u32 nlines;   // the 128-byte LS lines holding member words (guarded instead of the whole span)
};
extern const SpuAotCluster g_spu_aot_clusters[];
extern const u32 g_spu_aot_cluster_count;

struct SPUThread {
    // Registers use the same byte-reversed layout as VMX registers: BE word i lives in w[3-i],
    // so the preferred slot (BE word 0) is w[3].
    v128 gpr[128];
    u8* ls;                     // 256 KiB local store (big-endian bytes)
    u32 pc = 0;
    u32 id = 0;
    std::string name;

    // channels / MFC
    u32 mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    u32 tag_mask = 0, atomic_stat = 0;
    u32 event_mask = 0, event_stat = 0;
    u32 dec_start = 0;
    u64 dec_time = 0;
    u32 srr0 = 0;
    bool interrupts = false;    // enabled/disabled by the E/D bits of indirect branches
    // list DMA suspended at a stall-and-notify element, per tag
    struct StalledList { bool active = false, get = false; u32 list = 0, index = 0, count = 0, lsa = 0; };
    StalledList stalled[32];
    u32 stall_stat = 0;
    void run_list(u32 tag);

    bool has_reservation = false;
    u32 res_addr = 0, res_gen = 0;   // reservation line, its write generation at GETLLAR
    alignas(16) u8 res_data[128];
    // mailboxes
    std::mutex mbox_m;
    std::condition_variable mbox_cv;
    bool in_mbox_valid = false, out_mbox_valid = false;
    // SPURS tasks: spu_printf-style send_event reads a second word, the completion from the PPU-side handler
    bool event_needs_completion = false, event_completion_pending = false;
    u32 in_mbox = 0, out_mbox = 0;
    u32 snr[2] = {0, 0};

    // Called on 'stop' (code). Return true to continue at pc+4 (or wherever pc was set), false to halt.
    // SPU_RdMachStat bit 0 reports interrupts enabled even while this emulator keeps them off: the SPURS kernel runs
    // workloads and tasks with interrupts on, and job code asserts it (rdch MachStat; brz -> stopd)
    bool report_ie = false;
    std::vector<u32>* pc_trace = nullptr;   // debug: block start addresses of this run (F1CE_TRACE_WKL)
    std::function<bool(SPUThread&, u32 code)> on_stop;
    // Called when the SPU writes SPU_WrOutIntrMbox (sys_spu_thread_send_event / lv2 service request): the value
    // written, and the outbound mailbox word (data1) if one is pending. The result is put in the inbound mailbox.
    std::function<u32(SPUThread&, u32 intr, u32 data1)> on_send_event;
    // Called for branches that target the kernel area (pc < kernel_limit); return true if handled.
    u32 kernel_limit = 0;
    std::function<bool(SPUThread&)> on_kernel_call;

    // diagnostics: log LS accesses below trace_ls_limit (budgeted) and every MFC command
    u32 trace_ls_limit = 0, trace_budget = 0;
    // debug (F1CE_LS_WRITERS=<ls addr>): last writer of each quadword in [addr, addr + 0x800): pc (store) or
    // 0xD0000000 | ea >> 4 (DMA get), and the value of the last store
    u32 lsw_base = 0;
    u32 lsw_pc[128] = {};
    u32 lsw_val[128] = {};

    // block cache: basic blocks decoded once (handler + instruction word per op), invalidated per 128-byte line
    // when LS memory holding decoded code is written (stores, DMA)
    struct BlockOp { void (*h)(SPUThread&, u32); u32 w; };
    struct Block {
        u32 start = 0, n = 0, line0 = 0, line1 = 0, ver_sum = 0;
        u64 checked = 0;   // code_writes when the block was last validated
        void (*native)(SPUThread&) = nullptr;   // ahead-of-time translated version (generated/spu_aot*.cpp)
        s32 aot = -1;                            // its g_spu_aot index
        u32 count_n = 0;                         // instructions counted per dispatch (cluster code counts itself)
        const u16* lines = nullptr;              // cluster code: guarded lines (else line0..line1)
        u32 nlines = 0;
        std::vector<BlockOp> ops;
        std::unique_ptr<Block> alt;   // older variants decoded at the same pc (code swapped in and out by DMA)
    };
    // block slots per LS word in a two-level table (256 pages of 256 words, allocated where code runs), so the hot
    // part stays small in cache
    struct BlockPage { std::unique_ptr<Block> b[256]; };
    std::unique_ptr<BlockPage> block_pages[256];
    bool blocks_on = false;   // block cache in use (host LS writes must invalidate)
    Block* find_block(u32 pc) const {
        const BlockPage* p = block_pages[(pc >> 10) & 255].get();
        return p ? p->b[(pc >> 2) & 255].get() : nullptr;
    }
    std::unique_ptr<Block>& block_slot(u32 pc) {
        auto& p = block_pages[(pc >> 10) & 255];
        if (!p) p = std::make_unique<BlockPage>();
        return p->b[(pc >> 2) & 255];
    }
    u32 line_ver[2048] = {};
    u64 code_writes = 1;   // bumped whenever a line holding decoded code is written
    bool line_code[2048] = {};
    void ls_written(u32 a, u32 n) {
        if (!blocks_on) return;
        for (u32 l = (a & 0x3FFFF) >> 7, e = ((a + std::max(n, 1u) - 1) & 0x3FFFF) >> 7; ; l = (l + 1) & 2047) {
            if (line_code[l]) { line_ver[l]++; code_writes++; line_code[l] = false; }   // marked again when decoded
            if (l == e) break;
        }
    }
    std::vector<u32> cluster_ver;   // per cluster: version sum of its lines when its words last matched (0 = never)
    u32 lines_ver(const u16* l, u32 n) const {
        u32 v = 0;
        for (u32 i = 0; i < n; i++) v += line_ver[l[i]];
        return v;
    }
    u32 block_ver(const Block& b) const {
        if (b.lines) return lines_ver(b.lines, b.nlines);
        u32 v = 0;
        for (u32 l = b.line0; ; l = (l + 1) & 2047) { v += line_ver[l]; if (l == b.line1) break; }
        return v;
    }
    // copies into LS and invalidates decoded code only if the bytes change (re-loaded overlays keep their blocks)
    void write_ls(u32 lsa, const void* src, u32 n) {
        lsa &= 0x3FFFF;
        bool code = false;
        if (blocks_on)
            for (u32 l = lsa >> 7, e = ((lsa + std::max(n, 1u) - 1) & 0x3FFFF) >> 7; ; l = (l + 1) & 2047) {
                code |= line_code[l];
                if (l == e) break;
            }
        if (code && memcmp(ls + lsa, src, n) == 0) return;
        memmove(ls + lsa, src, n);
        if (code) ls_written(lsa, n);
    }
    // a direct jump inside translated cluster code may skip the dispatcher only when the dispatcher would not act:
    // no code was written since entry, the target is not a kernel call and no interrupt is pending
    bool can_chain(u64 cw, u32 target) const {
        return code_writes == cw && target >= kernel_limit && !(interrupts && (event_stat & event_mask));
    }
    // dispatch cache: native code of recently run blocks in one compact direct-mapped table, valid while no code was
    // written since the block was verified (the block objects are scattered on the heap, and the cache misses of
    // the full lookup were the largest single cost of the SPU threads)
    struct FastEnt { u64 checked; void (*fn)(SPUThread&); u32 pc, count_n; };
    FastEnt fast[1024] = {};
    Block* build_block(u32 pc);
    void flush_blocks();
    std::function<void(SPUThread&, u32 cmd, u32 lsa, u32 ea, u32 size)> on_dma;

    bool halted = false;
    u64 instructions = 0;
    u32 pc_samples[256] = {};   // one PC every 4096 instructions (diagnostics)

    SPUThread();
    ~SPUThread();
    SPUThread(const SPUThread&) = delete;

    u32 ls32(u32 a) const { u32 v; memcpy(&v, ls + (a & 0x3FFFC), 4); return __builtin_bswap32(v); }
    void ls32(u32 a, u32 v) { v = __builtin_bswap32(v); memcpy(ls + (a & 0x3FFFC), &v, 4); }
    u32 pref(int r) const { return gpr[r].w[3]; }   // preferred word slot
    void set_pref(int r, u32 v) { gpr[r].pi = _mm_setzero_si128(); gpr[r].w[3] = v; }

    void run();            // execute until halted
    void dma(u32 cmd);     // perform an MFC command synchronously
};

// Loads an SPU ELF image (from guest memory) into local store; returns the entry point.
u32 spu_load_elf(SPUThread& spu, u32 elf_ea);
