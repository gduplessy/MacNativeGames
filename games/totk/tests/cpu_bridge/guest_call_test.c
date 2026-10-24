/* GUEST_CALL in the forms the runtime uses (services.c observers, movie_host.c), compiled twice by build.cmd:
     native (no CPU_BACKEND_JIT): the macro is a plain cast-and-call; the "guest" functions are host C functions here
     jit    (CPU_BACKEND_JIT):    the macro goes through cpu_call; the functions are guest code (bridge_test.cpp)
   gc_run() returns the number of failed checks. In jit mode it must run inside a bridge (a guest thread is current).
   The jit-only cases return garbage above the result's width, which a correct conversion ignores. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cpu_backend.h"

/* Guest functions, by role (bridge_test.cpp generates the jit versions with the same behaviour):
   [0] sum8(a..h) = a + 2b + 3c + ... + 8h (64-bit)   [1] sxtw(x) = (int64_t)(int32_t)x
   [2] store(p, v): *(uint64_t *)p = v                [3] ptr16(p) = p + 16
   [4] answer() = 42                                  [5] bool_ret(x): w0 = x (jit: upper bits of w0 as given)
   [6] int_garbage(): x0 = 0xdead0000fffffffe (int -2 in w0)                                                 */
typedef struct { uint64_t fn[7]; int jit; } gc_fns;

static int failures;
static void expect(int ok, const char *what) { if (!ok) { failures++; printf("FAIL GUEST_CALL %s\n", what); } }

int gc_run(const gc_fns *g) {
    failures = 0;
    /* services.c observed_os_init / observed_sdk_init forms: void results, 2 and 0 arguments */
    uint64_t slot = 0;
    GUEST_CALL(void, void (*)(uint64_t *, uint64_t), g->fn[2], &slot, 0x1122334455667788ull);
    expect(slot == 0x1122334455667788ull, "void result, pointer + u64 arguments");
    expect(GUEST_CALL0(int, int (*)(void), g->fn[4]) == 42, "no arguments");
    /* observed_region form: the function address held in a data pointer */
    uint8_t *as_data = (uint8_t *)(uintptr_t)g->fn[3]; char buf[32];
    expect(GUEST_CALL(char *, char *(*)(char *), as_data, buf) == buf + 16, "pointer result, function address in a uint8_t *");
    /* movie_host.c forms: eight mixed arguments (pointer, negative int, int64, unsigned, NULL) */
    int neg = -5; int64_t t = -1234567890123ll; unsigned flags = 0x80000001u;
    const uint64_t expect_sum = 1 * (uint64_t)(uintptr_t)buf + 2 * (uint64_t)(int64_t)neg + 3 * (uint64_t)t + 4 * (uint64_t)flags
                              + 5 * 0 + 6 * 7 + 7 * (uint64_t)(int64_t)-1 + 8 * 0xffffffffffffffffull;
    uint64_t sum = GUEST_CALL(uint64_t, uint64_t (*)(void *, int, int64_t, unsigned, void *, int, long long, uint64_t), g->fn[0],
                              buf, neg, t, flags, NULL, 7, -1ll, 0xffffffffffffffffull);
    expect(sum == expect_sum, "eight mixed arguments (ints sign-extended, unsigned zero-extended)");
    expect(GUEST_CALL(int64_t, int64_t (*)(int), g->fn[1], neg) == -5, "negative int argument read as wN");
    /* movie_host.c bool results are read through uint8_t */
    expect(GUEST_CALL(uint8_t, bool (*)(int), g->fn[5], 1) != 0, "bool true");
    expect(GUEST_CALL(uint8_t, bool (*)(int), g->fn[5], 0) == 0, "bool false");
    if (g->jit) {
        expect(GUEST_CALL(uint8_t, bool (*)(int), g->fn[5], 0x100) == 0, "bool false with garbage above bit 7");
        expect(GUEST_CALL(uint8_t, bool (*)(int), g->fn[5], 0x101) != 0, "bool true with garbage above bit 7");
        expect(GUEST_CALL0(int, int (*)(void), g->fn[6]) == -2, "int result with garbage in the upper half of x0");
    }
    return failures;
}

#ifndef CPU_BACKEND_JIT
/* native mode: the same calls against host functions */
static uint64_t h_sum8(void *a, int b, int64_t c, unsigned d, void *e, int f, long long gg, uint64_t h) {
    return (uint64_t)(uintptr_t)a + 2 * (uint64_t)(int64_t)b + 3 * (uint64_t)c + 4 * (uint64_t)d + 5 * (uint64_t)(uintptr_t)e
         + 6 * (uint64_t)(int64_t)f + 7 * (uint64_t)gg + 8 * h;
}
static int64_t h_sxtw(int x) { return x; }
static void h_store(uint64_t *p, uint64_t v) { *p = v; }
static char *h_ptr16(char *p) { return p + 16; }
static int h_answer(void) { return 42; }
static bool h_bool(int x) { return x != 0; }
int main(void) {
    gc_fns g = {{(uint64_t)(uintptr_t)h_sum8, (uint64_t)(uintptr_t)h_sxtw, (uint64_t)(uintptr_t)h_store, (uint64_t)(uintptr_t)h_ptr16,
                 (uint64_t)(uintptr_t)h_answer, (uint64_t)(uintptr_t)h_bool, 0}, 0};
    int f = gc_run(&g);
    printf("native GUEST_CALL: %d failures\n", f);
    puts(f ? "RESULT fail" : "RESULT pass");
    return f != 0;
}
#endif
