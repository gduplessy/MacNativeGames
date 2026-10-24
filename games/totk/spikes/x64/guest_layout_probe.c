/* Spike X-S1 (docs/windows-android-port-plan.md, section 9.5): can a Windows x64 process hold the TOTK guest layout
   at its own addresses below 2^39, every launch?

   Mirrors runtime/services.c (macOS layout) and loader.c:
     low_space  6 GiB (stack +0x1000_0000, heap +0x3000_0000, cap 4 GiB), first fit from 0x3_0000_0000, 1 GiB stride
     alias      64 GiB at 0x70_0000_0000, else beside low_space, shrinking in 1 GiB steps down to 8 GiB
     modules    512 MiB anywhere below 2^39 (main + sdk + subsdk0 with patch areas)
   Then checks what the runtime does inside those regions, at Horizon's 4 KiB page size:
     T1 commit / write / decommit / recommit-is-zero of single 4 KiB pages (alias_map, heap)
     T2 4 KiB protection changes (stack_move)
     T3 two views of one pagefile-backed section at 4 KiB granularity in placeholders (virtual memory pools, mach_vm_remap)
     T4 Vulkan after the reservation: instance + device, VK_EXT_external_memory_host import of a guest heap page range
        and of a T3 alias view, and whether VK_EXT_map_memory_placed is offered
   Output: one "key=value" line per result, then "RESULT pass" or "RESULT fail <first failure>". Exit 0 on pass. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_LIMIT (1ULL << 39)
#define GIB (1ULL << 30)
#define MIB (1ULL << 20)
#define LOW_SEARCH_FROM 0x300000000ULL
#define LOW_SIZE (6 * GIB)
#define STACK_OFF 0x10000000ULL
#define HEAP_OFF 0x30000000ULL
#define HEAP_CAP (4 * GIB)
#define ALIAS_AT 0x7000000000ULL
#define ALIAS_MAX (64 * GIB)
#define ALIAS_MIN (8 * GIB)
#define MODULES_SIZE (512 * MIB)
#define PG 4096ULL

typedef PVOID(WINAPI *VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
static VirtualAlloc2_t pVirtualAlloc2;
static MapViewOfFile3_t pMapViewOfFile3;
static const char *first_failure;
static int verbose;

static void fail(const char *what) { if (!first_failure) first_failure = what; printf("FAIL %s (GetLastError=%lu)\n", what, GetLastError()); }
static void check(int ok, const char *what) { if (!ok) fail(what); }

/* Occupied regions below 2^39 before anything is reserved: what a fresh process already has there. */
static void survey(const char *when) {
    uint64_t a = 0x10000, used = 0, largest_free = 0, largest_at = 0; unsigned regions = 0;
    while (a < GUEST_LIMIT) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        uint64_t base = (uint64_t)mi.BaseAddress, end = base + mi.RegionSize; if (end > GUEST_LIMIT) end = GUEST_LIMIT;
        if (mi.State == MEM_FREE) { if (end - base > largest_free) { largest_free = end - base; largest_at = base; } }
        else {
            used += end - base; regions++;
            if (verbose) printf("  %s %#014llx-%#014llx %s type=%#lx\n", when, (unsigned long long)base, (unsigned long long)end,
                                mi.State == MEM_COMMIT ? "commit " : "reserve", mi.Type);
        }
        a = end;
    }
    printf("%s_regions_below_2^39=%u\n%s_used_below_2^39_mib=%llu\n%s_largest_free_gib=%.1f at %#llx\n", when, regions, when,
           (unsigned long long)(used / MIB), when, largest_free / (double)GIB, (unsigned long long)largest_at);
}

static uint8_t *reserve_fixed(uint64_t at, uint64_t size) {
    return VirtualAlloc((void *)at, size, MEM_RESERVE, PAGE_NOACCESS);
}
/* Anywhere below 2^39 (VirtualAlloc2 address requirements), like VM_FLAGS_ANYWHERE in a process whose map ends low. */
static uint8_t *reserve_below_limit(uint64_t size, ULONG type) {
    MEM_ADDRESS_REQUIREMENTS req = {0};
    req.LowestStartingAddress = (PVOID)LOW_SEARCH_FROM; req.HighestEndingAddress = (PVOID)(GUEST_LIMIT - 1); req.Alignment = 0x200000;
    MEM_EXTENDED_PARAMETER p = {0}; p.Type = MemExtendedParameterAddressRequirements; p.Pointer = &req;
    return pVirtualAlloc2(GetCurrentProcess(), NULL, size, type, PAGE_NOACCESS, &p, 1);
}

