# Windows 11 and Android port plan

Status: draft plan, nothing implemented. Scope: Tears of the Kingdom and Breath of the Wild (they share
`games/totk/runtime`). Odyssey and `by3` come after, because they carry their own runtimes and copies of `d2r_gfx`.

## 1. The constraint that decides everything

The project is not an emulator. Each NSO module is rewritten ahead of time (`third_party/d2r/patch_nso.py`) and
then **runs directly on an ARM64 CPU**. That gives:

| Target | CPU | Fits the design? | Notes |
|---|---|---|---|
| Android 16/17, arm64-v8a | ARM64 | Yes | Closest to the iOS port: same CPU model, Vulkan instead of Metal |
| Windows 11 on ARM (Snapdragon X / X2 Elite, NVIDIA N1X) | ARM64 | Yes | Same rewrite approach with Windows thread and memory primitives |
| Windows 11 x86-64 (i7-5820K + RTX 3060, for example) | x86-64 | Needs a CPU backend | ARM64 → x86-64 translation by JIT first, optionally static recompilation later: section 9 |

Hardware that follows from this:
- A Windows-on-ARM machine with 32 GB RAM (Snapdragon X2 Elite or a Snapdragon dev kit) to run Windows builds.
- An Android phone with a Snapdragon 8 Elite (Gen 5 preferred), **12 GB of RAM or more** (16 GB preferred), Android 16 or newer.
- An x86-64 PC can cross-compile every target and can develop the Vulkan backend through a replay harness (phase 4).
  Once the x86-64 JIT backend exists (phase X2) it runs the game too. The Android emulator on x86-64 hosts runs no
  arm64 code, so it does not help with Android.

## 2. What is macOS-specific today (TOTK runtime)

Measured in `games/totk/runtime` and `games/totk/third_party`:

| Area | Today | Size |
|---|---|---|
| Graphics backend | `nvn_mtl.m` (Metal 4 + MetalFX), `totk_mfx*.m`, `totk_taa.m`, `totk_shader_rt.m`, `nvn_refl.m` | ~1.6 MB of Objective-C; `nvn_mtl.m` alone is 1.47 MB |
| NVN HLE layer | `hle_nvn.c`, `nvn_dirty.c`, `nvn_tex.c`, … | Plain C. Talks to the backend through ~40 `mtl_*` functions in `nvn_obj.h`: **this is the seam** |
| Shader chain | Ryujinx translator → SPIR-V → SPIRV-Cross → MSL → Metal | SPIR-V is already the intermediate form; Vulkan drops SPIRV-Cross |
| Thread slots | `tpidrro_el0` + Darwin TSD in `gate.S`, `hle_stack.S` and in the code `patch_nso.py` emits | 65 hits / 9 files |
| Memory | `mach_vm_remap` aliasing, `vm_region` queries, `MAP_FIXED`, 16 KiB pages | `loader.c`, `services.c`, `nvn_mtl.m` |
| Faults and preemption | `sigaction` (SEGV/BUS/ILL/TRAP), `SIGUSR2` preemption, Mach `thread_get_state` | `loader.c`, `threads.c` |
| Thread info and QoS | Mach `thread_info`, time-constraint policy, `os_unfair_lock` (93 hits), GCD `dispatch_*` (172 hits) | `threads.c`, `hle_nvn.c`, `nvn_mtl.m` |
| Time | `mach_absolute_time` | 20 hits |
| Window, input, audio | AppKit `frontend.m`, GameController `gamepad*.m`, Core Audio, UIKit on iOS | ~120 KB of Objective-C |
| Assembly | Mach-O GNU asm (`nvn_thunks.S` 160 KB, `gate.S`, `thread_enter.S`, `hle_stack.S`) | `_sym` names, `@PAGE/@PAGEOFF` relocations |
| Build | `build_app.py` (124 KB): `xcrun`, `codesign`, APFS cloning, Metal toolchain, `.app`/`.ipa` | Host and target logic mixed together |
| CI | `macos-latest` only | |

Portable already: the C services (fs, romfs, ipc, audio renderer and mixer, opus, libvpx movies, HID logic),
the Python extraction, verification and rewrite pipeline, the Ryujinx translator (.NET 10 NativeAOT), BC texture
decoders, and the iOS work on 16 KiB pages and on code that is not JIT.

## 3. Target stack (latest tooling)

