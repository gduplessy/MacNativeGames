// Spike X-S2 (docs/windows-android-port-plan.md, section 9.5): can dynarmic carry the TOTK boot path on Windows x64,
// with the existing HLE design (guest memory at its own addresses, svc gates, NVN through a host bridge)?
//
// No game files are needed: a synthetic guest program (generated with oaknut at its own guest address) walks the same
// shape as nn::init::Start up to the first NVN calls, then the probe checks the CPU features the runtime depends on:
//   B1 TPIDRRO_EL0 (Horizon TLR), TPIDR_EL0 round trip, x18 as a plain register
//   B2 svc GetInfo / SetHeapSize answered by the host; the guest writes its heap, the host reads it by plain pointer
//   B3 CNTPCT_EL0 at 19.2 MHz, monotonic
//   B4 FMADD (double) and FMLA (4 x float) bit-identical to host fma()/fmaf()
//   B5 nvnBootstrapLoader -> guest stubs (svc #0x8000+i; ret) -> generic signature bridge -> host C functions, incl.
//      a float argument in s0 and a host -> guest callback with six arguments (nested run, state saved and restored)
//   B6 a guest load from reserved-but-uncommitted memory takes the fastmem fault path and falls back to the callback
//   T1 4 guest threads (own Jit each, one exclusive monitor) x 200k LDAXR/STLXR, LDXR/STXR (32-bit), LDAXP/STLXP: exact
//   L1 report only: an ARMv8.1 LSE instruction (LDADDAL), which the Switch's ARMv8.0 CPU does not have
//   T2 HaltExecution stops a spinning guest thread from another host thread (preemption), latency measured
//   P1 throughput of a load+add loop vs the same loop compiled natively
// Output: key=value lines, then "RESULT pass" or "RESULT fail <first failure>". Exit code 0 on pass.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <oaknut/oaknut.hpp>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- guest layout: one reservation at its own address below 2^39 (X-S1 showed the space is free) ----
constexpr u64 G_BASE = 0x400000000ULL, G_SIZE = 1ULL << 30;
constexpr u64 CODE = G_BASE, DATA = G_BASE + 0x1000000, TLR = G_BASE + 0x2000000, STACKS = G_BASE + 0x3000000;
constexpr u64 HEAP = G_BASE + 0x10000000, BENCH = G_BASE + 0x20000000, UNMAPPED = G_BASE + 0x38000000;
constexpr u64 STUBS = CODE + 0x800000;            // bridge stubs: svc #0x8000+i ; ret
constexpr u64 RET_TRAP = CODE + 0xF00000;         // host -> guest calls return here: svc #0x7fff
constexpr u32 SVC_BRIDGE = 0x8000, SVC_RET_TRAP = 0x7fff;
constexpr u64 CNT_HZ = 19200000;

static const char *first_failure;
static void check(bool ok, const char *what) { if (!ok && !first_failure) first_failure = what; if (!ok) std::printf("FAIL %s\n", what); }
template<typename T> static T &at(u64 a) { return *reinterpret_cast<T *>(static_cast<uintptr_t>(a)); }
static bool committed(u64 a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<void *>(a), &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    return a + n <= reinterpret_cast<u64>(mi.BaseAddress) + mi.RegionSize;
}
static u64 qpc_freq;
static u64 cntpct() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return static_cast<u64>(static_cast<double>(t.QuadPart) * CNT_HZ / qpc_freq); }

