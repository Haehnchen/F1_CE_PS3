# F1 Championship Edition (PS3) – static recompilation

Translates the game's PPU executable (`EBOOT.elf`, PowerPC64/Cell) into C++ and links it with a
runtime that reimplements the PS3 kernel (LV2), the system libraries (HLE), the SPUs and the RSX
GPU, producing native x86-64 executables for Windows and Linux. Game data is never embedded: the
executable reads your own `PS3_GAME` folder at runtime.

## Layout

| Path | Content |
|------|---------|
| `tools/ps3elf.py` | ELF loader: segments, OPD, imports (NIDs resolved via `nid_names.txt`) |
| `tools/ppc.py` | PowerPC/AltiVec decoder |
| `tools/functions.py` | function discovery (OPD + call targets + import stubs) |
| `tools/recomp.py` | PPU → C++ translator, jump-table resolver, output in `generated/` |
| `tools/spu.py` | SPU decoder/disassembler (used to analyse the SPU code) |
| `tools/frames_to_png.py` | converts frame dumps (`frame_*.argb`) to PNG |
| `ghidra/scripts/ExportDecomp.java` | headless Ghidra export of pseudo-C for every function (`out/decomp/`) |
| `runtime/core.cpp`, `loader.cpp` | guest memory, threads, dispatch, crash diagnostics, ELF loading |
| `runtime/lv2.cpp`, `fs.cpp` | LV2 syscalls (threads, sync, events, timers, memory, tty, file system) |
| `runtime/hle_*.cpp` | HLE for the 9 imported libraries (sysPrxForUser, sys_io, sys_fs, cellSysutil, cellAudio, cellGcmSys, cellSpurs, cellUsbd, cellSysmodule) |
| `runtime/spu.cpp` | SPU execution: block cache + dispatcher, MFC DMA (incl. lists with stall-and-notify), atomics, events, interrupts |
| `runtime/spu_ops.h` | SPU instruction semantics (one template handler per instruction), shared by the interpreter and the translated code |
| `tools/spu_aot.py` | ahead-of-time SPU translation: recorded blocks → C++ in `generated/spu/` (single blocks + linked clusters) |
| `runtime/hle_spurs.cpp` | host SPURS kernel running the game's SPU policy module |
| `runtime/hle_gcm.cpp` | RSX FIFO front end (jumps/calls, semaphores, labels, reports, flips) |
| `runtime/rsx_gl.cpp` | OpenGL 3.3 renderer: RSX vertex/fragment programs → GLSL, textures, blending |
| `runtime/host_sdl.cpp` | SDL3 window, keyboard/gamepad input, audio output |
| `runtime/host_null.cpp`, `rsx_null.cpp` | headless backend (used by the Linux build for now) |
| `cmake/` | toolchains: Windows (llvm-mingw), Linux cross (zig) |
| `third_party/` | SDL3 sources (not in git; download `SDL3-3.4.18` there) |

## Build

Requirements: Python 3, CMake, Ninja, llvm-mingw (Windows), zig (Linux cross build), SDL3 3.4.18
sources in `third_party/SDL3-3.4.18`, the decrypted `EBOOT.elf` copied to `input/EBOOT.elf`.

```sh
python tools/recomp.py                     # regenerate generated/*.cpp
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake
cmake --build build-win -j 6
cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-zig.cmake
cmake --build build-linux -j 6
```

SPU code runs through translated C++ where available (`generated/spu/`, kept in the repository). To add blocks
the game executes that are not translated yet, run with `F1CE_SPU_DUMP_BLOCKS=<file>` and then
`python -I tools/spu_aot.py <file>` (blocks already translated are kept), and rebuild.

`F1CE_HOST` selects the platform backend (`SDL` default on Windows, `NULL` = headless).
`F1CE_TRACE_PC` (on by default) records the guest PC of memory accesses for crash reports.

## Run

```sh
f1ce <path-to>/PS3_GAME [--trace]
```

A 1280x720 window opens. Log in `f1ce_recomp.log`, saves in `ps3_data/` (both in the current folder).
The console shows warnings and errors only; the log file has everything (`F1CE_LOG_CONSOLE=1` prints every line,
which slows the game noticeably in Git Bash / the Windows console).

Keyboard: arrows = d-pad, **Z** = ✕, **X** = ◯, **A** = □, **S** = △, **Q/W** = L1/R1, **1/2** = L2/R2,
**Enter** = Start, **Backspace** = Select. Gamepads are supported through SDL.

### Debugging aids