| Concern | Choice | Why |
|---|---|---|
| Graphics | **Vulkan 1.4** (1.3 baseline plus extensions) | One backend for Windows ARM (Adreno X2, NVIDIA) and Android. The Ryujinx translator already has a Vulkan SPIR-V target |
| Platform layer | **SDL 3.4** | Window, swapchain surface, gamepad (XInput/GameInput/DualSense/Switch Pro/Android), audio (WASAPI/AAudio), text input on both targets |
| Compiler | LLVM/Clang 21+ (`aarch64-pc-windows-msvc`, NDK clang) | One compiler family. GNU asm syntax survives with macros |
| Build | CMake 4 + Ninja with `CMakePresets.json` (`macos-arm64`, `windows-arm64`, `android-arm64`) | Replaces the `xcrun` calls inside `build_app.py` |
| Windows SDK | 10.0.26100+ (Windows 11 24H2) | `VirtualAlloc2`/`MapViewOfFile3` placeholders, Dev Drive block cloning |
| Android | NDK r29, AGP 9 / Gradle 9, JDK 21, `compileSdk`/`targetSdk` 36+, `minSdk` 35, GameActivity (AGDK) | ADPF performance hints, 16 KiB page support (already handled) |
| Shader translator | .NET 10 NativeAOT: `win-arm64`, `win-x64` (host tools), `linux-bionic-arm64` (on device) | Same code; only the RID changes |
| Upscaling | Phase 1: the game's own EASU/linear. Later: FSR 3.1 (Vulkan) on Windows, Snapdragon GSR 2 on Android | Replaces MetalFX spatial/temporal; frame generation is deferred |
| Python | 3.14 on every host (`compression.zstd` is already required) | Unchanged |

## 4. Architecture changes

1. **`platform/` layer** (C headers, one implementation per OS):
   `pf_mem` (reserve, commit, alias, protect, query), `pf_thread` (create, slot base, suspend/context, priority),
   `pf_fault` (fault handler registration), `pf_time`, `pf_lock` (replaces `os_unfair_lock`), `pf_task`
   (replaces the GCD queues used), `pf_paths` (profile, cache and log folders).
2. **GPU backend interface**: make the ~40 `mtl_*` functions in `nvn_obj.h` an explicit `gfx_backend.h`
   (rename to `gb_*`). Metal stays one backend and Vulkan becomes a second, compiled per target.
3. **Rewriter target profiles**: `patch_nso.py` takes `DM_TARGET=apple|win-arm64|linux-arm64` and emits the matching
   thread-slot access sequence:
   - Apple: `mrs xN, tpidrro_el0` → TSD slot (today).
   - Windows ARM64: `x18` is the TEB (the OS keeps it), so `ldr xN, [x18, #TlsSlots + i*8]` using `TlsAlloc` slots.
   - Android: `mrs xN, tpidr_el0` → bionic TCB → one reserved slot (`TLS_SLOT_APP`) holding the runtime's
     per-thread block; the key page adds a single indirection.

   Each target gets its own `expected-<ver>-<target>.json`, because the rewritten bytes differ.
4. **Code loading**: Windows uses `VirtualAlloc` + `VirtualProtect` RX. Android loads the code into anonymous memory
   and then sets it RX. SELinux `execmem` allows this for apps; `execmod` on file-backed text does not, so code is
   never patched in place inside a mapped `.so`.
5. **Frontends**: one SDL3 frontend (`frontend_sdl.c`) for Windows and Android, with a touch overlay ported from
   `ios/Sources/touch_pad.m` for Android. macOS keeps AppKit at first; the SDL3 frontend can then replace it.

## 5. Phased plan

Each phase ends in something that works and can be tested. Estimates count engineering weeks for one senior
engineer familiar with the code. They are rough.

### Phase 0: Decisions and hardware (week 0)
- [ ] Confirm the targets: Windows x86-64 (JIT first, section 9), Windows on ARM64, Android arm64.
- [ ] Get the hardware listed in section 1.
- [ ] Spike checklist (section 7): run every spike before committing to phase 4.

### Phase 1: Platform abstraction on macOS, with no change in behavior (3-4 wk)
- [ ] Add `runtime/platform/` with the macOS implementation behind the `pf_*` API; move every Mach, GCD,
      `os_unfair_lock` and `sigaction` call behind it.