static uint8_t *low_space, *alias_space, *modules; static uint64_t alias_len; static const char *layout = "none";
static void reserve_layout(void) {
    /* macOS 26 order: alias at its fixed place, then stack+heap first fit below it. */
    alias_space = reserve_fixed(ALIAS_AT, ALIAS_MAX);
    if (alias_space) {
        for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= ALIAS_AT && !low_space; at += GIB) low_space = reserve_fixed(at, LOW_SIZE);
        alias_len = ALIAS_MAX; layout = "fixed";
    }
    /* macOS 27 / iOS order: both side by side in one hole, alias shrinking. */
    if (!low_space) {
        if (alias_space) VirtualFree(alias_space, 0, MEM_RELEASE);
        alias_space = NULL;
        for (uint64_t g = ALIAS_MAX; g >= ALIAS_MIN && !low_space; g -= GIB)
            if ((low_space = reserve_below_limit(LOW_SIZE + g, MEM_RESERVE))) { alias_space = low_space + LOW_SIZE; alias_len = g; layout = "side-by-side"; }
    }
    modules = reserve_below_limit(MODULES_SIZE, MEM_RESERVE);
    check(low_space != NULL, "reserve stack+heap below 2^39");
    check(alias_space != NULL, "reserve alias region below 2^39");
    check(modules != NULL, "reserve module range below 2^39");
    check(!low_space || (uint64_t)low_space + LOW_SIZE <= GUEST_LIMIT, "stack+heap ends below 2^39");
    check(!alias_space || (uint64_t)alias_space + alias_len <= GUEST_LIMIT, "alias ends below 2^39");
    printf("layout=%s\nlow_space=%#llx+%#llx\nalias=%#llx+%#llx (%llu GiB)\nmodules=%#llx+%#llx\n", layout,
           (unsigned long long)(uintptr_t)low_space, LOW_SIZE, (unsigned long long)(uintptr_t)alias_space, (unsigned long long)alias_len,
           (unsigned long long)(alias_len / GIB), (unsigned long long)(uintptr_t)modules, MODULES_SIZE);
}

/* T1: alias_map / heap growth at 4 KiB: fresh pages read zero, decommitted pages come back zero. */
static void t1_commit_4k(void) {
    if (!alias_space || !low_space) return;
    uint8_t *heap = low_space + HEAP_OFF;
    uint64_t offs[] = {0, PG, 7 * PG, 1 * MIB + 3 * PG, 2 * GIB + 5 * PG, alias_len - PG};
    int ok = 1;
    for (int r = 0; r < 2; r++) {
        uint8_t *base = r ? alias_space : heap; uint64_t lim = r ? alias_len : HEAP_CAP;
        for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) {
            if (offs[i] >= lim) continue;
            uint8_t *p = base + offs[i];
            if (!VirtualAlloc(p, PG, MEM_COMMIT, PAGE_READWRITE)) { ok = 0; continue; }
            for (unsigned k = 0; k < PG; k++) if (p[k]) ok = 0;
            memset(p, 0xa5, PG);
            /* the next page stays uncommitted (each test page is decommitted before the next): granularity is 4 KiB */
            MEMORY_BASIC_INFORMATION mi; VirtualQuery(p + PG, &mi, sizeof mi);
            if (offs[i] + PG < lim && mi.State == MEM_COMMIT) ok = 0;
            if (!VirtualFree(p, PG, MEM_DECOMMIT)) ok = 0;
            if (!VirtualAlloc(p, PG, MEM_COMMIT, PAGE_READWRITE)) { ok = 0; continue; }
            for (unsigned k = 0; k < PG; k++) if (p[k]) { ok = 0; break; }
            VirtualFree(p, PG, MEM_DECOMMIT);
        }
    }
    printf("t1_commit_4k=%s\n", ok ? "pass" : "fail"); check(ok, "T1 4 KiB commit/decommit");
}