| Variable / option | Effect |
|-------------------|--------|
| `--trace` | logs every HLE call and syscall |
| `F1CE_DUMP_FRAMES=N` | saves every N-th frame as `frame_*.argb` (convert with `tools/frames_to_png.py`) |
| `F1CE_INPUT="12:cross,16.5:down"` | scripted pad presses (seconds since start, 200 ms each) for automated tests |
| `F1CE_DUMP_SURFACES=740000,10000` | saves those render targets every 300 frames |
| `F1CE_DUMP_ON_PROG=<prog>` | saves the input/target surfaces around that program's first draw every 300 frames |
| `F1CE_TRACE_FRAME=<n>` | logs every draw of frame n (program, target, state, textures) |
| `F1CE_DUMP_SHADERS=1` | writes the generated GLSL (`fp_<hash>.glsl`, `vp_<hash>.glsl`) |
| `F1CE_FP_STOP=<i>:<reg>` + `F1CE_FP_STOP_HASH=<fp hash>` | outputs a register (`R3`, `H0`, `v2`, `c`, `C0`) right after instruction i of one fragment program; `F1CE_FP_STOP_SCALE`, `F1CE_FP_STOP_NAN` |
| `F1CE_FP_DEBUG=t` | shows unit 0's texture instead of the shaded colour |
| `tools/dbg_runs.sh` | runs several debug sessions in parallel up to the profile screen |
| `tools/race_test.sh <name> [VAR=value ...]` | Quick Race test in `dbg/<name>`; quits `QUIT_AFTER` (default 10) s after the race scene appears |
| `F1CE_QUIT_AFTER_3D=<s>` | exits that many seconds after the first frame with 800+ draws (saves the last frame) |
| `F1CE_PROFILE=1` | every few seconds: what each guest thread spends its time on (functions, HLE calls) |
| `F1CE_HOST_PROFILE=<s>` | after s seconds, samples the SPU worker threads for 10 s: hottest host functions and addresses, and the callers of time spent in DLLs; `F1CE_HOST_PROFILE_RSX=1` samples the renderer thread instead |
| `F1CE_SPU_DUMP_BLOCKS=<file>` | records SPU blocks that have no translation (input of `tools/spu_aot.py`) |
| `F1CE_SPU_NOAOT=1`, `F1CE_SPU_NOCLUSTER=1` | interpret all SPU code / use single-block translations only |
| `F1CE_SPU_BENCH=1` | SPU interpreter benchmark and shufb self-test |
| `F1CE_MAX_FRAMES_AHEAD=<n>` | frames the PPU may queue ahead of the renderer (default 1) |
| `F1CE_TASK_DMA_LOG=1` | logs each distinct DMA of the SPURS tasks once |
| `F1CE_SYNC_COMPILE=1` | link shader programs on the render thread instead of the background compile thread |
| `F1CE_DUMP_FROM=<n>`, `F1CE_DUMP_COUNT=<k>` | limit `F1CE_DUMP_FRAMES` to frames n .. n+k-1; `tools/flicker_check.py <dir>` finds flicker in such dumps |
| `F1CE_FLIP_LOG=<first>,<count>` | per flip: displayed buffer and draws/clears per render target |
| `F1CE_TRACE_COUNT=<k>` | extends `F1CE_TRACE_FRAME` to k consecutive frames |
| `F1CE_WPOS_CLIP_W_FP=<fp hash>,..` | WPOS.w = clip w for those programs only (`-<hash>,..`: all but those) |
| `F1CE_FULLSCREEN=1`, F11 / Alt+Enter | start in full screen / toggle it at any time |
| `F1CE_WINDOW=<w>x<h>`, `F1CE_STRETCH=1` | initial window size / fill the window instead of keeping 16:9 (black bars) |
| `F1CE_COMPILE_THREADS=<n>`, `F1CE_NATIVE_GLSL=1` | shader compile threads (default 1) / the scale-1 shader sources of before |
| `F1CE_NO_PRECOMPILE=1` | skips the start-up shader phase (the program database is still recorded) |
| `F1CE_NO_LINEGEN=1` | SPU PUTLLC compares the line data only (no write generation: the old behaviour) |
| `F1CE_SCALE=1..4` | internal resolution (x 1280x720; 3 = 3840x2160): supersampled to the window |
| `F1CE_MSAA=2/4/8` | samples of the targets the game antialiases (default 4, as on the RSX) |
| `F1CE_FXAA=1` | FXAA on the final image |
| `F1CE_DUMP_WINDOW=<n>` | saves the window after presentation every n-th frame (`frame_w_*`) |
| `F1CE_TEX_CPU=1`, `F1CE_NO_IDX_ARENA=1`, `F1CE_TEX_NOMEMO=1` | texture decoding on the CPU / an element buffer per index list / no per-unit texture memo (comparisons) |
| `F1CE_VTX_CPU=1` | converts vertex attributes on the CPU into float4 buffers (the path before vertex pulling) |
| `F1CE_TEXEL_NEAREST`, `F1CE_SHADOW_CULL_FLIP` | old experiments, for comparison (crowd table filter, inverted shadow-map winding) |
| `F1CE_WATCH_LINE=<ea>` + `F1CE_LS_WRITERS=<ls>` + `F1CE_WATCH_SRC=<prog offset>` | follows a fragment-program constant from the SPU put back to its main-memory source and watches that line |
| `F1CE_TEX_NOSKIP`, `F1CE_TEX_NOSTATIC`, `F1CE_VTX_NOCACHE`, `F1CE_VTX_NOSTATIC`, `F1CE_VTX_NOSUBDATA`, `F1CE_IDX_NOCACHE` | switch off parts of the renderer caches (to bisect rendering bugs) |
| watchdog (always on) | every 20 s logs where each guest thread is, its current HLE call and backtrace |

## Status

- [x] Decompilation: 19,198 functions (Ghidra), 0 failures
- [x] PPU translation: 19,070 functions + 139 import stubs, all 195 used opcodes supported
- [x] LV2 kernel + HLE for all 9 imported libraries
- [x] SPU execution: interpreter + host SPURS kernel; the game's job manager and audio jobs run
- [x] RSX: FIFO front end and OpenGL renderer; the 2D frontend renders at ~58 fps
- [x] SDL3 window, keyboard/gamepad input, audio output (Windows)
- [x] Linux x86-64 cross build (headless backend; not tested, no Linux environment available)
- [x] 3D scene of the profile screen: lighting, shadow maps, fog, post-processing chain
- [ ] Save data (`cellSaveData*` callbacks): deferred
- [x] Profile creation (save data), Single Player menu, Quick Race at Silverstone: the race renders
- [x] SPURS tasksets: tasks run on SPU interpreters; event flags and queues shared with the SPU side
- [x] Race scene: sky, crowd, lit cars
- [x] Race performance: ~27 fps at Silverstone (was ~2 fps)
- [x] Scene brightness: matches RPCS3 (shadow compare direction fixed)
- [ ] SDL/OpenGL backend for the Linux build

## Roadmap

1. **Correctness and a steady 30 fps.** The race start (full grid) runs at ~23-26 fps, limited by the PPU waiting for
   SPU jobs (SPU workers ~88% busy): speed up the translated SPU code and the SPURS kernel paths.
2. **Decode guest data on the GPU.** *Done 2026-10-28: vertex pulling, one index arena, texture decoding in a compute shader.* Vertex conversion (big-endian, packed formats), index list rebuilding and
   texture deswizzling run on the CPU (~15-20 ms per frame on the render thread, plus change-detection hashing):
   upload raw data and decode it in shaders / compute shaders. Also removes most of the loading stutter.
3. **Resolution scaling (up to 4K) and anti-aliasing options.** *Done 2026-10-29: `F1CE_SCALE`, `F1CE_MSAA`, `F1CE_FXAA`.* Render targets at an internal scale factor (surfaces
   the CPU reads back stay native; copies, cube map and shadow maps scaled; post-processing offsets checked);
   MSAA 8x, FXAA/SMAA on the final image, supersampling via the scale factor. GPU-side cost only.
4. **60 fps.** The game ties its logic and physics to the frame rate; the RPCS3 60 fps patch for this game changes the
   frame limiter and timing constants. Apply the equivalent changes to the recompiled code (patch table, addresses
   verified against this binary) once steps 1-2 leave enough CPU headroom.
5. **Vulkan backend** next to OpenGL: lower per-call driver cost, explicit pipeline caches (no stutter on new
   shaders), multithreaded command recording, better Linux support. DirectX 11 is not planned (no gain over OpenGL,
   Windows only).

All of these become options (environment variables or a config file) with the original behaviour as default.

