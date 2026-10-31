// Index lists and raw vertex regions, written once for both renderers: this file is included in each one's anonymous
// namespace, after these are defined:
//   u8* ring_alloc(u32 bytes, ptrdiff_t* off)                  space in the stream ring
//   ptrdiff_t idx_stream(const u32* data, u32 n)               a list used by this draw only
//   bool idx_store(const u32* data, u32 n, u32& off, u32& bytes)   a cached list in the index arena
//   void heap_copy_begin(), void heap_copy(ptrdiff_t ring_off, u32 heap_off, u32 n)   stream ring -> vertex heap
//   BlockHeap g_heap, g_iheap; constexpr bool LINE_LOOP_AS_STRIP; constexpr u32 RING_SEG (stream ring segment size)
struct IdxEntry {
    u32 off = ~0u, bytes = 0;   // block in the index arena (~0u: none)
    u64 hash = 0; u32 count = 0, min_v = 0, max_v = 0; u64 last_change = 0, last_frame = 0;
    u32 id_src = 0, id_count = 0, id_flags = 0;
    u64 seen_frame = 0;   // as VtxEntry::seen_frame
    u64 checked_frame = ~0ull;
};
std::unordered_map<u64, IdxEntry> g_idx_cache;   // converted index lists (see draw())
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

void evict_index_caches() {
    for (auto it = g_idx_regions.begin(); it != g_idx_regions.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_iheap, it->second.off, it->second.bytes);
            it = g_idx_regions.erase(it);
        } else ++it;
    for (auto it = g_idx_cache.begin(); it != g_idx_cache.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_iheap, it->second.off, it->second.bytes);
            it = g_idx_cache.erase(it);
        } else ++it;
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
void setup_attrs_gpu(u32 min_v, u32 max_v, int* va) {
    u32 input_mask = reg(0x1FF0), nverts = max_v - min_v + 1;
    memset(va, 0, 64 * sizeof(int));
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
        ptrdiff_t roff;
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
                heap_copy_begin();
                for (u32 done = 0; done < n;) {   // in pieces of at most one ring segment
                    u32 k = std::min(n - done, RING_SEG);
                    ptrdiff_t roff;
                    u8* rp = ring_alloc(k, &roff);
                    if (!rp) break;
                    memcpy(rp, gmem(ea + done), k);
                    heap_copy(roff, re.off + done, k);
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
}

// the draw's index list in the arena (or the buffer idx_stream gave); false when there is nothing to draw
struct DrawGeom { u32 min_v, max_v, idx_count; ptrdiff_t idx_offset; };
bool gather_indices(bool indexed, DrawGeom& out) {
    constexpr u32 RESTART = 0xFFFFFFFFu;
    u32 min_v = ~0u, max_v = 0, idx_count = 0;
    ptrdiff_t idx_offset = 0;   // index lists stay absolute: every draw uses base vertex -min_v
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
        if (indexed && contiguous && g_prim != 8 && g_prim != 9 && !(LINE_LOOP_AS_STRIP && g_prim == 3) && !idx_nocache && !no_idx_region && count &&
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
            if (min_v > max_v) return false;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return false; }
            idx_count = count;
            if (ir.off != ~0u) idx_offset = (ptrdiff_t)ir.off + (ptrdiff_t)(first - ir.first) * 4;
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
            if (min_v > max_v) return false;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return false; }
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
            if (LINE_LOOP_AS_STRIP && g_prim == 3) {   // no line loops in Vulkan: closed line strips
                std::vector<u32> loop;
                size_t seg = 0;
                for (size_t i = 0; i <= indices.size(); i++) {
                    if (i < indices.size() && indices[i] != RESTART) continue;
                    if (i > seg) {
                        loop.insert(loop.end(), indices.begin() + seg, indices.begin() + i);
                        loop.push_back(indices[seg]);
                        loop.push_back(RESTART);
                    }
                    seg = i + 1;
                }
                indices.swap(loop);
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
                idx_offset = (ptrdiff_t)ie->off;
            } else {
                idx_offset = idx_stream(indices.data(), idx_count);   // (arena full: also cached lists stream)
            }
        } else if (!region) {
            idx_count = ie->count;
            min_v = ie->min_v;
            max_v = ie->max_v;
            idx_offset = (ptrdiff_t)ie->off;
        }
        if (!idx_count) return false;
    }
    out = {min_v, max_v, idx_count, idx_offset};
    return true;
}
