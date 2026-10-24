/* CPU backends: how the game's ARM64 code runs on the host (docs/windows-android-port-plan.md, section 9.1).

   native  ARM64 hosts. The rewritten modules run directly; svc and counter reads enter the host through gate.S, host
           functions run on the thread's host stack through hle_stack.S. Guest and host share one ABI (AAPCS64), so a
           host function is handed to the guest as is.
   jit     x86-64 hosts (cpu_jit.cpp, dynarmic). The guest's registers live in the JIT. A guest call into the host
           lands on a small guest stub (svc #CPU_SVC_BRIDGE + i; ret) and a generated bridge moves the AAPCS64
           arguments out of cpu_regs into a host call (tools/gen_cpu_bridge.py).

   Both backends hand svc handlers the same 800-byte register frame that gate.S builds today, so runtime_svc and every
   HLE service run unchanged on either backend. Guest memory lies at the same address in host and guest on both. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The gate frame (gate.S): what runtime_svc receives as uint64_t *regs. */
typedef struct cpu_regs {
    uint64_t x[31];      /* +0    x0..x30 */
    uint64_t pad0;       /* +248 */
    uint8_t q[32][16];   /* +256  q0..q31, little endian */
    uint64_t nzcv;       /* +768  NZCV in bits 31..28 */
    uint64_t fpsr;       /* +776 */
    uint64_t fpcr;       /* +784 */
    uint64_t pad1;       /* +792 */
} cpu_regs;
#ifdef __cplusplus
static_assert(sizeof(cpu_regs) == 800 && offsetof(cpu_regs, q) == 256 && offsetof(cpu_regs, nzcv) == 768 && offsetof(cpu_regs, fpcr) == 784, "gate.S frame layout");
#else
_Static_assert(sizeof(cpu_regs) == 800 && offsetof(cpu_regs, q) == 256 && offsetof(cpu_regs, nzcv) == 768 && offsetof(cpu_regs, fpcr) == 784, "gate.S frame layout");
#endif

/* AAPCS64 argument access for bridges. A 32-bit argument arrives in wN: the upper half of xN is unspecified. */
static inline int32_t cpu_arg_i32(const cpu_regs *r, unsigned n) { return (int32_t)(uint32_t)r->x[n]; }
static inline uint64_t cpu_arg_i64(const cpu_regs *r, unsigned n) { return r->x[n]; }
static inline float cpu_arg_f32(const cpu_regs *r, unsigned n) { float f; memcpy(&f, r->q[n], 4); return f; }
static inline double cpu_arg_f64(const cpu_regs *r, unsigned n) { double d; memcpy(&d, r->q[n], 8); return d; }
static inline void cpu_ret_i64(cpu_regs *r, uint64_t v) { r->x[0] = v; }
static inline void cpu_ret_f32(cpu_regs *r, float f) { memset(r->q[0], 0, 16); memcpy(r->q[0], &f, 4); }
static inline void cpu_ret_f64(cpu_regs *r, double d) { memset(r->q[0], 0, 16); memcpy(r->q[0], &d, 8); }

/* One guest-visible host function: read the guest's arguments from r, call fn with the host ABI, write the result. */
typedef void (*cpu_bridge_fn)(cpu_regs *r, void *fn);

/* svc immediates the jit backend reserves above Horizon's (< 0x80) and the host cuts (0x80..0xff). */
#define CPU_SVC_BRIDGE 0x8000u     /* + entry index, up to CPU_MAX_HOST_ENTRIES */
#define CPU_SVC_RETURN 0x7fffu     /* end of a host -> guest call */
#define CPU_MAX_HOST_ENTRIES 4096u /* = hle_stack.c HLE_THUNKS */

typedef struct cpu_thread cpu_thread;

typedef struct cpu_config {
    uint64_t guest_base, guest_size;           /* guest memory, already reserved at the same host address, below 2^39 */
    uint64_t stub_base;                        /* 64 KiB inside it for host-entry stubs (jit); committed by the backend */
    uint64_t counter_hz;                       /* CNTFRQ_EL0 / CNTPCT_EL0 rate: 19.2 MHz on Horizon */
    uint64_t (*counter)(void);                 /* CNTPCT_EL0 */
    uint64_t (*svc)(unsigned num, cpu_regs *r);   /* runtime_svc */
    void (*preempted)(cpu_thread *t);          /* after cpu_request_halt: give the core up, return when it is back */
    void (*fault)(cpu_thread *t, uint64_t pc, const char *what);   /* undefined instruction, unmapped access: no return expected */
    unsigned cores;                            /* guest cores sharing one exclusive monitor */
} cpu_config;