- [ ] Extract `gfx_backend.h` from `nvn_obj.h`; Metal implements it unchanged.
- [ ] Turn the assembly into portable macros: `SYM()`, `PAGE()/PAGEOFF()`, `.type`/`.size`, and the thread-slot
      load as one macro per target.
- [ ] Audit the C for LLP64 and ABI issues: `long` used as 64-bit (Windows), `char` signedness (Linux AArch64 is
      unsigned), Apple variadic convention assumptions.
- [ ] Acceptance: `make check` passes, a Mac TOTK 1.4.3 build hashes byte-identical, and FPS is within noise.

### Phase 2: Build system split (2-3 wk)
- [ ] Split `build_app.py` into `pipeline/` (extract, verify, rewrite, seed: works on any host) and
      `packagers/{macos,ios,windows,android}.py`.
- [ ] CMake + presets for the runtime, SPIRV-Cross (Metal only), hactool, opus and libvpx (vcpkg or pinned
      sources).
- [ ] Run the Windows host build: Python 3.14 on Windows, paths, block cloning on Dev Drive (ReFS) with a
      copy fallback, hactool built with clang.
- [ ] CI: add `windows-11-arm` and `ubuntu-24.04-arm` GitHub runners for compile + unit tests; keep
      `macos-latest`.
- [ ] Acceptance: from a Windows x86-64 host, the pipeline produces verified rewritten modules and RomFS
      for TOTK 1.4.3.

### Phase 3: Rewriter target profiles (2 wk)
- [ ] `DM_TARGET` in `patch_nso.py`, `call_window.py` and the gate/stub emitter.
- [ ] Rerun the existing site-by-site equivalence check (`equiv.py`, with negative controls) for every target.
- [ ] Generate `expected-*-win-arm64.json` and `expected-*-linux-arm64.json`.
- [ ] Acceptance: 1.0.0/1.2.1/1.4.3 pass equivalence on all three targets; the Apple output is unchanged.

### Phase 4: Vulkan backend (12-20 wk, the critical path)
- [ ] **Replay harness first**: record a submitted NVN command stream with its memory-pool snapshots on Mac, then
      replay it headless against `gfx_backend`. This lets Vulkan work happen on any GPU, including the RTX 3060,
      and gives golden-image tests.
- [ ] Device setup: Vulkan 1.3 baseline; require `dynamic_rendering`, `synchronization2`, `descriptor_buffer`
      or `push_descriptor`, `buffer_device_address`, `extended_dynamic_state3` where present.
- [ ] Guest memory → GPU: `VK_EXT_external_memory_host` import, else `VK_EXT_map_memory_placed` at the guest
      address, else staged copies driven by `nvn_dirty.c` (the slow path).
- [ ] Shaders: Ryujinx `TargetApi.Vulkan` SPIR-V straight from `totk_xlate`; move every MSL-level patch
      (early fragment tests, unwritten vertex outputs, visibility-counter lowering) to the translator or SPIR-V level.
- [ ] Pipelines: `VK_KHR_pipeline_binary` / `VkPipelineCache` replaces the Metal pipeline archive; the shader
      list and seed format gain a Vulkan chain name.
- [ ] Render passes and G-buffer self-reads: `VK_KHR_dynamic_rendering_local_read` (core in 1.4) or
      `VK_EXT_attachment_feedback_loop_layout`, replacing `[[color(n)]]` tile reads.
- [ ] Textures: native BCn where supported; reuse the `nvn_bctc` / `bc1astc` transcoders (written for iOS) for
      GPUs without BC.
- [ ] Present: `VK_KHR_swapchain` + `present_id2/present_wait2` for frame pacing; the game's own upscale at first.
- [ ] Acceptance: the replay harness matches the Metal golden images (tolerance-based) across 20+ captured scenes.

### Phase 5: Windows 11 ARM64 runtime (6-8 wk, parallel with phase 4 once phase 1 is done)
- [ ] `pf_mem` on `VirtualAlloc2` placeholders + `CreateFileMapping`/`MapViewOfFile3` for aliasing (reference:
      Ryujinx `MemoryManagementWindows`/`PlaceholderManager`, which aliases at 4 KiB granularity on Windows).
- [ ] Guest layout below 2^39, reserved at startup before other allocations take the space.
- [ ] `pf_fault` via a vectored exception handler (VEH); dirty tracking via `VirtualProtect` or `MEM_WRITE_WATCH`.
- [ ] Guest stacks: update the TEB `StackBase`/`StackLimit` on every switch to a guest or host stack. Exception
      dispatch rejects an SP outside them (`STATUS_BAD_STACK`).
