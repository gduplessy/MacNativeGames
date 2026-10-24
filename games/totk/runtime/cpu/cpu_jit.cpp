// jit CPU backend (cpu_backend.h): the game's ARM64 code on an x86-64 host through dynarmic.
//
// Guest memory is at the same address in host and guest (fastmem base 0, 39 address bits), so HLE services read and
// write it by plain pointer. A guest svc halts the JIT; the svc is served outside Run(), with the registers in the gate.S
// frame layout, so runtime_svc and the bridges see what they see on an ARM64 host. Guest -> host calls go through
// stubs in config.stub_base: entry i is "svc #CPU_SVC_BRIDGE+i; ret", and its (bridge, fn) pair moves the AAPCS64
// arguments into a host call. Host -> guest calls (cpu_call) save the thread's state, run fn with LR at a return stub,
// and restore it, so they nest inside bridges.
//
// Validated by games/totk/tests/cpu_bridge (Windows x64). dynarmic: lioncash/dynarmic a41c380 (archived upstream).
#include "cpu_backend.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr uint32_t SVC_INSN = 0xd4000001u, RET_INSN = 0xd65f03c0u;
constexpr uint64_t STUB_BYTES = 0x10000, RETURN_STUB_OFF = 0x8000;   // entries at +0 (8 bytes each), return stub at +0x8000
static_assert(CPU_MAX_HOST_ENTRIES * 8 <= RETURN_STUB_OFF, "host-entry stubs overlap the return stub");

cpu_config g_cfg;
std::unique_ptr<Dynarmic::ExclusiveMonitor> g_monitor;
struct HostEntry { cpu_bridge_fn bridge; void *fn; const char *name; };
HostEntry g_entries[CPU_MAX_HOST_ENTRIES];
std::atomic<unsigned> g_nentries;
std::mutex g_entries_mu, g_threads_mu;
std::vector<cpu_thread *> g_threads;
thread_local cpu_thread *tls_current;