/* Once per process, before any guest code runs. 0 on success. */
int cpu_init(const cpu_config *c);
const char *cpu_backend_name(void);

/* One per guest thread, used from one host thread at a time. tpidrro: the thread's Horizon TLR address. */
cpu_thread *cpu_thread_new(unsigned core, uint64_t tpidrro);
void cpu_thread_free(cpu_thread *t);
cpu_thread *cpu_current(void);                 /* the guest thread the calling host thread is running, or NULL */
uint64_t cpu_tpidr(const cpu_thread *t);       /* TPIDR_EL0 as the guest last wrote it */

/* Host -> guest: call fn(args) on t with stack sp (0: keep the thread's current sp), return x0. Nests: a bridge may call
   back into guest code. Up to 8 integer arguments, as every NVN and HLE callback takes. */
uint64_t cpu_call(cpu_thread *t, uint64_t fn, const uint64_t *args, unsigned nargs, uint64_t sp);

/* Guest -> host: the guest address that calls fn through bridge. The same (bridge, fn) gets the same address, so pointers
   the game compares stay consistent (as runtime_hle_wrap_named). name is kept for diagnostics. */
uint64_t cpu_host_entry(cpu_bridge_fn bridge, void *fn, const char *name);

/* Host code calling a guest function, written once for both backends:
       GUEST_CALL(ret, fn_type, fn, args...)   GUEST_CALL0(ret, fn_type, fn)
   On an ARM64 host (the native backend) this is exactly ((fn_type)(fn))(args...), the call the runtime made before.
   With CPU_BACKEND_JIT it runs fn on the calling guest thread through cpu_call and converts x0 to ret. Integer and
   pointer arguments only (up to 8): a float argument or result would need the v registers. */
#define CPU_U64_(x) ((uint64_t)(uintptr_t)(x))
#define CPU_NARGS_(...) CPU_NARGS_N_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define CPU_NARGS_N_(a1, a2, a3, a4, a5, a6, a7, a8, n, ...) n
#define CPU_CAT_(a, b) a##b
#define CPU_CAT(a, b) CPU_CAT_(a, b)
#define CPU_ARGS1_(a) CPU_U64_(a)
#define CPU_ARGS2_(a, ...) CPU_U64_(a), CPU_ARGS1_(__VA_ARGS__)
#define CPU_ARGS3_(a, ...) CPU_U64_(a), CPU_ARGS2_(__VA_ARGS__)
#define CPU_ARGS4_(a, ...) CPU_U64_(a), CPU_ARGS3_(__VA_ARGS__)
#define CPU_ARGS5_(a, ...) CPU_U64_(a), CPU_ARGS4_(__VA_ARGS__)
#define CPU_ARGS6_(a, ...) CPU_U64_(a), CPU_ARGS5_(__VA_ARGS__)
#define CPU_ARGS7_(a, ...) CPU_U64_(a), CPU_ARGS6_(__VA_ARGS__)
#define CPU_ARGS8_(a, ...) CPU_U64_(a), CPU_ARGS7_(__VA_ARGS__)
#ifdef CPU_BACKEND_JIT
uint64_t cpu_guest_call(uint64_t fn, const uint64_t *args, unsigned nargs);   /* cpu_call on cpu_current() */
#define GUEST_CALL(ret, fn_type, fn, ...) \
    ((ret)cpu_guest_call(CPU_U64_(fn), (const uint64_t[]){CPU_CAT(CPU_ARGS, CPU_CAT(CPU_NARGS_(__VA_ARGS__), _))(__VA_ARGS__)}, CPU_NARGS_(__VA_ARGS__)))
#define GUEST_CALL0(ret, fn_type, fn) ((ret)cpu_guest_call(CPU_U64_(fn), 0, 0))
#else
#define GUEST_CALL(ret, fn_type, fn, ...) (((fn_type)(fn))(__VA_ARGS__))
#define GUEST_CALL0(ret, fn_type, fn) (((fn_type)(fn))())
#endif

/* Preemption from any host thread: t stops at the next block boundary and calls config.preempted, then continues. */
void cpu_request_halt(cpu_thread *t);
/* Guest code in [addr, addr+len) changed (load-time game patches): drop its translations. */
void cpu_invalidate_code(uint64_t addr, size_t len);

#ifdef __cplusplus
}
#endif