- [ ] Preemption: `SuspendThread` + `Get/SetThreadContext` instead of `SIGUSR2`; priorities via MMCSS
      (`AvSetMmThreadCharacteristics`).
- [ ] SDL3 frontend, audio and gamepad; save/cache paths under `%LOCALAPPDATA%\MacNativeGames\<game>`.
- [ ] Package as a portable folder first, then MSIX (self-signed) as an option.
- [ ] Acceptance: TOTK 1.4.3 boots to the title on Metal-free code, then Great Sky Island playable at 30+ fps on
      Snapdragon X2 Elite.

### Phase 6: Android runtime (8-10 wk, after phase 5 reaches the title screen)
- [ ] App shell: GameActivity + SDL3, `arm64-v8a` only, `largeHeap`, keep-screen-on, ADPF hint sessions.
- [ ] Memory: reserve the guest region in `JNI_OnLoad` before anything else grows; handle 39-bit VA kernels;
      memory budget checks with a clear refusal on phones under 12 GB.
- [ ] Code: rewritten modules loaded into anonymous memory, then RX (no `execmod`).
- [ ] Faults, preemption and threads: Linux signals (very close to the macOS code), `tgkill` for preemption.
- [ ] Storage: RomFS (~17 GB) and the shader seed in app-specific external storage
      (`/Android/data/<pkg>/files`), pushed by a host tool over `adb` (`tools/install_android.py`, like
      `install_device.py` for iOS). The APK carries only code and small assets.
- [ ] Shader translation on device: `totk_xlate` as `linux-bionic-arm64` NativeAOT `.so`; the seed is SPIR-V
      prepared on the PC.
- [ ] Touch overlay (port of `touch_pad.m`), gamepad via SDL3, AAudio via SDL3.
- [ ] Thermal: default 30 fps cap, 960x540 internal like the iPhone build.
- [ ] Acceptance: TOTK 1.4.3 playable on a Snapdragon 8 Elite (16 GB) at 30 fps after warm-up.

### Phase 7: Parity and hardening (4-6 wk)
- [ ] Shader-list contributions: the Vulkan chain gets its own list section; keep the GitHub issue flow on Windows.
- [ ] Mods, DLC, update merging and save import/export on the new targets.
- [ ] FSR 3.1 / GSR 2 upscaling; frame generation evaluated afterwards.
- [ ] BotW 1.6.0/1.9.0 on the new targets (shared runtime, little extra work).
- [ ] Docs: README sections and requirements for Windows and Android.

### Phase 8: Other games (separate decision)
- [ ] Deduplicate the three copies of `d2r_gfx` (`totk`, `odyssey`, `by3`) into one shared module first, then
      reuse the Vulkan backend.

**Total (ARM64 targets only)**: about 9-12 months for one engineer to reach Android + Windows-on-ARM parity for
TOTK/BotW; phase 4 dominates. Two engineers (one on graphics, one on platform) bring it to about 6 months. With
Windows x86-64 added (phases X0-X4), see the totals in section 9.6.

## 6. Dependency graph

```
P0 ─► P1 ─┬─► P2 ─► P3 ─┬─► X1 ─► X2 (Windows x64, JIT) ─┬─► P5 (Windows ARM) ─► P6 (Android) ─► P7 ─► P8
          │      X0 ────┘                                 │
          └─► P4 (Vulkan, via replay harness) ────────────┘          X2 ─► X3 (AOT, optional) ─► X4
```

P4 can start right after P1 (backend interface + replay harness). X2 is the first point where the whole game runs
on hardware you already own, so it validates P4 end to end. P5 reuses most of X2's Windows work. P5 and P6 need
P3's rewritten modules and P4's backend to reach gameplay, but their platform layers can be built and unit-tested
earlier. X3 is independent of P5/P6 and can run in parallel with them.

## 7. Spikes to run first (about 2 weeks total)