#ifdef _WIN32
// Copies that survive an access violation. A write-protected page (dirty tracking) is handled by the runtime's
// vectored handler first, which unprotects it and lets the copy finish; only a page that is really not mapped fails.
bool guarded_copy(void *dst, const void *src, size_t n) {
    __try { memcpy(dst, src, n); return true; } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool guarded_cas64(uint64_t a, uint64_t v, uint64_t e) {
    __try { return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64 *>(a), static_cast<LONG64>(v), static_cast<LONG64>(e)) == static_cast<LONG64>(e); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool guarded_cas32(uint64_t a, uint32_t v, uint32_t e) {
    __try { return InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(a), static_cast<LONG>(v), static_cast<LONG>(e)) == static_cast<LONG>(e); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
#else
bool guarded_copy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); return true; }
bool guarded_cas64(uint64_t a, uint64_t v, uint64_t e) { return __atomic_compare_exchange_n(reinterpret_cast<uint64_t *>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
bool guarded_cas32(uint64_t a, uint32_t v, uint32_t e) { return __atomic_compare_exchange_n(reinterpret_cast<uint32_t *>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
#endif

void fail(cpu_thread *t, uint64_t pc, const char *what);

}  // namespace

struct cpu_thread final : Dynarmic::A64::UserCallbacks {
    unsigned core;
    uint64_t tpidr = 0, tpidrro;
    uint32_t pending_svc = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit;

    cpu_thread(unsigned core, uint64_t tlr) : core(core), tpidrro(tlr) {
        Dynarmic::A64::UserConfig c;
        c.callbacks = this;
        c.processor_id = core;
        c.global_monitor = g_monitor.get();
        c.tpidrro_el0 = &tpidrro;
        c.tpidr_el0 = &tpidr;
        c.cntfrq_el0 = static_cast<uint32_t>(g_cfg.counter_hz);
        c.fastmem_pointer = uintptr_t{0};        // guest address == host address
        c.fastmem_address_space_bits = 39;       // the SDK's IPC descriptors carry 39-bit addresses
        c.silently_mirror_fastmem = false;
        c.fastmem_exclusive_access = true;
        c.recompile_on_fastmem_failure = true;   // only unmapped pages reach the callbacks; a protected page is resumed by the VEH
        c.enable_cycle_counting = false;         // preemption is cpu_request_halt, not a tick budget
        c.code_cache_size = 256ull << 20;
        jit = std::make_unique<Dynarmic::A64::Jit>(c);
    }

    // Instruction fetch and the accesses fastmem could not serve.
    std::optional<uint32_t> MemoryReadCode(uint64_t a) override { uint32_t v; if (guarded_copy(&v, reinterpret_cast<void *>(a), 4)) return v; return std::nullopt; }
    template<typename T> T rd(uint64_t a) { T v{}; if (!guarded_copy(&v, reinterpret_cast<void *>(a), sizeof v)) fail(this, jit->GetPC(), "read of unmapped guest memory"); return v; }
    template<typename T> void wr(uint64_t a, T v) { if (!guarded_copy(reinterpret_cast<void *>(a), &v, sizeof v)) fail(this, jit->GetPC(), "write to unmapped guest memory"); }
    uint8_t MemoryRead8(uint64_t a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(uint64_t a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(uint64_t a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(uint64_t a) override { return rd<uint64_t>(a); }
    Dynarmic::A64::Vector MemoryRead128(uint64_t a) override { return {rd<uint64_t>(a), rd<uint64_t>(a + 8)}; }
    void MemoryWrite8(uint64_t a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(uint64_t a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(uint64_t a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(uint64_t a, uint64_t v) override { wr(a, v); }
    void MemoryWrite128(uint64_t a, Dynarmic::A64::Vector v) override { wr(a, v[0]); wr(a + 8, v[1]); }
    bool MemoryWriteExclusive32(uint64_t a, uint32_t v, uint32_t e) override { return guarded_cas32(a, v, e); }
    bool MemoryWriteExclusive64(uint64_t a, uint64_t v, uint64_t e) override { return guarded_cas64(a, v, e); }

    void InterpreterFallback(uint64_t pc, size_t) override { fail(this, pc, "instruction without a translation (dynarmic implements ARMv8.0 user code; no LSE)"); }
    void CallSVC(uint32_t swi) override { pending_svc = swi; jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) override {
        char what[64]; std::snprintf(what, sizeof what, "guest exception %d", static_cast<int>(e)); fail(this, pc, what);
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~0ull >> 1; }
    uint64_t GetCNTPCT() override { return g_cfg.counter(); }

    void load(cpu_regs &r) const {
        auto x = jit->GetRegisters(); for (int i = 0; i < 31; i++) r.x[i] = x[i];
        auto v = jit->GetVectors(); for (int i = 0; i < 32; i++) { memcpy(r.q[i], &v[i][0], 8); memcpy(r.q[i] + 8, &v[i][1], 8); }
        r.nzcv = jit->GetPstate() & 0xf0000000u; r.fpsr = jit->GetFpsr(); r.fpcr = jit->GetFpcr();
    }
    void store(const cpu_regs &r) {
        std::array<uint64_t, 31> x; for (int i = 0; i < 31; i++) x[i] = r.x[i]; jit->SetRegisters(x);
        std::array<Dynarmic::A64::Vector, 32> v; for (int i = 0; i < 32; i++) { memcpy(&v[i][0], r.q[i], 8); memcpy(&v[i][1], r.q[i] + 8, 8); } jit->SetVectors(v);
        jit->SetPstate((jit->GetPstate() & 0x0fffffffu) | (static_cast<uint32_t>(r.nzcv) & 0xf0000000u));
        jit->SetFpsr(static_cast<uint32_t>(r.fpsr)); jit->SetFpcr(static_cast<uint32_t>(r.fpcr));
    }

    // Run until the innermost cpu_call reaches the return stub.
    void run() {
        for (;;) {
            const Dynarmic::HaltReason hr = jit->Run();
            if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
                const uint32_t s = pending_svc; pending_svc = 0;
                if (s == CPU_SVC_RETURN) return;
                cpu_regs r; load(r);
                if (s >= CPU_SVC_BRIDGE && s - CPU_SVC_BRIDGE < g_nentries.load(std::memory_order_acquire)) {
                    const HostEntry &e = g_entries[s - CPU_SVC_BRIDGE];
                    e.bridge(&r, e.fn);
                } else {
                    g_cfg.svc(s, &r);
                }
                store(r);
            }
            if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1) && g_cfg.preempted) g_cfg.preempted(this);
        }
    }
};

namespace {
void fail(cpu_thread *t, uint64_t pc, const char *what) {
    if (g_cfg.fault) g_cfg.fault(t, pc, what);
    std::fprintf(stderr, "[cpu] FAIL %s at pc %#llx\n", what, static_cast<unsigned long long>(pc));
    std::abort();
}
}  // namespace

extern "C" {

int cpu_init(const cpu_config *c) {
    if (!c || !c->svc || !c->counter || !c->counter_hz || !c->cores || c->stub_base < c->guest_base || c->stub_base + STUB_BYTES > c->guest_base + c->guest_size) return -1;
    g_cfg = *c;
    g_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(c->cores);
#ifdef _WIN32
    if (!VirtualAlloc(reinterpret_cast<void *>(c->stub_base), STUB_BYTES, MEM_COMMIT, PAGE_READWRITE)) return -2;
#endif
    uint32_t *ret_stub = reinterpret_cast<uint32_t *>(c->stub_base + RETURN_STUB_OFF);
    ret_stub[0] = SVC_INSN | (CPU_SVC_RETURN << 5); ret_stub[1] = RET_INSN;
    return 0;
}
const char *cpu_backend_name(void) { return "jit (dynarmic a41c380, x86-64)"; }

cpu_thread *cpu_thread_new(unsigned core, uint64_t tpidrro) {
    if (core >= g_cfg.cores) return nullptr;
    auto *t = new cpu_thread(core, tpidrro);
    std::lock_guard<std::mutex> l(g_threads_mu); g_threads.push_back(t);
    return t;
}
void cpu_thread_free(cpu_thread *t) {
    { std::lock_guard<std::mutex> l(g_threads_mu); std::erase(g_threads, t); }
    delete t;
}
cpu_thread *cpu_current(void) { return tls_current; }
uint64_t cpu_tpidr(const cpu_thread *t) { return t->tpidr; }

uint64_t cpu_call(cpu_thread *t, uint64_t fn, const uint64_t *args, unsigned nargs, uint64_t sp) {
    auto &j = *t->jit;
    cpu_thread *outer = tls_current; tls_current = t;
    const auto regs = j.GetRegisters(); const auto vecs = j.GetVectors();
    const uint64_t pc = j.GetPC(), old_sp = j.GetSP(); const uint32_t ps = j.GetPstate(), fpcr = j.GetFpcr(), fpsr = j.GetFpsr();
    for (unsigned i = 0; i < nargs && i < 8; i++) j.SetRegister(i, args[i]);
    j.SetRegister(30, g_cfg.stub_base + RETURN_STUB_OFF);
    j.SetPC(fn);
    if (sp) j.SetSP(sp & ~15ull);
    t->run();
    const uint64_t result = j.GetRegister(0);
    j.SetRegisters(regs); j.SetVectors(vecs); j.SetPC(pc); j.SetSP(old_sp); j.SetPstate(ps); j.SetFpcr(fpcr); j.SetFpsr(fpsr);
    tls_current = outer;
    return result;
}

uint64_t cpu_host_entry(cpu_bridge_fn bridge, void *fn, const char *name) {
    if (!bridge || !fn) return 0;
    std::lock_guard<std::mutex> l(g_entries_mu);
    unsigned n = g_nentries.load(std::memory_order_relaxed), i = 0;
    while (i < n && (g_entries[i].bridge != bridge || g_entries[i].fn != fn)) i++;
    if (i == n) {
        if (n == CPU_MAX_HOST_ENTRIES) fail(nullptr, 0, "host-entry table full");
        g_entries[n] = {bridge, fn, name};
        uint32_t *stub = reinterpret_cast<uint32_t *>(g_cfg.stub_base + uint64_t{n} * 8);
        stub[0] = SVC_INSN | ((CPU_SVC_BRIDGE + n) << 5); stub[1] = RET_INSN;
        g_nentries.store(n + 1, std::memory_order_release);
    }
    return g_cfg.stub_base + uint64_t{i} * 8;
}

void cpu_request_halt(cpu_thread *t) { t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }

void cpu_invalidate_code(uint64_t addr, size_t len) {
    std::lock_guard<std::mutex> l(g_threads_mu);
    for (cpu_thread *t : g_threads) t->jit->InvalidateCacheRange(addr, len);   // deferred to each thread's next Run()
}

}  // extern "C"