### Known rendering issues (reported 2026-10-25, updated 2026-10-28)

- Mirror images are darker than on PS3/RPCS3. The rear view (256x128 target at 0x02550000) is drawn with an unlit
  fragment program: colour = texture x C[467].x (1) x FC0, fogged towards (2, 2, 4), and FC0 is 0.1. The mirror glass
  then applies exposure 0.18 and Reinhard tone mapping, so the asphalt ends near black while the sky (about 1.0) is
  right. FC0 is not patched: the SPU job copies it from the material's program in main memory, written once at load
  (`F1CE_WATCH_LINE` on the copy + `F1CE_LS_WRITERS=f180` + `F1CE_WATCH_SRC=20` follow it to that source). The
  vertex program passes normals that this fragment program ignores, so the game probably selects a lit variant on
  PS3 (quality / LOD choice still to find). The fog factor is 1 there, as expected for near objects.
- Suspension arms in front of the cockpit are darker than in RPCS3 (self-shadowing; the shadow maps cull front faces
  as requested since 2026-10-28).
- Rubber marks near the corners looked much darker than on PS3: not checked again since the 2026-10-28 fixes (needs a
  driven lap).

## Progress log

**2026-10-20** – Toolchain set up (llvm-mingw, zig, CMake, Ninja, JDK 21, Ghidra). Ghidra decompilation of
the whole executable. PPU → C++ translator with jump-table resolution. Runtime with LV2 syscalls and HLE.
First Windows build: the game runs `main`, initialises memory/SPURS/GCM, loads `offline.wad` and audio
banks, then waits for SPU work.

**2026-10-21**
- SPU interpreter (about 200 instructions, MFC DMA, atomics, events, decrementer, mailboxes).
- Host SPURS kernel, with the kernel/policy-module interface derived from the game's own SPU code:
  SPURS instance address, current workload id, exit and poll services, ready counts in the first line
  of the SPURS instance.
- Fixes found through the SPU code: MFC list stall-and-notify, SPU interrupts (E/D bits of indirect
  branches, vector at LS 0), missing `orx` instruction. The audio workload now completes its jobs and
  the main thread runs the game loop.
- SDL3 window/input/audio, then the OpenGL renderer: surfaces as FBOs, vertex arrays and indexed
  draws with primitive restart, vertex/fragment program translation, textures (ARGB, DXT, swizzled),
  blending, depth, alpha test, viewport with bottom-left window origin.
- Fixed `CellPadInfo` layout (arrays sized for 127 pads): the game now detects the controller.
- Scripted input and frame dumps for automated tests. The frontend renders: language selection,
  "Press Start", "Manage Profiles"; the game loads engine sounds, menu music and the intro video.
- Fixed a double vertical flip when presenting to the window.
- Blocked at profile creation: `cellSaveDataListSave` is not implemented yet.

**2026-10-22** – 3D rendering of the profile-creation screen (garage with the driver):
- Fragment programs: condition codes (set/test, conditional writes, conditional KIL), depth export
  (`gl_FragDepth` from R1.z), and the FOGC input computed from FOG_MODE/FOG_PARAMS as on RSX. The
  fog factor was 0, so every lit pixel collapsed to the fog colour (black screen).
- Shadow maps: depth-compare order fixed (reference FUNC stored depth); a zeta buffer sampled with a
  colour format returns its depth bits as A8R8G8B8.
- 2D engines: NV308A inline transfers (the game patches fragment-program constants in video memory
  this way; POINT is method 0x304) and NV0039 memory copies, including copies of render targets.
- New debug tools: per-instruction register output for one shader, surface dumps around a draw,
  parallel test runs.
- Later the same day: profile creation works with the existing save-data HLE; Single Player → Quick
  Race loads Silverstone. Fixed on the way:
  - jump tables whose bound check is not the nearest `cmplwi` (the index register is now tracked):
    a 9-way switch was cut to 2 cases and the game called an unknown address while loading a track;
  - `_cellSpursQueueInitialize` / `_cellSpursEventFlagInitialize` take (spurs, taskset, object, ...);
  - SPURS tasks now run (task ELF loaded at LS 0x3000, SPURS EA at LS 0x1C0, exit through the table
    at [LS 0x2FB8] + 0xC4), and queues/event flags live in guest memory with the layout the game's
    task-side library uses (derived from its SPU code), so PPU pushes reach the tasks and their event
    bits reach the PPU.
  - The race renders (cockpit, start lights, HUD, opponents) but runs at a few frames per second.
- `tools/spu_elf.py <vaddr>` extracts and disassembles an SPU ELF embedded in the executable.
- Race rendering work: texture mip chains, unnormalized texture coordinates (format bit 0x40), vertex
  texture fetch (VP opcode TXL, float textures on vertex units 0x0900+), vertex-program condition codes,
  and the command-buffer callback now also waits for return addresses of RSX CALLs (the PPU overwrote
  commands still to be executed when the RSX lagged behind). Still open: wrong geometry/streaks from
  the programs that use vertex textures, dark image, car/helmet textures, sky, crowd.
- **2026-10-23** – Race scene: vertex-attribute instancing (frequency field of the attribute format +
  FREQUENCY_DIVIDER_OPERATION 0x1FC0: `index % freq` or `index / freq`) makes the crowd billboards appear and
  removes the streaks over the sky; SPU single-precision semantics (no inf/NaN: results saturate,
  denormals flushed); at most one frame of PPU lead over the renderer (`F1CE_MAX_FRAMES_AHEAD`).
  Car bodies still show only reflections: their fragment programs are rebuilt every frame by an SPU job
  (template + patch list, DMA into video memory) and the ambient/sun constants arrive as zero; the source
  of those values is not found yet. Debug aids: `F1CE_PROG_ID`, `F1CE_SKIP_PROG`, `F1CE_SKIP_VTEX`,
  `F1CE_WATCH_FP`, `F1CE_SPU_LS_WATCH`, `F1CE_WATCH_HISTORY`, `F1CE_WATCH_DUMP_LS`, `F1CE_WATCH_MEM_SIZE`.
- Car lighting investigation (2026-10-23, continued): the car fragment programs are assembled by an SPU job
  that patches each constant component by component from per-instance material parameters (84-byte entries,
  copied by PPU function 0x446354 from the material definition); the ambient/sun entries of the cars are zero
  in main memory and nothing updates them during the race. Fixed on the way: MFC list elements smaller than
  16 bytes land at `list LSA | (EA & 15)` and the list LSA advances by max(size, 16) rounded to a quadword.
  New aids: `F1CE_LS_WRITERS`, `F1CE_SPU_PC_LOG` (+ `_R3` filter), `F1CE_AUTO_WATCH`.