| # | Question | Risk if no |
|---|---|---|
| S1 | Does Adreno (X2 and 8 Elite) expose `VK_EXT_external_memory_host` or `VK_EXT_map_memory_placed`? | Guest memory pools need a copy path: slower, more complex dirty tracking |
| S2 | Can Windows ARM64 alias guest pages at 4/16 KiB granularity with placeholders as the runtime needs? | Rework of virtual memory pools |
| S3 | Does a guest thread with the TEB-x18 slot sequence run 10^6 svc round-trips under VEH with no `STATUS_BAD_STACK`? | Thread model redesign on Windows |
| S4 | On Android, can a zygote-forked process reserve ~16 GB contiguous below 2^39 on a 39-bit VA kernel? | Smaller alias region (the runtime already shrinks down to 8 GB) or no-go on some kernels |
| S5 | Is `TLS_SLOT_APP` (or an equivalent fixed TCB offset) usable from app code on Android 16/17? | Extra indirection or a different slot scheme in the rewriter |
| S6 | Does the Ryujinx Vulkan SPIR-V path compile the 1.4.3 seed (~18k variants) on Adreno without driver failures? | Shader workarounds per driver |

## 8. Main risks

- **Graphics effort**: `nvn_mtl.m` holds two years of correctness fixes tied to Apple GPU behavior. Many become
  unnecessary on Vulkan (tile memory, MSL quirks), but each has to be rechecked. The replay harness is the mitigation.
- **Android memory**: TOTK commits about 3.3 GB of guest memory plus host and GPU memory; the low-memory killer
  and thermal limits make 12 GB the practical minimum.
- **Android drivers**: Adreno proprietary driver bugs. Mali/Xclipse are out of scope at first.
- **Distribution**: legal posture is unchanged (the user builds from their own files), but sideloading a 17 GB
  data set on Android needs a good host tool.
- **Maintenance**: three backends (Metal, Vulkan, and the iOS variants) multiply test work; golden-image replay
  CI keeps it in check.

## 9. Windows x86-64

On an x86-64 CPU the game's ARM64 code cannot run as is. Something has to turn it into x86-64 code: ahead of time
(**static recompilation**, AOT) or while the game runs (**JIT**). Everything else in this plan stays: the HLE services,
the NVN layer, the Vulkan backend (phase 4), SDL3, the build pipeline. What changes is the layer between the game's
code and the host.

**Recommendation: build the JIT first (X1-X2), designed so that AOT can be added (X3-X4) behind the same interface,
with the JIT as the fallback for anything AOT missed.** The JIT puts the game on your own PC within months and turns
it into a full test machine for the Vulkan backend. AOT is then an optimization with a safety net, not an
all-or-nothing research bet.

### 9.1 Work shared by both approaches

These are needed whichever way the code is translated, and they come first.