/* T2: stack_move protects source pages at 4 KiB (Darwin rounds to 16 KiB). */
static volatile int t2_faulted;
static LONG CALLBACK t2_veh(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && t2_faulted == 0) {
        t2_faulted = 1; DWORD old; VirtualProtect((void *)e->ExceptionRecord->ExceptionInformation[1], 1, PAGE_READWRITE, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void t2_protect_4k(void) {
    if (!low_space) return;
    uint8_t *s = low_space + STACK_OFF; DWORD old; int ok = 1;
    ok &= VirtualAlloc(s, 4 * PG, MEM_COMMIT, PAGE_READWRITE) != NULL;
    ok &= VirtualProtect(s + PG, PG, PAGE_NOACCESS, &old) != 0;
    s[0] = 1; s[2 * PG] = 2;   /* neighbours stay writable */
    void *h = AddVectoredExceptionHandler(1, t2_veh);
    ((volatile uint8_t *)s)[PG] = 3;   /* faults once; the handler restores access (the dirty-tracking pattern) */
    RemoveVectoredExceptionHandler(h);
    ok &= t2_faulted == 1 && s[PG] == 3;
    VirtualFree(s, 4 * PG, MEM_DECOMMIT);
    printf("t2_protect_4k=%s\nt2_veh_fault_resume=%s\n", ok ? "pass" : "fail", t2_faulted ? "yes" : "no"); check(ok, "T2 4 KiB protect + VEH resume");
}

/* T3: a physical pool backed by a section, mapped a second time page by page into placeholders (virtual memory pool). */
static uint8_t *t3_view_a, *t3_view_b; static HANDLE t3_section;
static void t3_alias_4k(void) {
    if (!alias_space) return;
    const uint64_t POOL = 16 * MIB;
    t3_section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, (DWORD)(POOL >> 32), (DWORD)POOL, NULL);
    check(t3_section != NULL, "T3 pagefile section");
    if (!t3_section) return;
    /* A plain MEM_RESERVE region cannot be turned into placeholders in place, so the runtime would reserve its
       virtual-pool range as a placeholder from the start; this test uses its own placeholder below 2^39. */
    uint8_t *ph = reserve_below_limit(64 * MIB, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER);
    check(ph != NULL, "T3 placeholder reservation below 2^39");
    if (!ph) return;
    /* view A: the whole pool at the start; view B: pool pages 1..3 (4 KiB offset) at a 4 KiB-aligned address inside. */
    int ok = 1;
    ok &= VirtualFree(ph, POOL, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
    t3_view_a = pMapViewOfFile3(t3_section, GetCurrentProcess(), ph, 0, POOL, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    ok &= t3_view_a == ph;
    uint8_t *b = ph + 32 * MIB + 5 * PG;
    ok &= VirtualFree(ph + POOL, b - (ph + POOL), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;   /* split off [POOL, b) */
    ok &= VirtualFree(b, 3 * PG, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;                     /* split off [b, b+12K) */
    t3_view_b = pMapViewOfFile3(t3_section, GetCurrentProcess(), b, PG, 3 * PG, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    DWORD e = GetLastError();
    int view_b_ok = t3_view_b == b;
    if (view_b_ok && t3_view_a) {
        t3_view_a[PG + 17] = 0x5a; t3_view_b[2 * PG + 9] = 0xc3;
        ok &= t3_view_b[17] == 0x5a && t3_view_a[3 * PG + 9] == 0xc3;
    }
    printf("t3_view_4k_offset=%s%s\n", view_b_ok ? "pass" : "fail", view_b_ok ? "" : (e == ERROR_MAPPED_ALIGNMENT ? " (needs 64 KiB alignment)" : ""));
    if (!view_b_ok) printf("t3_view_4k_error=%lu\n", e);
    printf("t3_alias_coherent=%s\n", ok && view_b_ok ? "pass" : "fail");
    check(ok && view_b_ok, "T3 4 KiB section views in placeholders");
}

/* T4: Vulkan, declared by hand (no SDK on this machine): only what the probe calls. */
typedef struct { int sType; const void *pNext; const char *app; uint32_t appv; const char *eng; uint32_t engv; uint32_t api; } VkAppInfo;
typedef struct { int sType; const void *pNext; uint32_t flags; const VkAppInfo *app; uint32_t nl; const char *const *l; uint32_t ne; const char *const *e; } VkInstInfo;
typedef struct { int sType; const void *pNext; uint32_t flags; uint32_t family; uint32_t count; const float *prio; } VkQueueInfo;
typedef struct { int sType; const void *pNext; uint32_t flags; uint32_t nq; const VkQueueInfo *q; uint32_t nl; const char *const *l; uint32_t ne; const char *const *e; const void *feat; } VkDevInfo;
typedef struct { char name[256]; uint32_t spec; } VkExtProps;
typedef struct { uint32_t flags, count, tsbits, g[3]; } VkQFamProps;
typedef struct { int sType; void *pNext; uint64_t align; } VkHostProps;
typedef struct { int sType; void *pNext; uint8_t props[824]; } VkProps2;   /* VkPhysicalDeviceProperties is 824 bytes on x64 */
typedef struct { int sType; void *pNext; uint32_t bits; } VkHostPtrProps;
typedef struct { int sType; const void *pNext; uint32_t handle; void *ptr; } VkImportHost;
typedef struct { int sType; const void *pNext; uint64_t size; uint32_t type; } VkAllocInfo;
typedef void *VkH; typedef void (*PFN)(void);
typedef PFN (*GetInstanceProcAddr_t)(VkH, const char *);
#define VK_HOST_ALLOC_BIT 0x80
static void t4_vulkan(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    check(vk != NULL, "T4 load vulkan-1.dll"); if (!vk) return;
    GetInstanceProcAddr_t gipa = (GetInstanceProcAddr_t)GetProcAddress(vk, "vkGetInstanceProcAddr");
    int (*createInstance)(const VkInstInfo *, void *, VkH *) = (void *)gipa(NULL, "vkCreateInstance");
    VkAppInfo app = {0, NULL, "x-s1", 1, "totk", 1, (1u << 22) | (3u << 12)};
    VkInstInfo ii = {1, NULL, 0, &app, 0, NULL, 0, NULL}; VkH inst = NULL;
    int r = createInstance(&ii, NULL, &inst); printf("t4_create_instance=%d\n", r); check(r == 0, "T4 vkCreateInstance"); if (r) return;
#define IP(R, n, ...) R (*n)(__VA_ARGS__) = (void *)gipa(inst, #n)
    IP(int, vkEnumeratePhysicalDevices, VkH, uint32_t *, VkH *);
    IP(int, vkEnumerateDeviceExtensionProperties, VkH, const char *, uint32_t *, VkExtProps *);
    IP(void, vkGetPhysicalDeviceQueueFamilyProperties, VkH, uint32_t *, VkQFamProps *);
    IP(void, vkGetPhysicalDeviceProperties2, VkH, VkProps2 *);
    IP(int, vkCreateDevice, VkH, const VkDevInfo *, void *, VkH *);
    IP(PFN, vkGetDeviceProcAddr, VkH, const char *);
    uint32_t n = 1; VkH pd = NULL; vkEnumeratePhysicalDevices(inst, &n, &pd); check(n > 0 && pd, "T4 physical device"); if (!pd) return;
    VkHostProps hp = {1000178002, NULL, 0}; VkProps2 p2 = {1000059001, &hp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    uint32_t api = *(uint32_t *)p2.props; const char *name = (const char *)p2.props + 20;
    printf("t4_device=%s\nt4_api=%u.%u.%u\n", name, api >> 22, (api >> 12) & 0x3ff, api & 0xfff);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtProps *ext = calloc(ne, sizeof *ext); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int has_host = 0, has_placed = 0;
    for (uint32_t i = 0; i < ne; i++) { has_host |= !strcmp(ext[i].name, "VK_EXT_external_memory_host"); has_placed |= !strcmp(ext[i].name, "VK_EXT_map_memory_placed"); }
    printf("t4_ext_external_memory_host=%s\nt4_ext_map_memory_placed=%s\nt4_min_host_pointer_alignment=%#llx\n", has_host ? "yes" : "no",
           has_placed ? "yes" : "no", (unsigned long long)hp.align);
    uint32_t nf = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, NULL);
    VkQFamProps *qf = calloc(nf, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, qf);
    uint32_t fam = 0; for (uint32_t i = 0; i < nf; i++) if (qf[i].flags & 1) { fam = i; break; }
    float prio = 1; VkQueueInfo qi = {2, NULL, 0, fam, 1, &prio};
    const char *exts[] = {"VK_KHR_external_memory", "VK_EXT_external_memory_host"};
    VkDevInfo di = {3, NULL, 0, 1, &qi, 0, NULL, has_host ? 2 : 0, exts, NULL}; VkH dev = NULL;
    r = vkCreateDevice(pd, &di, NULL, &dev); printf("t4_create_device=%d\n", r); check(r == 0, "T4 vkCreateDevice"); if (r || !has_host) return;
    int (*getHostPtrProps)(VkH, uint32_t, const void *, VkHostPtrProps *) = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    int (*allocMemory)(VkH, const VkAllocInfo *, void *, VkH *) = (void *)vkGetDeviceProcAddr(dev, "vkAllocateMemory");
    void (*freeMemory)(VkH, VkH, void *) = (void *)vkGetDeviceProcAddr(dev, "vkFreeMemory");
    /* (a) a committed guest heap range, (b) the T3 second view of a section (aliased guest memory) */
    struct { const char *what; uint8_t *ptr; uint64_t size; } cases[2] = {{"heap", NULL, 2 * MIB}, {"alias_view", t3_view_a, 2 * MIB}};
    if (low_space) { cases[0].ptr = low_space + HEAP_OFF + 64 * MIB; VirtualAlloc(cases[0].ptr, cases[0].size, MEM_COMMIT, PAGE_READWRITE); }
    for (int c = 0; c < 2; c++) {
        if (!cases[c].ptr) { printf("t4_import_%s=skipped\n", cases[c].what); continue; }
        VkHostPtrProps hpp = {1000178001, NULL, 0};
        int rp = getHostPtrProps(dev, VK_HOST_ALLOC_BIT, cases[c].ptr, &hpp);
        uint32_t type = 0; while (type < 32 && !(hpp.bits >> type & 1)) type++;
        VkImportHost imp = {1000178000, NULL, VK_HOST_ALLOC_BIT, cases[c].ptr};
        VkAllocInfo ai = {5, &imp, cases[c].size, type}; VkH mem = NULL;
        int ra = rp == 0 && type < 32 ? allocMemory(dev, &ai, NULL, &mem) : -1;
        printf("t4_import_%s=%s (props=%d bits=%#x alloc=%d)\n", cases[c].what, ra == 0 ? "pass" : "fail", rp, hpp.bits, ra);
        if (c == 0) check(ra == 0, "T4 import guest heap memory into Vulkan");
        if (mem) freeMemory(dev, mem, NULL);
    }
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    pVirtualAlloc2 = (VirtualAlloc2_t)GetProcAddress(kb, "VirtualAlloc2");
    pMapViewOfFile3 = (MapViewOfFile3_t)GetProcAddress(kb, "MapViewOfFile3");
    if (!pVirtualAlloc2 || !pMapViewOfFile3) { puts("RESULT fail VirtualAlloc2/MapViewOfFile3 missing"); return 1; }
    survey("start");
    reserve_layout();
    t1_commit_4k();
    t2_protect_4k();
    t3_alias_4k();
    /* libraries a game process loads after the reservation must still find room (they load high anyway) */
    const char *libs[] = {"d3d12.dll", "dxgi.dll", "xinput1_4.dll", "winmm.dll", "ws2_32.dll", "mmdevapi.dll", "hid.dll"};
    int loaded = 0; for (unsigned i = 0; i < sizeof libs / sizeof libs[0]; i++) loaded += LoadLibraryA(libs[i]) != NULL;
    printf("system_libs_loaded=%d/%u\n", loaded, (unsigned)(sizeof libs / sizeof libs[0]));
    t4_vulkan();
    survey("end");
    if (first_failure) printf("RESULT fail %s\n", first_failure); else puts("RESULT pass");
    return first_failure != NULL;
}