- **Cars lit (2026-10-23):** the per-car sun factor comes from a 32x1 A8R8G8B8 render target that the game
  draws in *main memory* and reads back on the PPU (function 0x2FA404, table at the target's address); the
  renderer skipped colour buffers outside local memory. Main-memory colour targets are now rendered and
  written back to guest memory after every draw and clear. Chain found with the new tools: car fragment
  program -> SPU patch job -> per-frame PPU lighting block (0x3094E0: sun colour x per-car factor) -> lookup
  table filled by the GPU. Shader parameter names are hashed as `~crc32(name)`.
- `tools/race_test.sh <name>`: Quick Race test that exits 10 s after the race scene appears
  (`F1CE_QUIT_AFTER_3D`), about 60-70 s per run.
- **Speed (2026-10-23):** the race went from ~2 to ~27 fps on a 6-core Ryzen 5 5600G.
  - SPU: decoded blocks are cached per 128-byte LS line (content-aware invalidation, up to 8 variants per address
    for job code swapped in by DMA), and blocks recorded in play are translated ahead of time into C++
    (`tools/spu_aot.py`). Linked blocks form clusters (one function with direct jumps, validated once by a hash
    of its words). Clusters with loops or long blocks keep the SPU registers in host registers (`SpuLocal`).
    SPURS task contexts are reused per ELF; the reservation lock is striped per 128-byte line; DMA counters
    no longer share a cache line with the dispatcher.
  - Renderer: textures, vertex data and index lists are cached with content hashes (static data is re-checked
    every few frames); GL state, render target, program, uniforms and vertex-buffer bindings are only sent when
    they change; vertex-program constants are uploaded by changed range; render targets that alternate sizes
    at one address are kept instead of reallocated; unused buffers are evicted. Cache entries carry their full
    identity, because a colliding key on a static entry drew another mesh's data (stretched triangles).
  - Now the PPU, SPUs and renderer are all close to their limits: the main thread still waits on SPU jobs
    for about 40% of the frame, and the renderer spends ~15 ms per frame in vertex setup.
- **Flicker fixed (2026-10-23):** splash screens and menus flickered. Both came from the "static data" shortcut of
  the renderer caches (data unchanged for 60 frames was re-checked only every 8-16 frames): the splash quads live
  in a small vertex ring buffer rewritten after a long pause, and the menu background is a video whose YUV planes
  (linear textures) the decoder rewrites. The shortcut now applies only to vertex ranges over 64 KB and to
  swizzled/compressed textures. Vertex and index data seen for the first time is streamed instead of getting
  its own GL buffer (dynamic data sits at a new ring-buffer address every frame); a buffer is made when the same
  data comes back in a later frame. Race: ~27-29 fps.
- **Comparison with RPCS3 screenshots (2026-10-23):**
  - Race scene: the shadow-map test was inverted. RSX texture depth compare is "stored FUNC reference"; we used
    GL's "reference FUNC stored", so with the game's GEQUAL the player car sat in its own shadow and lost the sun
    term: washed-out car, faint decals, dark image. Now the car, asphalt and shadows match the reference.
  - Quick Race menu: the driver portrait (DXT5) is loaded into memory where the menu video used to be rendered,
    and the renderer kept sampling the old render target. Compressed textures now always come from memory.
  - Colour targets in B8 format (one byte per pixel, used by the glare pass) are kept as RGBA; a B8 texture read
    of such a target returns its blue channel.
  - New debug aids: `F1CE_TRACE_DUMP=<from>,<to>` (target after each draw in that range), `F1CE_DUMP_TEX_PROG=<prog>`
    (textures of that program's draws), `F1CE_TRACE_FC=<prog>` (its fragment constants), `F1CE_FP_STOP=<i>:f`
    (fog factor), target size and pitch in the draw trace.
- **Second RPCS3 comparison (2026-10-24):**
  - Title screen: the logo image is loaded where the menu video used to be rendered (uncompressed this time).
    Each local-memory render target now keeps a sampled fingerprint of its guest memory from when it was last
    rendered; if the CPU has rewritten that memory since, the texture comes from memory.
  - Texture sampling state was ignored: TEXTURE_FILTER bits 0-12 hold a signed LOD bias (the game uses -1.5),
    bits 16-19/24-27 the min/mag filters, CONTROL0 the anisotropy and min/max LOD. Car liveries and tyres are now
    as sharp as in RPCS3. Textures addressed in texels (format bit 0x40, e.g. the crowd's G8B8 data table) keep
    linear filtering: nearest sampling at integer texel coordinates made the crowd disappear (probably the reason
    RPCS3 shows no spectators).
  - Occlusion queries: ZPASS_PIXEL_CNT reports (SET_ZPASS_PIXEL_COUNT_ENABLE 0x1D7C, CLEAR_REPORT_VALUE 0x17C8,
    GET_REPORT type 1) are GL samples-passed queries, written into the report at the next flip. They returned 0;
    with real counts the game selects more detailed car/driver models.
  - Still different from RPCS3: cockpit side panels and mirrors look light grey (strong sky reflection from the
    256x256 environment map) where RPCS3 shows them dark and glossy. The tone-mapping exposure is a fixed 0.359.
    It is not the cause on its own: an exposure of 1.0 matches the stands but overexposes the barrier and helmet.
  - Tools: `tools/img_compare.py` (mean colour of regions against a reference screenshot),
    `tools/img_side_by_side.py` (stacked crops), `F1CE_FC_LOG=<fp offset>`, `F1CE_SCAN_FLOAT=<value>`,
    `F1CE_WB_LOCAL=1` (experiment: write small local render targets back to guest memory), FP hash in the
    program log.
- **Race crash, loading, motion blur (2026-10-24):**
  - Crash about 20 s into a race: a jump table (0x119D9C, 6 entries) was cut to 3 entries because the bound came from
    an unrelated `cmplwi` (its index is set by `li` in each path); case 3 fell into an indirect call to 0x11A624 and
    the game stopped. A bound now counts only when the compare is on the index register; otherwise the table is
    read up to the first entry that leaves the function. A 4-minute race now runs to the finish.
  - The watchdog diagnostics could fault on a racy pointer (fixed); scripted pad presses now report full pressure
    (the throttle is the pressure of cross), and `tools/race_test.sh` takes `DRIVE=1` (throttle held for the race),
    `TIMEOUT=<s>` and the `t:button:duration` input syntax.
  - White screen after loading: on the first frames of a scene the renderer linked ~110-220 GL programs (~50-110 ms
    each in the AMD driver) on the render thread, so nothing was presented for up to 25 s; a long stall like this also
    let the RSX run into a command list that was not ready (FIFO fault). Programs are now linked on a background
    thread with a shared GL context (draws that need them are skipped for those frames) and stored in
    `build-win/shader_cache/` (`glProgramBinary`), so later runs load them in ~1 ms each.
  - Motion blur everywhere: the motion-blur pass reads a depth-only render target (0x02F20000) that also has an
    unwritten colour surface at its offset; the renderer took the colour one. A colour and a depth surface at the
    same offset are now told apart by which one was written last. TEXTURE_CONTROL1 channel remaps are applied for
    A8R8G8B8/B8 data and render targets (one texture uses an ARGB->BGRA swap), but not for depth read as colour
    (the decode already gives the order the game expects).
  - Still open: weak reflections and tyre sun highlights compared with PS3 screenshots (`REFERENCE/`), some ghosting
    of distant scenery in the motion blur, ~25 fps in the race instead of a fixed 30.
- **Frame rate, reflections, lighting (2026-10-24):**
  - Vertex data is cached per buffer region (attribute base address + format) instead of per draw range, and index
    lists are built in one pass and drawn with a base vertex (`glDrawElementsBaseVertex`) instead of being rebased.
    Index buffers that the game rewrites every frame (most of the ~450k indices per frame) cannot be cached; static
    ones use index regions per 16K-index chunk, and regions whose content keeps changing fall back to per-draw lists.
    A `getenv` left in the per-draw uniform path cost ~5-8 ms per frame (Windows `getenv` is slow); debug switches
    are read once into statics. The race start (full grid, ~3000 draws per frame) runs at ~25 fps, later in the race
    27-30 (the game's cap is 30); vertex setup (~15 ms) is the largest remaining render-thread cost.
  - Environment reflections: the game renders a 256x256 cube map every frame (six faces one face size apart,
    double-buffered at 0x04060000 / 0x041E0000) and samples it with a cube texture (TEXTURE_FORMAT bit 2), which
    the renderer did not support. The faces are now blitted into a GL cube map and the units declared `samplerCube`
    (a program variant per cube mask). Cockpit side panels and mirror housings are dark and glossy as on PS3.
  - Lighting: two TEXTURE_ADDRESS fields were ignored. Bits 12-15 = 1 (unsigned remap BIASED, x * 2 - 1) are set
    on the normal maps, so normals were wrong on every normal-mapped surface (the tyres were almost black, 21 vs 72
    in RPCS3). Bits 20-23 decode R/G/B/A from sRGB, and SHADER_PACKER (0x1FD8) bit 0 encodes the output to sRGB:
    the game lights in linear space. With both, the onboard view matches RPCS3 within ~10% per region (tyres, white
    bodywork, helmet, sky, asphalt) and the image has the original's contrast. `F1CE_NO_TEXGAMMA`,
    `F1CE_NO_TEXBIAS` and `F1CE_NO_CUBE` switch them off for comparison.
  - The watchdog crashed the game when its diagnostic dump followed a register into an unmapped guest range (seen
    once during the menu video); it now checks the committed ranges (`mem::readable`).
  - Command lists reached by CALL are not bounded by PUT; twice the RSX front end read a stale word of a list the
    game was still writing (a jump to an unmapped offset, fatal). A jump/call to an unmapped offset now waits up to
    500 ms for the word to be rewritten before failing.
  - Debug: `F1CE_TRACE_DUMP_FP=<fp ctl>,...` dumps the target after the draws of chosen fragment programs (draw
    numbers vary between runs), `F1CE_GL_FINISH=1` logs the GPU wait per frame.
- **Video colours, motion blur, render-thread cost (2026-10-24, later):**
  - sRGB output: colour surfaces are now `GL_SRGB8_ALPHA8` (sampled raw, `GL_SKIP_DECODE_EXT`) and drawn with
    `GL_FRAMEBUFFER_SRGB`, so the GPU encodes and blends in linear space. SHADER_PACKER bit 0 stays set for the
    whole game, yet the intro/menu video (YUV planes without gamma bits, copied to the display buffer by a pass
    that reads it raw) came out washed out and magenta when encoded. Encoding is applied to draws that sample
    sRGB-decoded textures and do not copy a raw (non-decoded) colour target: the 3D passes write encoded colour
    that the post passes read back with gamma bits, the video chain stays raw. Video, menus and race now all match
    RPCS3/PS3 screenshots.
  - Motion blur: the pass that smears the screen rebuilds depth from the depth buffer read as A8R8G8B8 with the
    game's 0xAA93 channel remap (A/R/G = depth high/mid/low bytes moved to R/G/B). The remap was skipped for depth
    reads, so depth came from the low bytes and distant scenery was smeared as if close; with it the blur stays at
    the screen edges as in the PS3 footage, and the dotted look on nearby cars is gone.
  - Render thread: the 2D-subchannel method histogram (a `std::map` update per inline-transfer word and a log line
    every 2000 words) cost several ms per frame and is now behind `F1CE_SUBCH_STATS`; small vertex regions that
    have not changed for 60 frames are checked once per frame instead of per draw; streamed vertices and index lists
    go through a 64 MB persistently mapped ring (`F1CE_NO_RING`); the attribute offsets' common part moves into the
    base vertex. At the race start the renderer now waits for the game 5-9 ms per frame: the start (full grid,
    ~3000 draws) runs at 23-26 fps limited by the PPU waiting for SPU jobs (SPU workers ~88% busy, AOT coverage
    complete), later 27-30.
  - SPU dispatch: a direct-mapped cache of native block entry points (`F1CE_SPU_NOFAST`) skips the scattered block
    lookup; no measurable change at the race start, kept as it is cheaper per dispatch.
- **Stability, windscreen, reflections (2026-10-24, evening):**
  - FIFO crashes (entering DRIVER in the Quick Race menu, before the race start): the game builds each frame into
    one of two alternating command lists reached from a 4-slot ring (0x10000-0x14000) and rewrites a list without
    waiting for the RSX. When the renderer stalled, the PPU got ahead, rewrote a list the RSX was still running
    and the FIFO decoded garbage. Fixes: synchronisation writes (reference, semaphore releases) become visible only
    after the next command is fetched (the RSX prefetches; the game reuses a list as soon as it is released);
    the PPU's `sys_timer_usleep` waits while it is more than one ring slot ahead of the RSX (measured from the
    SET_REFERENCE at the start of each slot vs control->ref; `F1CE_NO_RSX_THROTTLE`). The crash history (last jumps
    with PUT/reference, semaphore operations) is printed with a FIFO fault. `tools/menu_test.sh` runs a custom
    input script; `F1CE_INPUT_RECORD=<file>` records the player's input in that syntax (`dbg/rec1/input_rec.txt`
    reaches DRIVER); `F1CE_RSX_STALL=<frame>:<ms>[:<n>]` simulates renderer stalls.
  - Windscreen: method 0x1D7C is ANTI_ALIASING_CONTROL (not the zpass enable): the race target (surface format AA
    bits) is 4x antialiased and see-through materials use alpha to coverage (bit 4). Antialiased targets are now
    drawn into 4x multisampled renderbuffers and resolved before they are read (`F1CE_NO_MSAA`), with
    GL_SAMPLE_ALPHA_TO_COVERAGE: the windscreen is transparent (RPCS3, without MSAA, shows it grainy), the driver's
    gloves behind it are visible again, and edges are antialiased as on PS3.
  - Reflections: the cube map faces were written sRGB-encoded (their draws sample gamma textures) while the car
    shaders read the cube without decode and add it to linear lighting, so reflections were far too strong, most
    visibly on cars in shadow. Cube faces are now written linear; the DRIVER garage scene matches RPCS3.
  - Streamed lists (later the same evening): while loading (race grid, pits) and in race frames the game streams a
    frame list with PUT inside it, then returns PUT to a ring slot and soon rewrites the list. When the renderer was
    still seconds behind in that list (the loading list, texture decoding) it ran into the rewritten part (crash
    starting a Quick Race; the PPU throttle cannot help, the game does not call usleep there). A watcher thread now
    copies the rest of a streamed list the moment PUT leaves it (only if the front end is still in it or has not
    entered it yet) and the front end runs the copy (`F1CE_NO_LIST_SNAP`). The PPU throttle waits up to 15 s and
    only applies in the ring layout. `launch_debug_pit.sh` (repo root) records input and dumps SPURS workload 5's
    control block for the pit-entry freeze (main thread waiting in 0x551228 for job manager wkl 5, which runs once,
    finds no work and exits).
  - Logging: every log line was printed to the console under the log lock and the file flushed per line; in a
    terminal (Git Bash) this stalled the renderer and the PPU enough to cause the slow loading, long white screens
    and FIFO crashes seen only by the user (tests redirect the output). The console now gets warnings/errors only and
    the file is flushed on warnings/errors and once a second. Also handled: `sys_spu_thread_send_event` from SPURS
    tasks (OutIntrMbox): acknowledged instead of blocking the SPU forever (freeze in the race: the task never finished
    and the PPU waited on its event flag).
  - Pit-entry freeze: entering the pits the game queues one job on SPURS job manager workload 5 and waits for its
    completion. The job manager took the job and ran it, but the job asserts that SPU interrupts are enabled
    (`rdch SPU_RdMachStat; andi 1; brz -> stopd`) and our MachStat channel returned 0, so it halted silently with
    `stopd` and the completion never came. SPURS workloads and tasks now report interrupts enabled (they run with
    interrupts on under the real kernel), and `stopd` is logged. Found with `F1CE_TRACE_WKL=<wid>` (block trace of a
    workload's runs) and the workload control-block dumps at ReadyCountStore / after rare runs.
- **Roadmap step 1, first pass (2026-10-25):**
  - SPU code invalidation: the job manager's interrupt handler generates 80 register-save instructions on its LS
    stack at every interrupt. Stores into decoded code counted as code changes even when the bytes were equal, and a
    line stayed marked as code forever, so ordinary stack stores invalidated every block of the SPU ~7 million times
    per second (each one revalidating all blocks and emptying the dispatch cache). Stores now invalidate only on a
    real change and clear the line's code mark (set again when code is decoded there): ~240k/s, the job manager's
    share of SPU samples fell from ~50% to ~8%, SPU workers 77-81% busy.
  - Render thread FIFO: the streamed-list reader took a mutex per command word when snapshots existed but none
    covered the address (~10% of the thread); it now keeps a local copy of the snapshot list and reads a command
    with its arguments through one pointer. `rsx::method` has a table-driven fast path for plain state writes.
  - Race start (full grid): ~25 -> 26-28 fps; the render thread is now the limit (FIFO idle 0-4 ms per frame),
    with vertex setup (~12 ms) the largest part: roadmap step 2.
- **Graphics fixes (2026-10-28):**
  - Fragment program flow control: IFE (opcode 0x42, `if (cond) { } else { }`, words 2/3 = else/endif offsets in
    32-bit words) was ignored, so both branches ran. The crowd shader picks each figure's clothes in such a branch:
    the spectators now wear coloured clothes as on PS3 instead of only skin colour.
  - Textures addressed in texels (crowd data table) are linear-filtered again by default (`F1CE_TEXEL_NEAREST`
    keeps the game's nearest filter; with it the crowd disappears).
  - Shadow maps cull front faces as requested (the inverted winding was an experiment): the start gantry casts its
    shadow across the grid again (`F1CE_SHADOW_CULL_FLIP` restores the experiment for comparison).
  - The draw trace also logs the cull state (`cull=enable/face/front`).
- **Roadmap step 2, vertices (2026-10-28): vertex pulling.** The vertex programs read the raw big-endian guest
  records themselves from two buffer textures (`u_va_ring`: the mapped stream ring; `u_va_heap`: a 256 MB vertex
  heap in video memory) and decode the seven RSX formats (S1, F, SF, UB, S32K, CMP, UB256) and instancing
  (frequency, modulo) in GLSL; `gl_VertexID` is the guest vertex index (base vertex 0). Per draw only `u_va[16]`
  (offset, stride, format, frequency) is uploaded, instead of ~2400 `glBindVertexBuffers` per frame (~1.6 us each in
  the AMD driver) and the float4 conversion. The region cache, change detection and streaming of first sightings
  are as before but keep raw bytes (2-4x less memory, ~40 MB at the race start). Heap blocks are never rewritten:
  changed data gets a new block, the old one is reused when the fence of the frame that freed it has passed. Uploads
  go through the ring and `glCopyBufferSubData`: `glBufferSubData` into the heap while draws read it stalled the
  driver (0.1 fps). Race (driving, average of 8 samples, both with `F1CE_GL_FINISH`): vertex setup 11.7 -> 7.7 ms
  per frame, 27.4 -> 29.5 fps; not GPU-bound (~2 ms wait at the flip either way). `F1CE_VTX_CPU=1` keeps the old
  path. Next for this step: one element buffer for all index lists (the index block is now the largest part of
  vertex setup, ~2.5-4 ms: per-draw GL calls ~1.2 ms, the rest hash-map lookups and min/max scans), then textures.
  The host profiler no longer allocates while a sampled thread is suspended (it could deadlock on the heap lock).
- **Fixes after the user's test (2026-10-28):**
  - Heat haze behind the cars was a white cloud: the haze pass refracts the 1280x720 scene copy into a 640x360
    buffer, and a raw read of a target counted as a copy (not sRGB-encoded) only when it had exactly the
    destination's size; now when it is at least as large.
  - Shader stutter: a cached program cost ~10 ms (`glProgramBinary`) at its first draw. Programs used in earlier
    sessions are listed in `shader_cache/used.txt` and preloaded at start-up on a second shared context (~0.4 s
    for 184 programs, in the background); first-use cost in a race 413 -> 3 ms. Programs never seen before still
    compile in the background once (`F1CE_NO_PRELOAD` turns the preload off).
  - Quitting: `sys_process_exit` used `std::exit`, which ran static destructors while the SPU workers and the
    renderer were still running (access violations in the log after every quit); it now flushes the log and
    leaves at once.
  - The Driver garage opened fine in scripted tests; the reported crash there was not reproduced.
  - Crash entering the Driver garage (user log): a session with ~130 programs new to the cache. The compile thread
    linked in a context sharing objects with the renderer's, and the AMD driver held up every renderer GL call
    during each 50-100 ms link: 0.3 fps for tens of seconds, the PPU overran the FIFO (jump to 0, semaphore
    timeout with an address as value, fault). The compile thread now uses a context of its own share group and
    hands over the program binary; the renderer loads at most two binaries per frame. Cold-compile test
    (`F1CE_SHADER_SALT=<text>` makes every program new, with `F1CE_NO_PRELOAD=1`): 0.3 -> 26-30 fps while compiling.
    KHR_parallel_shader_compile is exposed by the driver (RX 6600) but compiles on the calling thread: worse.
  - Still seen: `glError=502` after ~100 first draws per run (present since before 2026-10-28) and FP opcode 0x3F
    (not a documented RSX opcode) in a few garage programs.
  - FIFO fault in a race (user log, no stall: ~29 fps): the front end ran a CALL at 006d9b8c with the target of two
    frames earlier and hit rewritten memory. A copy of a streamed list (taken when PUT leaves it) stayed queued
    because the front end never left it at call depth 0, and kept covering its addresses (the race streams each
    frame list from 0x55d100 to ~0x6f0000). Copies now carry the reference value of the slot PUT returned to and
    are ignored and dropped once the front end is two frames past them ("dropped as stale" in the log). Not
    reproduced in two 70 s driving runs (no stale copy occurred there).
- **Roadmap step 2 completed (2026-10-28): index lists and textures.**
  - Index arena: all index lists in one persistently mapped 96 MB buffer, bound once as the VAO's element buffer:
    a 16 MB ring (four fenced segments) for lists used by one draw and a block heap (the vertex heap's allocator,
    now generic: `BlockHeap`) for cached lists and index regions, written through the mapping. Replaces a buffer per
    list (a `glBindBuffer` per draw, `glBufferData` per rebuild). Race start: index block 3.8 -> 2.4 ms per frame.
  - Texture decoding: non-DXT formats (A8R8G8B8, B8, A4R4G4B4, A1R5G5B5, R5G6B5, G8B8, others as 32-bit) are
    deswizzled and converted by a compute shader from a staging buffer texture straight into the texture levels
    (same results as the CPU loop: menu frames identical, race identical up to animation). Race: 0.6 MB of textures
    decoded per frame cost 2.3 ms on the CPU, 0.08 ms now; large new textures no longer stall the render thread.
  - Per-unit texture memo in draw(): unchanged unit registers within a frame and a surface epoch reuse the last
    result (90% of ~19k unit binds per frame at the race start) and skip the wrap / filter maps; units are selected
    (`glActiveTexture`) only before GL calls on them; the unit loop is timed once instead of per unit.
  - Driving race (average of 14 samples): textures + uniforms 5.9-6.3 -> 5.0 ms, vertex setup 6.7-7.5 -> 6.5 ms per
    frame; at the 30 fps cap. Still on the CPU: content hashes for change detection (~1.5-2 ms), index-region
    hash maps / min-max scans (~2 ms), fragment-constant uploads per draw (~1-2 ms).
  - Scripted menu input: the Single Player menu ignores presses for ~10 s after it opens (Quick Race opens at the
    36 s press with `2:cross,12:start,16:cross,...` every 4 s); the Driver garage was entered three times with
    `...,36:cross,40:down,42:cross,...`.
- **Roadmap step 3 (2026-10-29): resolution scaling and anti-aliasing.**
  - `F1CE_SCALE=N` (1-4): local render targets, depth buffers, multisampled storage and cube maps are allocated at
    N x their guest size; viewport, scissor, resolves, surface copies (NV0039) and the presentation use the physical
    sizes; the vertex programs map window coordinates through the guest size, so geometry is unchanged.
    Main-memory targets (read back by the CPU: the per-car lighting table) stay native. Fragment programs read WPOS
    divided by the target's scale (`u_res_scale`, emitted only when N > 1, so the shader cache of scale 1 stays
    valid); occlusion counts are divided by N^2 (LOD and flare intensity come from them).
  - Presentation: with N > 1 or FXAA, a full-window pass samples a mip chain of the display buffer trilinearly at the
    level matching the window (supersampling; the linear blit skipped pixels when shrinking 4K to 720p) and
    optionally applies FXAA (`F1CE_FXAA=1`). `F1CE_MSAA` sets the samples of the antialiased targets (8x tested).
  - Results: 2560x1440 and 3840x2160 render correctly (shadows, crowd, haze, mirrors, HUD); thin structures (ladder
    rungs, poles, crowd figures) are solid and sharp after downsampling. The full grid runs at ~28-29 fps at 4K and
    the GPU (RX 6600) waits only 2.5 ms per frame at the flip: still CPU-bound; driving at 4K holds 30 fps.
  - PPU throttle while lists stream: `refs_behind()` returned 0 whenever PUT stood inside a streamed frame list (race
    frames), so a renderer stuck for a moment mid-list (a long upload, 4K) let the game rewrite lists the front end
    was still running: the FIFO faults seen in races ("unmapped IO offset"). The reference of the last ring slot PUT
    passed through is now remembered (`note_slot`, also from the list watcher) and used while streaming.
    Reproduced with `F1CE_RSX_DRAW_STALL=3100:300:3` (600 ms per frame mid-list): without the throttle 1 of 2 runs
    faulted, with it 2 of 2 completed (the PPU waited ~1.45 s per stall).
- **Fixes after the user's test at scale 2 (2026-10-29):**
  - Full screen (`F1CE_FULLSCREEN=1`, F11 or Alt+Enter) and the window keeps the image's 16:9 aspect ratio with black
    bars (`F1CE_STRETCH=1` stretches as before); `F1CE_WINDOW=<w>x<h>` sets the initial size.
  - Vertical line in the Quick Race menu at higher scales: the portrait (512x512 DXT1) is drawn 1:1 onto 512 pixels;
    at scale 2 the edge samples lie a quarter texel outside and the linear filter blended in the opposite edge of
    the repeating texture. Draws without depth test (interface, full-screen passes) keep coordinates within [0, 1]
    half a texel from the edges when scaled (`u_ui_clamp`); repeating coordinates outside [0, 1] are unchanged.
  - Shadows under the cars were black: their shadow decals decode alpha only (gamma 8) and subtract from the scene,
    and the sRGB-output rule counted any gamma bit, so their colour was encoded (0.2 -> 0.48). Only decoded colour
    channels count now (`F1CE_ALPHA_GAMMA_ENCODE=1`: the old rule). Grid screen behind the car: 3 -> 29 (RPCS3 37);
    the sides of the car are still darker (22 vs 38).
  - One shader source for all scales: the programs read the scale from uniforms, so changing `F1CE_SCALE` no longer
    compiles every program again (the Driver garage stayed white for seconds at a new scale). The first run after
    this change compiles everything once (~14 s of white screens in the first scenes). Four compile threads were
    tried: the AMD driver serialises the links (each took ~6x longer, ~15 fps while compiling), so one thread stays.
  - Open: an intermittent freeze at the race start / early race (2 of ~10 runs on 2026-10-29): the main thread waits
    in `cellSpursEventFlagWait` while SPURS workloads 0, 1, 2 and 4 stop running (6-8 go on); no warning, no FIFO or
    renderer stall. Not reproduced in six further runs.
- **Shader preparation at start-up and the race-start freeze (2026-10-29):**
  - Program database `shader_cache/programs.bin`: the guest microcode of every program pair the game uses (vertex
    program slots, fragment program bytes, shader control, cube mask) is recorded when it first appears. At start-up,
    before the renderer runs its first command, every entry is translated again with the current translator and
    loaded from the binary cache or compiled, with a progress bar in the window; get_program then finds them ready.
    No compile or binary load happens during play for anything seen before, also after a translator change, a new
    scale or a driver update (then the phase compiles: 284 programs in 79.5 s). Warm start: 281 programs in 0.7 s.
    Programs never seen still compile once in play and join the database. (The scripted test input counts from
    process start, so a long phase makes it miss the menus.)
  - Freeze at the race start / early race (main thread in `cellSpursEventFlagWait`, SPURS job managers no longer
    scheduled, their job lists not empty, ready counts 0): a lost wake-up. SPU PUTLLC compared the line data only;
    the job manager read ready count 1 and an empty queue, the game queued jobs and stored ready count 1 again (an
    unchanged value), and the PM's PUTLLC of ready 0 succeeded. On the Cell any store to the line loses the
    reservation. Each 128-byte line now has a write generation (hashed table, `core.cpp`), bumped by every write
    under the reservation lock (PPU stwcx./stdcx., SPU PUTLLC/PUTLLUC, the HLE SPURS writers: ready counts, event
    flags, queues); PUTLLC fails when it changed since GETLLAR. PUTLLC failure rate unchanged (~3%). The freeze was
    rare (3 of ~15 runs); not seen in the runs since, but not reproducible on demand.

- **FIFO fault late in a Quick Race (2026-10-30):**
  - User log: at ~30 fps with no stall the front end ran `0090502a` at 006901ec, right after a complete command: vertex
    program microcode from an older frame decoded as a CALL into the empty end of the IO area, NOPs, then a jump to an
    unmapped offset. `F1CE_FIFO_VERIFY=1` (new) re-reads each command header 4-12 ms after it ran: 655 million headers,
    none changed, so the front end does not overtake the writers.
  - Cause in the list copies. PUT stopping inside a list at an address ending in 0x060 was taken for a ring slot
    (20+ times per 90 s): such a copy had no slot header, so no reference, so it never aged, and could stay queued to
    cover later frames. A late wake-up of the watcher had the same effect: the copy ended short of the closing jump
    and took a later slot's reference. `F1CE_SNAP_LAG=40` (new, late wake-up every ~2 s) reproduced a bad jump at scale 2.
  - Fix: a copy now needs the real closing jump to PUT's slot (searched up to 2 MB from the old PUT), a poll gap below
    30 ms and a reference. The fault itself was not reproduced: its link to these copies is inferred, as the user's
    log has only the first 20 copy messages. Guard added: a jump or call into empty memory waits up to 500 ms for the
    word or target to be written, and drops the copies if a copy disagrees with memory.
  - Scripted input times now count from the end of the start-up shader phase (`host::input_clock_reset`). A held
    scripted button (`50:cross:900`) also blocks manual presses of it: the game sees no new press.
- **Freeze on the race grid (2026-10-30, fixed):**
  - User log (Monaco, at once on the grid): main in `cellSpursEventFlagWait` at 0x16ED9C, second of the three flags of
    FUN_0016ebac, which starts three TerrainCollider tasks (ELF at 0x10082500, host threads). One task sat at pc 0x12DC4
    with its instruction count unchanged. A first theory (result queues full) was wrong: the peak was 17 items of 50.
  - A task retries `QueueTryPopBegin` 100000 times, then prints "QueueTryPopBegin STUCK" (error BUSY: another task
    preempted between PopBegin and PopEnd, more likely in heavy scenes). The print routine (0x12D60) uses send_event and
    reads a second mailbox word, the completion from the PPU-side handler. Only the first word was ever written.
  - Fix: SPURS tasks get the completion word at once (`event_needs_completion`) and the text goes to the log
    ("SPU task<n> print:"). `F1CE_STUCK_TEST=1` lowers the retry limit to 2: it froze every time before the fix, and
    runs normally after it. A PPU flag wait over 3 s logs "freeze:" lines and saves `freeze_task<tid>.bin`.
  - Confirmed by the user: Monaco loads and a full lap completed without a freeze.
  - To do (user notes): mirror images are still too dark; tearing in fast scenes, vsync to implement.