1. **CPU backend interface** (`runtime/cpu/cpu_backend.h`): one guest CPU context per guest thread (x0-x30, sp, pc,
   v0-v31 as 128-bit, NZCV, FPCR/FPSR, TPIDR_EL0, TPIDRRO_EL0), plus `run(ctx)`, `halt(thread)`, `invalidate(range)`.
   Three implementations:
   - `native` (ARM64 hosts: today's rewritten code, unchanged);
   - `jit` (x86-64);
   - `aot` (x86-64, later).
2. **The guest stays at its own addresses.** Reserve the guest layout below 2^39 at the same virtual addresses on the
   x86-64 host. Windows x64 gives 128 TB of user space, and DLLs and heaps go high under ASLR (link with
   `/HIGHENTROPYVA` and reserve at the first constructor).

   Guest pointers then stay valid host pointers. `hle_nvn.c`, `fs_host.c`, the IPC code and the audio renderer keep
   reading guest memory directly, so the HLE code needs almost no change. This is the biggest saving available.
3. **Guest↔host ABI bridge**, generated rather than hand-written:
   - **Guest → host**: about 520 NVN entry points (`names.txt`, `nvn_protos.h`, today the AAPCS64 thunks in
     `nvn_thunks.S` from `gen_protos.py`). Add the svc gates and the host-native cuts (`svc #0x80+`). Each becomes a
     stub that reads arguments from the guest context and calls the host C function with the Windows x64 ABI. Floats
     and structs passed by value need care in the generator.
   - **Host → guest**: thread entry points, `nn::init::Start`, and any callback the game registers. Each becomes a
     call into the backend with a prepared context.
   - `gen_protos.py` gains an `--abi x64-bridge` mode; the same generator emits both forms.
4. **Struct layout audit**: guest structs are LP64 and Windows x64 is LLP64. Any host code that maps a guest struct
   with `long` or `size_t` assumptions breaks. This is the same audit as phase 1, with a static-assert per mirrored
   struct.
5. **Rewriter profile `DM_TARGET=x64`**: apply only the registered cuts and the game patches. Drop the x18, TLS, svc
   and counter rewrites, because the backend now handles those instructions itself.

   One catch: `game_patches*.c` writes instruction words at load time and some are switchable by environment
   variable (`TOTK_FPS60=0`). The JIT handles that for free, since guest code is just data. AOT has to lift both
   variants of each patched site behind a runtime flag, or fix them at build time.
6. **Counters and time**: `cntpct_el0`/`cntvct_el0` reads return `QueryPerformanceCounter` scaled to the Switch's
   19.2 MHz. `cntfrq_el0` is a constant.
7. **Preemption**: there is no signal into running guest code anymore. The JIT exits at block boundaries on a halt
   request; AOT code polls a per-thread flag at loop back-edges and function entries.
8. **Host CPU baseline: x86-64-v3** (AVX2, FMA3, BMI2, LZCNT/POPCNT). Your i7-5820K (Haswell-E) meets it. FMA3
   matters: ARM64 fused multiply-add must stay fused to match results bit for bit.
9. **Graphics**: the phase 4 Vulkan backend unchanged; the RTX 3060 is a first-class target. BCn is native, and
   `VK_EXT_external_memory_host` is available on NVIDIA, which makes the guest-memory import from spike S1 simple
   here.

### 9.2 Option A: JIT (dynarmic)

**What it is.** [dynarmic](https://github.com/merryhime/dynarmic) is a mature A64 → x86-64 dynamic recompiler
(0BSD license) used by yuzu and its successors. It translates guest basic blocks to x86-64 on first execution,
caches them, and calls back into the host for:
- memory (fastmem with a fixed base, here 0 because the guest keeps its addresses);
- `svc`;
- system registers;
- exclusive monitors.

**How it fits here.**
- One `Dynarmic::A64::Jit` per guest thread, sharing one exclusive monitor. Callbacks map to the existing gate
  handlers in `threads.c`/`services.c`, which already dispatch by svc number.
- `TPIDR_EL0`/`TPIDRRO_EL0` are context fields; no code rewriting is needed.
- NVN calls: the bootstrap loader hands the guest addresses of small `svc #hle` stubs, and the bridge from 9.1
  dispatches them.
- Code cache invalidation only for the load-time game patches, which are applied before the first run anyway.

**Performance.** yuzu-class JIT on Haswell-E: plan for **30 fps** in TOTK, with dips in dense areas. 60 fps is not
expected on a 2014 CPU. Fastmem keeps memory access near native. Costs come from flag computation, FP edge cases
and block linking.

Stutter has two sources: first-time translation of new code, and shaders. dynarmic has no persistent code cache, so
add one as an optional later improvement, or rely on AOT (9.3) for that.

**Effort.** About 3-4 months to boot and play on x86-64, given that the HLE runtime and the Vulkan backend exist.
- Integration: about 4-6 weeks.
- ABI bridge generator: about 3 weeks.
- Threading and preemption on the new backend: about 3 weeks.
- Debugging game-specific JIT issues: open-ended but bounded; yuzu ran these games.

**Risks.**
- Upstream maintenance: dynarmic upstream is quiet. Pin a commit or a maintained fork, and expect to own fixes.
- "Not an emulator" positioning: the CPU path on x86-64 becomes an emulator CPU core. Everything else stays HLE and
  native. Say so plainly in the README.
- FP accuracy settings: dynarmic's fast-FP options trade accuracy for speed. Start accurate, then measure.

### 9.3 Option B: Static recompilation (AOT)

**What it is.** At build time, on the player's own machine, lift every reachable function of `main`, `sdk` and
`subsdk0` to C or LLVM IR with ARM64 semantics, compile it to x86-64, and link it into the game executable. This is
in the style of N64Recomp, XenonRecomp and ps3recomp. It fits this project's model exactly: a build step that turns
the player's files into a native program, verified the same way the rewriter is today.

**Pipeline.**
1. **Discovery.** Find every function and block entry:
   - the exported and imported symbols of `sdk`/`subsdk0`;
   - `bl` targets (recursively);
   - pointers in `.rodata`/`.data` (vtables);
   - RELA and RELR relocation targets (`relr.py` already decodes them);
   - jump tables behind `br xN` (pattern recovery: `adrp`/`add`/`ldr[b|h|sw]`/`add`/`br`);
   - targets observed at run time by the JIT (see 9.4).
2. **Lifting.** One C function per guest function over the context struct, with registers promoted to locals inside
   the function.
   - NZCV is computed lazily.
   - NEON maps to SSE4.1/AVX2 intrinsics.
   - FP honours FPCR: flush-to-zero, default NaN, rounding mode. `FRECPE`/`FRSQRTE` estimates use exact tables.
   - LSE atomics map to `lock cmpxchg`/`xadd`.
   - LDXR/STXR pairs map to a compare-and-swap reservation, with the known ABA caveat that emulators accept.
   - x86-64's stronger ordering (TSO) means plain loads and stores need no fences.
   - Writing the lifter in Python on top of the existing capstone tooling is fine for generation speed. The
     semantics live in a C header of instruction helpers that is tested on its own.
3. **Dispatch.** Direct calls become C calls. Indirect calls and returns go through a guest-address → host-function
   table (a perfect hash built at link time). A miss falls back to the JIT (Option A) for that address and logs it.
4. **Compilation.** clang `-O2`, sharded by function across all cores, with a cache keyed by the hash of each
   function's guest bytes. Expect the generated C to be several times the guest code size, and a first build that
   takes tens of minutes to hours on a 6-core CPU. Measure this in spike X-S3.
5. **Verification.** Reuse the culture of `equiv.py`: run each lifted function differentially against Unicorn with
   random register and memory states. Compare registers, flags, vectors, memory writes and faults, with mutation
   negative controls. Add per-instruction semantic tests of the helper header against Unicorn.

   A version is admitted when the hashes of its generated sources match a committed manifest
   (`expected-<ver>-x64.json`), as the rewritten modules are today.

**Performance.** Best achievable on x86-64: no translation at run time, whole-function optimization, and no
first-run CPU stutter. Plan for **60 fps where the GPU allows** on a modern CPU, and a clear gain over the JIT on
Haswell-E. Bit-exact FP and flags cost some of that back.

**Effort.** About 6-9 months after the JIT exists, about 12+ months without one.
- Lifter and semantic helpers: about 3 months.
- Discovery, jump-table recovery and dispatch: about 2 months.
- Build integration and caching: about 1 month.
- Verification harness: about 1-2 months.
- Game-specific correctness work: open-ended.

Without the JIT fallback, every missed indirect target is a crash, so discovery has to be complete. That is the
hard, long tail of every static recompilation project.

**Risks.**
- **Coverage**: incomplete indirect-branch discovery. The JIT fallback contains it.
- **Build cost** for players: hours of compile time and GBs of intermediate files. Shard, cache, and allow a
  precompiled-headers or LLVM IR path.
- **Per-version and per-mod cost**: every game version, and every code mod (`--mod` main patches), needs a new
  lift and a new manifest. Cache by function hash, so only changed functions recompile.
- **Semantic corners**: FP NaN propagation, exclusive-monitor timing, self-checks of code bytes (none known in these
  games; verify).

### 9.4 Comparison

| Criterion | JIT (dynarmic) | Static recompilation | Hybrid (AOT + JIT fallback) |
|---|---|---|---|
| Time to first playable on x86-64 | **3-4 months** | 12+ months | 3-4 months (JIT), AOT added later |
| Steady-state CPU performance | Good (30 fps class on Haswell-E) | **Best** | Best for covered code |
| First-run CPU stutter | Yes (block translation) | **None** | Only on misses |
| Correctness risk | Low (proven on these games) | High until coverage is complete | Low |
| Work per new game version | **None** (patch addresses only) | Re-lift, re-verify, new manifest | Re-lift; JIT covers gaps meanwhile |
| Code mods | **Work as-is** | Need a re-lift | Re-lift or fall back to JIT |
| Player build time | Minutes (as today) | +tens of minutes to hours | Same as AOT, optional |
| Fits "not an emulator" | No (CPU is emulated) | **Yes** (native program from your files) | Mostly |
| Third-party dependency | dynarmic (owned fork) | LLVM/clang only | Both |
| Reuse on ARM64 targets | None needed | None needed | None needed |

Hybrid discovery bonus: the JIT logs every block entry it translates. Like the shared shader list, players can
contribute those **code-coverage lists** (guest addresses only, no game data). The AOT build then reads them, and
coverage converges across the community instead of per player.

### 9.5 Phases

These slot into section 5 after phase 1. Phase 4 (Vulkan) runs in parallel and needs X1 to test end to end on an
x86-64 PC.

#### Phase X0: Spikes (2-3 wk)
| # | Question | Risk if no |
|---|---|---|
| X-S1 | Can a Windows x64 process reserve the full guest layout below 2^39 at startup, every launch? | Base-offset fastmem: every host HLE access to guest memory must translate pointers (large change) |
| X-S2 | Does dynarmic (pinned commit) run TOTK `sdk` init and `nn::init::Start` to the first NVN call, with the gate handlers reused? | Choose a different JIT (Ryujinx ARMeilleure, MIT, via NativeAOT) |
| X-S3 | Lift 1,000 random `main` functions to C: generated size, clang time per function, Unicorn differential pass rate | Recalibrate the AOT estimate or drop AOT |
| X-S4 | What fraction of `br`/`blr` targets does static discovery find, measured against a JIT-logged play session? | Coverage lists become mandatory, not a bonus |

#### Phase X1: CPU backend interface + ABI bridge (5-6 wk)
- [ ] `cpu_backend.h`; the current ARM64 path becomes the `native` backend with no behavior change (Mac hash parity).
- [ ] `gen_protos.py --abi x64-bridge` for NVN; bridge tables for svc gates and host cuts; host → guest entry.
- [ ] `DM_TARGET=x64` rewriter profile (cuts and patches only) + `expected-*-x64.json`.
- [ ] Guest-at-own-address reservation on Windows x64 (`VirtualAlloc2` placeholders, as in phase 5).
- [ ] Acceptance: an x64 unit test calls 50 NVN entry points through the bridge from a synthetic guest context and
      matches the ARM64 thunk results.

#### Phase X2: JIT backend, Windows x64 playable (8-10 wk)
- [ ] dynarmic integration: per-thread JIT, shared exclusive monitor, fastmem base 0, svc/sysreg/counter callbacks.
- [ ] Threading: halt-based preemption, core affinity emulation as in `threads.c`, MMCSS priorities.
- [ ] Windows x64 build preset; same SDL3 frontend and Vulkan backend as phase 5.
- [ ] Coverage logging (block entries per module, deduplicated), written next to `shader-list-contribution.txt`.
- [ ] Acceptance: TOTK 1.4.3 Great Sky Island playable at 30 fps on i7-5820K + RTX 3060; BotW 1.6.0 boots.

#### Phase X3: AOT recompiler (16-24 wk, optional, after X2)
- [ ] Lifter + semantic helper header with per-instruction Unicorn tests.
- [ ] Discovery: symbols, call graph, data pointers, RELA/RELR, jump tables, coverage lists.
- [ ] Dispatch table + JIT fallback on miss; miss logging.
- [ ] Sharded, cached compile in the build pipeline (`pipeline/recompile.py`).
- [ ] Per-function differential verification + manifests per version.
- [ ] Acceptance: 1.4.3 runs with fewer than 0.1% of executed blocks on the JIT fallback, and at least 25% higher
      average CPU frame-time headroom than X2 in the same scene.

#### Phase X4: Performance and polish (4-6 wk)
- [ ] Persistent JIT cache (if AOT is not adopted) or AOT by default with the JIT as fallback.
- [ ] Profile FP accuracy options; vectorization of hot NEON paths.
- [ ] Windows x64 packaging, README section, minimum requirements (x86-64-v3, 16 GB RAM, Vulkan 1.3 GPU with
      `external_memory_host`).

**Estimates.** X0-X2: about 4 months on top of phases 1 and 4. X3-X4: about 5-7 months more.

### 9.6 What this changes in the overall plan

- **Order**: X1-X2 move ahead of phases 5 and 6. The x86-64 JIT turns your current PC into a full test machine for
  the Vulkan backend and the SDL3 frontend before any ARM hardware arrives. Most of phase 5 (memory placeholders,
  VEH, SDL3, packaging) is shared with the x64 build.
- **Windows becomes two builds**: `windows-x64` (JIT or AOT backend) and `windows-arm64` (native backend). They share
  everything except the CPU backend.
- **Totals** (one engineer): about 12-15 months for Windows x64 (JIT), Windows ARM64 and Android at TOTK/BotW parity;
  about 18-22 months including AOT. With two engineers (graphics; platform + CPU), roughly 8-9 and 12-14 months.
