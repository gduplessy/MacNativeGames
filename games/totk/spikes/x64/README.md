# Windows x64 spikes (phase X0)

Throwaway probes that answer the X0 spike questions in
[the port plan](../../../../docs/windows-android-port-plan.md#phase-x0-spikes-2-3-wk). They need no game files.

```bat
games\totk\spikes\x64\build.cmd
```

`build.cmd` needs Visual Studio 2022 Build Tools (MSVC x64) and the two dependencies below in `third_party/`
(ignored by git):

```sh
git clone https://github.com/lioncash/dynarmic.git third_party/dynarmic   # archived; pinned at a41c380 (2024-03-03)
# Boost 1.89.0 headers only (dynarmic uses boost/icl and boost/variant)
curl -LO https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.gz   # sha256 9de758db755e8330a01d995b0a24d09798048400ac25c03fc5ea9be364b13c93
mkdir -p third_party/boost && tar -xzf boost_1_89_0.tar.gz -C third_party/boost --strip-components=1 boost_1_89_0/boost
cmake -S third_party/dynarmic -B build/dynarmic -G Ninja -DCMAKE_BUILD_TYPE=Release -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON \
  -DDYNARMIC_TESTS=OFF -DDYNARMIC_WARNINGS_AS_ERRORS=OFF -DDYNARMIC_FRONTENDS=A64 -DBoost_INCLUDE_DIR=third_party/boost \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/dynarmic
```

Each probe prints `key=value` lines and ends with `RESULT pass` or `RESULT fail <first failure>` (exit code 0 / 1).

## X-S1: guest layout below 2^39 (`guest_layout_probe.c`)

Reserves the macOS layout of `runtime/services.c` at the same addresses, then exercises it at Horizon's 4 KiB page
size:
- **layout**: stack+heap 6 GiB first fit from `0x3_0000_0000`; alias 64 GiB at `0x70_0000_0000`, else side by side,
  shrinking to 8 GiB; modules 512 MiB below 2^39;
- **T1**: 4 KiB commit, decommit and recommit-is-zero;
- **T2**: 4 KiB protection plus VEH fault-and-resume;
- **T3**: two views of one pagefile section in placeholders at a 4 KiB offset;
- **T4**: Vulkan after the reservation, importing guest memory with `VK_EXT_external_memory_host`.

Results on i7-5820K, RTX 3060, Windows 11 Pro 26100, MSVC 14.44 (2026-10-23):

| Check | Result |
|---|---|
| Fixed layout (alias at `0x70_0000_0000`), `/HIGHENTROPYVA:NO` | **50 / 50** launches |
| Fixed layout, default `/HIGHENTROPYVA` | 49 / 50; the side-by-side fallback served the other launch; all 50 passed |
| Below 2^39 at process start (`/HIGHENTROPYVA:NO`) | only ranges below 2 GiB (5 MiB) |
| Below 2^39 at process start (`/HIGHENTROPYVA`) | about half the launches have 1-2 thread stacks there, sometimes inside `0x70..0x80_0000_0000` |
| T1 4 KiB commit / decommit / zero | pass |
| T2 4 KiB protect + VEH resume | pass |
| T3 section views at 4 KiB offset and address, coherent | pass (no 64 KiB alignment needed with placeholders) |
| T4 Vulkan device after the reservation | NVIDIA GeForce RTX 3060, Vulkan 1.4.351 |
| T4 `VK_EXT_external_memory_host` | yes, minimum alignment 4 KiB; heap and alias-view imports both pass |
| T4 `VK_EXT_map_memory_placed` | not offered |
| System DLLs loaded after the reservation (d3d12, dxgi, xinput, winmm, ws2_32, mmdevapi, hid) | 7 / 7 |

**Answer: yes.**
- Link the game with `/HIGHENTROPYVA:NO`: thread stacks and heaps then stay below 2 GiB and the fixed layout lands
  on every launch.
- Keep the side-by-side fallback as it is.
- Guest pointers can stay host pointers on x64, and the GPU can use guest memory in place.

## X-S2: dynarmic through a synthetic boot to the first NVN calls (`jit_boot_probe.cpp`)

**Setup.**
- A guest program, generated with oaknut at its own guest address, follows the shape of `nn::init::Start` up to
  the first NVN calls.
- One `Dynarmic::A64::Jit` per guest thread, with fastmem base 0 (guest address = host address) and 39 address bits.
- `svc` is recorded in `CallSVC` and served outside `Run()`, as modern yuzu does.

| Check | Result |
|---|---|
| B1 `TPIDRRO_EL0` → TLR, `TPIDR_EL0` round trip, `x18` as a plain register | pass |
| B2 svc `GetInfo` / `SetHeapSize`; the host reads the guest heap by plain pointer | pass |
| B3 `CNTPCT_EL0` at 19.2 MHz (QPC), monotonic | pass |
| B4 `FMADD` (double), `FMLA` 4S (denormal input included), bit-identical to `fma`/`fmaf` | pass |
| B5 `nvnBootstrapLoader` → `svc #0x8000+i; ret` stubs → signature bridge → host C | pass: 5 functions, a float argument in `s0`, and a host → guest callback with 6 arguments (state saved and restored) |
| B6 load from reserved, uncommitted memory → fastmem fault → callback fallback | pass |
| T1 4 threads x 200k each of LDAXR/STLXR (64), LDXR/STXR (32), LDAXP/STLXP, under contention | exact (800,000 each); 1.26-1.86 s for 2.4M contended exclusive operations |
| L1 ARMv8.1 LSE `LDADDAL` | **not translated** (InterpreterFallback); see below |
| T2 `HaltExecution` from another thread stops a spinning guest | pass; 190-580 µs from the halt request until the thread has returned and been joined |
| P1 load+add loop, JIT vs. MSVC `/O2` native | 3.8-4.2 ns vs. 0.76-0.94 ns per iteration (**4.3-5.1x**), about 1 ns per guest instruction |
| Repeat runs | 6 / 6 pass |

**Answer: yes for the synthetic path; the real `sdk` run is still open.**
- The CPU-side mechanisms the runtime needs all work on x64: the svc gates, thread registers, the NVN bridge in both
  directions, exclusive monitors across threads, preemption by halt, and fastmem with the guest at its own addresses.
- Running the real TOTK `sdk` / `nn::init::Start` needs the player's dumped modules, which are not on this machine.
  That run reuses this harness: load the three NSOs at their layout, resolve imports, and point the bridge at the
  real `hle_nvn.c`.

**Findings that change the plan.**
- **dynarmic upstream is gone.** `merryhime/dynarmic` no longer exists, and `lioncash/dynarmic` was archived on
  2024-03-03 with no tags. The pinned commit builds cleanly with MSVC 14.44. Maintained forks (Vita3K, EKA2L1,
  touchHLE, Panda3DS) mainly exercise the 32-bit ARM frontend. The project has to own its fork, and should first
  evaluate Eden's in-tree A64 fixes.
- **No LSE.** That is fine for retail code: the Switch's Cortex-A57 is ARMv8.0, so the game cannot contain LSE.
  It matters for this project's own code: anything the host injects into guest code must stay ARMv8.0. One example
  is `nvn_thunks.S`, which uses `stadd` (LSE) for call counting.
- **The P1 slowdown (about 4-5x on a load-heavy loop)** is the steady-state cost before any tuning: accurate FP, no
  unsafe optimizations, and a halt check on every block. It sets a baseline for the X2 "30 fps on Haswell-E" target,
  which has to be checked against real game code.