// ---- one guest thread ----
struct GuestThread;
static std::atomic<u64> fault_fallback_reads;
struct Callbacks final : Dynarmic::A64::UserCallbacks {
    GuestThread *t;
    explicit Callbacks(GuestThread *t) : t(t) {}
    std::optional<u32> MemoryReadCode(u64 a) override { return at<u32>(a); }
    // Reached only when fastmem cannot serve the access (B6): answer like an unmapped Horizon page would be caught.
    template<typename T> T rd(u64 a) { if (committed(a, sizeof(T))) { T v; std::memcpy(&v, reinterpret_cast<void *>(a), sizeof v); return v; } fault_fallback_reads++; return static_cast<T>(0xBAD0BAD0BAD0BAD0ULL); }
    template<typename T> void wr(u64 a, T v) { if (committed(a, sizeof(T))) std::memcpy(reinterpret_cast<void *>(a), &v, sizeof v); }
    u8 MemoryRead8(u64 a) override { return rd<u8>(a); }
    std::uint16_t MemoryRead16(u64 a) override { return rd<std::uint16_t>(a); }
    u32 MemoryRead32(u64 a) override { return rd<u32>(a); }
    u64 MemoryRead64(u64 a) override { return rd<u64>(a); }
    Dynarmic::A64::Vector MemoryRead128(u64 a) override { return {rd<u64>(a), rd<u64>(a + 8)}; }
    void MemoryWrite8(u64 a, u8 v) override { wr(a, v); }
    void MemoryWrite16(u64 a, std::uint16_t v) override { wr(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { wr(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { wr(a, v); }
    void MemoryWrite128(u64 a, Dynarmic::A64::Vector v) override { wr(a, v[0]); wr(a + 8, v[1]); }
    template<typename T> bool wx(u64 a, T v, T e) { return committed(a, sizeof(T)) && InterlockedCompareExchange64(reinterpret_cast<volatile LONG64 *>(a), static_cast<LONG64>(v), static_cast<LONG64>(e)) == static_cast<LONG64>(e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return wx(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override { return committed(a, 4) && InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(a), static_cast<LONG>(v), static_cast<LONG>(e)) == static_cast<LONG>(e); }
    void InterpreterFallback(u64 pc, size_t n) override;
    void CallSVC(u32 swi) override;
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override { std::printf("guest exception %d at %#llx\n", static_cast<int>(e), static_cast<unsigned long long>(pc)); check(false, "guest exception"); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ULL << 40; }
    u64 GetCNTPCT() override { return cntpct(); }
};

static Dynarmic::ExclusiveMonitor monitor{8};
struct GuestThread {
    unsigned id;
    Callbacks cb{this};
    u64 tpidr = 0, tpidrro = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    u32 pending_svc = 0; bool exited = false;
    explicit GuestThread(unsigned id) : id(id) {
        tpidrro = TLR + id * 0x200;
        Dynarmic::A64::UserConfig c;
        c.callbacks = &cb;
        c.processor_id = id;
        c.global_monitor = &monitor;
        c.tpidrro_el0 = &tpidrro;
        c.tpidr_el0 = &tpidr;
        c.cntfrq_el0 = static_cast<u32>(CNT_HZ);
        c.fastmem_pointer = uintptr_t{0};             // guest address == host address
        c.fastmem_address_space_bits = 39;            // Horizon's 39-bit space; nothing mirrors above it
        c.silently_mirror_fastmem = false;
        c.fastmem_exclusive_access = true;
        c.enable_cycle_counting = false;              // preemption by HaltExecution, as the runtime would do it
        c.code_cache_size = 256ULL << 20;
        jit = std::make_unique<Dynarmic::A64::Jit>(c);
        jit->SetSP(STACKS + (id + 1) * 0x100000ULL - 16);
    }
};
// dynarmic has no translation for the instruction at pc: record its encoding and stop this guest thread.
static std::atomic<u32> unsupported_insn; static std::atomic<u64> unsupported_pc;
void Callbacks::InterpreterFallback(u64 pc, size_t) {
    unsupported_insn = at<u32>(pc); unsupported_pc = pc; t->exited = true; t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
}
void Callbacks::CallSVC(u32 swi) { t->pending_svc = swi; t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }

// ---- host NVN functions (stand-ins with the real signatures) and the bridge ----
struct NVNdeviceBuilder { u64 flags; u64 reserved[7]; };
struct NVNsamplerBuilder { float lod_bias; u32 pad[15]; };
struct NVNdevice { u64 magic; u64 flags; u64 debug_cb; u64 debug_user; };
static void run_until_exit(GuestThread &t);
static u64 guest_call(GuestThread &t, u64 fn, std::initializer_list<u64> args);
static GuestThread *bridge_thread;   // the thread whose svc is being served (one boot thread in this probe)

static void nvnDeviceBuilderSetDefaults(NVNdeviceBuilder *b) { std::memset(b, 0, sizeof *b); }
static void nvnDeviceBuilderSetFlags(NVNdeviceBuilder *b, int flags) { b->flags = static_cast<u32>(flags); }
static void nvnSamplerBuilderSetLodBias(NVNsamplerBuilder *b, float bias) { b->lod_bias = bias; }
static void nvnDeviceInstallDebugCallback(NVNdevice *d, u64 cb, u64 user, int enable) { d->debug_cb = enable ? cb : 0; d->debug_user = user; }
static u8 nvnDeviceInitialize(NVNdevice *d, const NVNdeviceBuilder *b) {
    d->magic = 0x4E564E44; d->flags = b->flags;
    // the driver reports through the game's callback: host -> guest, six arguments (source, type, id, severity, message, user)
    if (d->debug_cb) guest_call(*bridge_thread, d->debug_cb, {1, 2, 0x1234, 3, DATA + 0xE00, d->debug_user});
    return 1;
}

// Signature string: return type, ':', argument types. p = pointer/u64 (x), i = int (w), f = float (s), v = void, b = bool.
struct BridgeEntry { const char *name; void *fn; const char *sig; };
static const BridgeEntry bridge[] = {
    {"nvnDeviceBuilderSetDefaults", reinterpret_cast<void *>(&nvnDeviceBuilderSetDefaults), "v:p"},
    {"nvnDeviceBuilderSetFlags", reinterpret_cast<void *>(&nvnDeviceBuilderSetFlags), "v:pi"},
    {"nvnSamplerBuilderSetLodBias", reinterpret_cast<void *>(&nvnSamplerBuilderSetLodBias), "v:pf"},
    {"nvnDeviceInstallDebugCallback", reinterpret_cast<void *>(&nvnDeviceInstallDebugCallback), "v:pppi"},
    {"nvnDeviceInitialize", reinterpret_cast<void *>(&nvnDeviceInitialize), "b:pp"},
};
constexpr u32 NBRIDGE = sizeof bridge / sizeof bridge[0];
constexpr u32 BOOTSTRAP = NBRIDGE;   // stub index of nvnBootstrapLoader itself
static unsigned bridge_calls;

static void bridge_call(GuestThread &t, u32 index) {
    auto &j = *t.jit;
    if (index == BOOTSTRAP) {   // nvnBootstrapLoader(const char *name): guest stub address or 0
        const char *name = reinterpret_cast<const char *>(j.GetRegister(0));
        u64 r = 0;
        for (u32 i = 0; i < NBRIDGE; i++) if (!std::strcmp(name, bridge[i].name)) r = STUBS + i * 8;
        j.SetRegister(0, r);
        return;
    }
    const BridgeEntry &e = bridge[index]; bridge_calls++;
    u64 x[8]; float f[8]; unsigned nx = 0, nf = 0;
    for (const char *s = e.sig + 2; *s; s++) {
        if (*s == 'f') { u32 bits = static_cast<u32>(j.GetVector(nf)[0]); std::memcpy(&f[nf++], &bits, 4); }
        else x[nx] = j.GetRegister(nx), nx++;
    }
    const std::string sig = e.sig; u64 r = 0;
    // a generator (gen_protos.py --abi x64-bridge) would emit one typed stub per function instead of this switch
    if (sig == "v:p") reinterpret_cast<void (*)(u64)>(e.fn)(x[0]);
    else if (sig == "v:pi") reinterpret_cast<void (*)(u64, int)>(e.fn)(x[0], static_cast<int>(x[1]));
    else if (sig == "v:pf") reinterpret_cast<void (*)(u64, float)>(e.fn)(x[0], f[0]);
    else if (sig == "v:pppi") reinterpret_cast<void (*)(u64, u64, u64, int)>(e.fn)(x[0], x[1], x[2], static_cast<int>(x[3]));
    else if (sig == "b:pp") r = reinterpret_cast<u8 (*)(u64, u64)>(e.fn)(x[0], x[1]);
    else check(false, "bridge signature");
    if (e.sig[0] != 'v') j.SetRegister(0, r);
}

// ---- svc service loop ----
static void serve_svc(GuestThread &t) {
    auto &j = *t.jit; u32 s = t.pending_svc; t.pending_svc = 0;
    switch (s) {
    case 0x01: {   // SetHeapSize(size) -> (result, address)
        u64 size = j.GetRegister(1);
        bool ok = VirtualAlloc(reinterpret_cast<void *>(HEAP), size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
        j.SetRegister(0, ok ? 0 : 0xd401); j.SetRegister(1, HEAP); break;
    }
    case 0x07: t.exited = true; break;   // ExitProcess
    case 0x29: j.SetRegister(0, 0); j.SetRegister(1, j.GetRegister(1) == 6 ? 0xcd500000ULL : 0); break;   // GetInfo: 6 = TotalMemorySize
    default:
        if (s >= SVC_BRIDGE && s < SVC_BRIDGE + NBRIDGE + 1) bridge_call(t, s - SVC_BRIDGE);
        else { std::printf("unhandled svc %#x\n", s); check(false, "unhandled svc"); t.exited = true; }
    }
}
static bool returned;   // set by the return trap of a host -> guest call
static void run_until_exit(GuestThread &t) {
    while (!t.exited) {
        Dynarmic::HaltReason hr = t.jit->Run();
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            if (t.pending_svc == SVC_RET_TRAP) { t.pending_svc = 0; returned = true; return; }
            serve_svc(t);
        }
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1)) return;   // preempted (T2)
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined3)) return;   // unsupported instruction
    }
}
// Host -> guest: save the interrupted guest state, run fn with args and LR at the return trap, restore.
static u64 guest_call(GuestThread &t, u64 fn, std::initializer_list<u64> args) {
    auto &j = *t.jit;
    auto regs = j.GetRegisters(); auto vecs = j.GetVectors(); u64 pc = j.GetPC(), sp = j.GetSP(); u32 ps = j.GetPstate(), fpcr = j.GetFpcr(), fpsr = j.GetFpsr();
    unsigned i = 0; for (u64 a : args) j.SetRegister(i++, a);
    j.SetRegister(30, RET_TRAP); j.SetPC(fn); j.SetSP(sp - 0x100);
    bool outer = returned; returned = false;
    run_until_exit(t);
    check(returned, "host -> guest call returned through the trap");
    u64 r = j.GetRegister(0);
    returned = outer;
    j.SetRegisters(regs); j.SetVectors(vecs); j.SetPC(pc); j.SetSP(sp); j.SetPstate(ps); j.SetFpcr(fpcr); j.SetFpsr(fpsr);
    return r;
}

// ---- guest programs ----
using namespace oaknut::util;
static u64 here(oaknut::CodeGenerator &g) { return g.xptr<std::uintptr_t>(); }

static u64 emit_boot(oaknut::CodeGenerator &g, u64 &callback) {
    // strings and the import slot the "loader" fills, like the module's GOT entry for nvnBootstrapLoader
    const u64 NAMES = DATA + 0x400, SLOTS = DATA + 0x300, BUILDER = DATA + 0x1000, SAMPLER = DATA + 0x1100, DEVICE = DATA + 0x2000;
    for (u32 i = 0; i < NBRIDGE; i++) strcpy_s(reinterpret_cast<char *>(NAMES + i * 64), 64, bridge[i].name);
    strcpy_s(reinterpret_cast<char *>(DATA + 0xE00), 64, "debug message");
    at<u64>(DATA + 0x200) = STUBS + BOOTSTRAP * 8;
    at<double>(DATA + 0x40) = 1.0000001; at<double>(DATA + 0x48) = 3.333333333; at<double>(DATA + 0x50) = -3.33333;
    float va[4] = {1.5f, -2.25f, 1e-30f, 3.0e30f}, vb[4] = {1.0000001f, 4.0f, 1e-10f, 7.0f}, vc[4] = {0.1f, 9.0f, -1e-40f, -2.0e31f};
    std::memcpy(reinterpret_cast<void *>(DATA + 0x60), va, 16); std::memcpy(reinterpret_cast<void *>(DATA + 0x70), vb, 16); std::memcpy(reinterpret_cast<void *>(DATA + 0x80), vc, 16);

    // the debug callback: store id (x2) and user data (x5), clobber caller-saved registers, return
    callback = here(g);
    g.MOV(X9, DATA); g.STR(X2, X9, 0xA0); g.STR(X5, X9, 0xA8); g.MOV(X10, 0xdead); g.MOV(X0, 0); g.RET();

    u64 start = here(g);
    g.STP(X29, X30, SP, PRE_INDEXED, -16);
    g.MOV(X20, DATA);
    // B1: Horizon TLR via TPIDRRO_EL0; TPIDR_EL0 round trip; x18 is just a register here
    g.MRS(X19, oaknut::SystemReg::TPIDRRO_EL0);
    g.MOV(X0, 0x5a5a); g.STR(X0, X19, 0x1f8);
    g.MOV(X1, DATA + 0x100); g.MSR(oaknut::SystemReg::TPIDR_EL0, X1); g.MRS(X2, oaknut::SystemReg::TPIDR_EL0); g.STR(X2, X1, 0);
    g.MOV(X18, 0x1818); g.ADD(X18, X18, 1); g.STR(X18, X20, 0x8);
    // B2: GetInfo(TotalMemorySize), SetHeapSize(32 MiB), write the heap
    g.MOV(X1, 6); g.MOV(X2, 0); g.MOV(X3, 0); g.SVC(0x29); g.STR(X1, X20, 0x10);
    g.MOV(X1, 0x2000000); g.SVC(0x01); g.STR(X1, X20, 0x18); g.MOV(X3, 0xC0FFEE); g.STR(X3, X1, 0);
    // B3: counters
    g.MRS(X4, oaknut::SystemReg::CNTPCT_EL0); g.MRS(X5, oaknut::SystemReg::CNTPCT_EL0); g.STP(X4, X5, X20, 0x20);
    // B4: FMADD and FMLA
    g.LDR(D0, X20, 0x40); g.LDR(D1, X20, 0x48); g.LDR(D2, X20, 0x50); g.FMADD(D3, D0, D1, D2); g.STR(D3, X20, 0x58);
    g.LDR(Q0, X20, 0x60); g.LDR(Q1, X20, 0x70); g.LDR(Q2, X20, 0x80); g.FMLA(V2.S4(), V0.S4(), V1.S4()); g.STR(Q2, X20, 0x90);
    // B5: resolve every function through nvnBootstrapLoader, then make the boot calls
    g.LDR(X21, X20, 0x200);
    for (u32 i = 0; i < NBRIDGE; i++) { g.MOV(X0, NAMES + i * 64); g.BLR(X21); g.MOV(X9, SLOTS + i * 8); g.STR(X0, X9, 0); }
    auto call = [&](u32 i) { g.MOV(X9, SLOTS + i * 8); g.LDR(X8, X9, 0); g.BLR(X8); };
    g.MOV(X0, BUILDER); call(0);
    g.MOV(X0, BUILDER); g.MOV(W1, 3); call(1);
    g.MOV(X0, SAMPLER); g.MOV(W2, 0x3f400000); g.FMOV(S0, W2); call(2);   // 0.75f in s0
    g.MOV(X0, DEVICE); g.MOV(X1, callback);
    g.MOV(X2, 0x77); g.MOV(W3, 1); call(3);
    g.MOV(X0, DEVICE); g.MOV(X1, BUILDER); call(4); g.STR(X0, X20, 0x30);
    // B6: a load from reserved, uncommitted guest memory
    g.MOV(X6, UNMAPPED); g.LDR(X7, X6, 0); g.STR(X7, X20, 0x38);
    g.LDP(X29, X30, SP, POST_INDEXED, 16);
    g.MOV(X0, 0); g.SVC(0x07);
    return start;
}
static u64 emit_atomics(oaknut::CodeGenerator &g) {   // x0 = counters, x1 = iterations
    u64 start = here(g);
    oaknut::Label l1, l2, l3;
    g.MOV(X2, X1);
    g.l(l1); g.LDAXR(X3, X0); g.ADD(X3, X3, 1); g.STLXR(W4, X3, X0); g.CBNZ(W4, l1); g.SUBS(X2, X2, 1); g.B(oaknut::Cond::NE, l1);
    g.MOV(X2, X1); g.ADD(X7, X0, 8);   // 32-bit exclusives without acquire/release, then a DMB, like compiled std::atomic code
    g.l(l2); g.LDXR(W3, X7); g.ADD(W3, W3, 1); g.STXR(W4, W3, X7); g.CBNZ(W4, l2); g.DMB(oaknut::BarrierOp::ISH); g.SUBS(X2, X2, 1); g.B(oaknut::Cond::NE, l2);
    g.MOV(X2, X1); g.ADD(X8, X0, 16);  // exclusive pair on a 128-bit slot, both halves incremented
    g.l(l3); g.LDAXP(X9, X10, X8); g.ADD(X9, X9, 1); g.ADD(X10, X10, 1); g.STLXP(W4, X9, X10, X8); g.CBNZ(W4, l3);
    g.SUBS(X2, X2, 1); g.B(oaknut::Cond::NE, l3);
    g.SVC(0x07);
    return start;
}
static u64 emit_lse(oaknut::CodeGenerator &g) { u64 s = here(g); g.MOV(X5, 1); g.LDADDAL(X5, X6, X0); g.SVC(0x07); return s; }
static u64 emit_spin(oaknut::CodeGenerator &g) { u64 s = here(g); oaknut::Label l; g.MOV(X0, 0); g.l(l); g.ADD(X0, X0, 1); g.B(l); return s; }
static u64 emit_sum(oaknut::CodeGenerator &g) {   // x0 = buffer, x1 = count of u64 -> x0 = sum
    u64 s = here(g); oaknut::Label l;
    g.MOV(X2, 0); g.l(l); g.LDR(X3, X0, POST_INDEXED, 8); g.ADD(X2, X2, X3); g.SUBS(X1, X1, 1); g.B(oaknut::Cond::NE, l);
    g.MOV(X0, X2); g.SVC(0x07);
    return s;
}

__declspec(noinline) static u64 native_sum(const volatile u64 *p, u64 n) { u64 s = 0; while (n--) s += *p++; return s; }

int main() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpc_freq = static_cast<u64>(f.QuadPart);
    bool reserved = VirtualAlloc(reinterpret_cast<void *>(G_BASE), G_SIZE, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
    check(reserved, "reserve guest range at its own address"); if (!reserved) { std::puts("RESULT fail reserve"); return 1; }
    for (u64 a : {CODE, DATA, TLR, STACKS}) VirtualAlloc(reinterpret_cast<void *>(a), 0x1000000, MEM_COMMIT, PAGE_READWRITE);

    // bridge stubs and the return trap, as the loader would place them in guest memory
    { oaknut::CodeGenerator g(reinterpret_cast<u32 *>(STUBS)); for (u32 i = 0; i <= NBRIDGE; i++) { g.SVC(SVC_BRIDGE + i); g.RET(); } }
    { oaknut::CodeGenerator g(reinterpret_cast<u32 *>(RET_TRAP)); g.SVC(SVC_RET_TRAP); }
    oaknut::CodeGenerator g(reinterpret_cast<u32 *>(CODE));
    u64 callback = 0;
    const u64 boot = emit_boot(g, callback), atomics = emit_atomics(g), lse = emit_lse(g), spin = emit_spin(g), sum = emit_sum(g);
    std::printf("guest_code_bytes=%llu\n", static_cast<unsigned long long>(here(g) - CODE));

    // ---- boot thread (B1-B6) ----
    {
        GuestThread t(0); bridge_thread = &t;
        t.jit->SetPC(boot);
        auto t0 = std::chrono::steady_clock::now();
        run_until_exit(t);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("boot_run_ms=%.2f\n", ms);
        check(at<u64>(TLR + 0x1f8) == 0x5a5a, "B1 TPIDRRO_EL0 -> TLR");
        check(at<u64>(DATA + 0x100) == DATA + 0x100 && t.tpidr == DATA + 0x100, "B1 TPIDR_EL0 round trip");
        check(at<u64>(DATA + 0x8) == 0x1819, "B1 x18 as a plain register");
        check(at<u64>(DATA + 0x10) == 0xcd500000ULL, "B2 svc GetInfo");
        check(at<u64>(DATA + 0x18) == HEAP && at<u64>(HEAP) == 0xC0FFEE, "B2 SetHeapSize + host reads guest heap by plain pointer");
        u64 c0 = at<u64>(DATA + 0x20), c1 = at<u64>(DATA + 0x28);
        check(c1 >= c0 && c0 > 0, "B3 CNTPCT_EL0 monotonic");
        double d = std::fma(at<double>(DATA + 0x40), at<double>(DATA + 0x48), at<double>(DATA + 0x50));
        check(std::memcmp(&d, reinterpret_cast<void *>(DATA + 0x58), 8) == 0, "B4 FMADD bit-identical to fma()");
        bool fmla = true;
        for (int i = 0; i < 4; i++) {
            float r = std::fmaf(at<float>(DATA + 0x60 + i * 4), at<float>(DATA + 0x70 + i * 4), at<float>(DATA + 0x80 + i * 4));
            fmla &= std::memcmp(&r, reinterpret_cast<void *>(DATA + 0x90 + i * 4), 4) == 0;
        }
        check(fmla, "B4 FMLA 4S bit-identical to fmaf() (incl. denormal input, default FPCR)");
        auto &b = at<NVNdeviceBuilder>(DATA + 0x1000); auto &s = at<NVNsamplerBuilder>(DATA + 0x1100); auto &dv = at<NVNdevice>(DATA + 0x2000);
        check(b.flags == 3, "B5 nvnDeviceBuilderSetDefaults/SetFlags through the bridge");
        check(s.lod_bias == 0.75f, "B5 float argument in s0");
        check(at<u64>(DATA + 0x30) == 1 && dv.magic == 0x4E564E44 && dv.flags == 3, "B5 nvnDeviceInitialize result");
        check(at<u64>(DATA + 0xA0) == 0x1234 && at<u64>(DATA + 0xA8) == 0x77, "B5 host -> guest debug callback with six arguments");
        check(bridge_calls == NBRIDGE, "B5 every bridge entry called once");
        check(at<u64>(DATA + 0x38) == 0xBAD0BAD0BAD0BAD0ULL && fault_fallback_reads > 0, "B6 uncommitted guest page -> fastmem fault -> callback");
        std::printf("b6_fault_fallback_reads=%llu\nbridge_calls=%u\n", static_cast<unsigned long long>(fault_fallback_reads.load()), bridge_calls);
    }

    // ---- T1: four guest threads contending on the same counters ----
    {
        const u64 COUNTERS = DATA + 0x3000, N = 200000; const unsigned T = 4;
        std::vector<std::thread> hs;
        auto t0 = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < T; i++) hs.emplace_back([&, i] {
            GuestThread t(1 + i); t.jit->SetPC(atomics); t.jit->SetRegister(0, COUNTERS); t.jit->SetRegister(1, N); run_until_exit(t);
        });
        for (auto &h : hs) h.join();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("t1_ldaxr_stlxr_64=%llu\nt1_ldxr_stxr_32=%u\nt1_ldaxp_stlxp=%llu,%llu\nt1_expected=%llu\nt1_ms=%.1f\n",
                    static_cast<unsigned long long>(at<u64>(COUNTERS)), at<u32>(COUNTERS + 8), static_cast<unsigned long long>(at<u64>(COUNTERS + 16)),
                    static_cast<unsigned long long>(at<u64>(COUNTERS + 24)), static_cast<unsigned long long>(T * N), ms);
        check(unsupported_insn == 0, "T1 every exclusive instruction translated");
        check(at<u64>(COUNTERS) == T * N, "T1 LDAXR/STLXR (64-bit) exact under contention");
        check(at<u32>(COUNTERS + 8) == T * N, "T1 LDXR/STXR (32-bit) exact under contention");
        check(at<u64>(COUNTERS + 16) == T * N && at<u64>(COUNTERS + 24) == T * N, "T1 LDAXP/STLXP exact under contention");
    }

    // ---- L1: ARMv8.1 LSE is outside the Switch's ARMv8.0 CPU; report what dynarmic does with it ----
    {
        GuestThread t(7); t.jit->SetPC(lse); t.jit->SetRegister(0, DATA + 0x3100); run_until_exit(t);
        std::printf("l1_lse_ldaddal=%s", unsupported_insn ? "unsupported" : "translated");
        if (unsupported_insn) std::printf(" (insn %#010x at %#llx)", unsupported_insn.load(), static_cast<unsigned long long>(unsupported_pc.load()));
        std::printf("\n");
        unsupported_insn = 0;
    }

    // ---- T2: preempt a spinning guest thread ----
    {
        GuestThread t(5); t.jit->SetPC(spin);
        std::atomic<bool> stopped{false}; std::chrono::steady_clock::time_point halt_at;
        std::thread h([&] { run_until_exit(t); stopped = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        halt_at = std::chrono::steady_clock::now(); t.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        h.join();
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - halt_at).count();
        u64 iters = t.jit->GetRegister(0);
        std::printf("t2_halt_to_return_us=%.1f\nt2_spin_iterations_per_ms=%.0f\n", us, iters / 200.0);
        check(stopped && iters > 0, "T2 HaltExecution stops a spinning guest thread");
    }

    // ---- P1: load+add loop, JIT vs native ----
    {
        const u64 N = 8u << 20;   // 64 MiB of u64
        VirtualAlloc(reinterpret_cast<void *>(BENCH), N * 8, MEM_COMMIT, PAGE_READWRITE);
        for (u64 i = 0; i < N; i++) at<u64>(BENCH + i * 8) = i * 2654435761ULL;
        u64 expect = native_sum(reinterpret_cast<const volatile u64 *>(BENCH), N);
        double best_jit = 1e9, best_native = 1e9;
        for (int r = 0; r < 5; r++) {
            GuestThread t(6); t.jit->SetPC(sum); t.jit->SetRegister(0, BENCH); t.jit->SetRegister(1, N);
            auto t0 = std::chrono::steady_clock::now(); run_until_exit(t);
            double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
            if (ns < best_jit) best_jit = ns;
            check(t.jit->GetRegister(0) == expect, "P1 JIT sum matches native");
            t0 = std::chrono::steady_clock::now(); volatile u64 v = native_sum(reinterpret_cast<const volatile u64 *>(BENCH), N); (void)v;
            ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
            if (ns < best_native) best_native = ns;
        }
        std::printf("p1_jit_ns_per_iter=%.3f\np1_native_ns_per_iter=%.3f\np1_jit_over_native=%.2fx\n", best_jit, best_native, best_jit / best_native);
    }

    if (first_failure) std::printf("RESULT fail %s\n", first_failure); else std::puts("RESULT pass");
    return first_failure != nullptr;
}
