/* Native functions, host functions, guards and probes reached from guest code (tools/cpu/test_jit.sh runs it
 * translated and interpreted): each runs once per call, with the guest's registers. */
#include "../src/cpu/bbcpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

uint64_t bb_image_base;

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

enum { RAX = 0, RDI = 7 };
enum { CALL_NATIVE = 0x000, CALL_GUARDED = 0x040, NATIVE = 0x100, GUARDED = 0x200, FALLS_INTO = 0x300,
       PROBED = 0x305, PROBED_INTERPRETED = 0x340, GUARDED_TAIL = 0x380, CALL_HOST = 0x400,
       HOST_CHAIN = 0x480, CALLS = 1000 };

static uint8_t *guest;

static void put(uint32_t at, const uint8_t *bytes, size_t size) { memcpy(guest + at, bytes, size); }
static void put_call(uint32_t at, uint32_t target) {
    const int32_t rel = (int32_t)(target - (at + 5));
    guest[at] = 0xe8;
    memcpy(guest + at + 1, &rel, 4);
}

static uint64_t native_function(uint64_t a) { return a * 3; }

static uint64_t host_calls, host_arguments;
static uint64_t host_function(uint64_t a) {
    ++host_calls;
    host_arguments += a;
    return a * 2 + 1;
}

static uint64_t chain_first_calls, chain_second_calls;
static void chain_first(void) { ++chain_first_calls; }
static uint64_t chain_second(void) {
    assert(chain_first_calls == chain_second_calls + 1);
    return ++chain_second_calls;
}

static uint64_t guard_enters, guard_leaves, guard_arguments;
static uint64_t guard_enter(const uint64_t regs[16], uint32_t tag) {
    assert(tag == 7);
    ++guard_enters;
    guard_arguments += regs[RDI];
    return guard_enters;
}
static void guard_leave(uint64_t cookie) {
    ++guard_leaves;
    assert(cookie == guard_leaves);
}

static uint64_t tail_enters, tail_leaves, inner_leaves_at_tail_leave;
static uint64_t tail_enter(const uint64_t regs[16], uint32_t tag) {
    (void)regs;
    assert(tag == 8);
    return ++tail_enters;
}
static void tail_leave(uint64_t cookie) {
    assert(cookie == tail_enters);
    ++tail_leaves;
    inner_leaves_at_tail_leave = guard_leaves;
}

static uint64_t probe_hits, probe_rax, interpreted_probe_hits;
static void probe(const uint64_t regs[16], uint64_t address, uint32_t tag) {
    assert(address == (uintptr_t)guest + PROBED && tag == 3);
    ++probe_hits;
    probe_rax = regs[RAX];
}
static void interpreted_probe(const uint64_t regs[16], uint64_t address, uint32_t tag) {
    (void)regs;
    assert(address == (uintptr_t)guest + PROBED_INTERPRETED && tag == 4);
    ++interpreted_probe_hits;
}

int main(void) {
    guest = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guest != MAP_FAILED);
    memset(guest, 0xcc, 4096);
    /* call NATIVE; ret */
    put_call(CALL_NATIVE, NATIVE);
    guest[CALL_NATIVE + 5] = 0xc3;
    /* NATIVE: mov eax, 1; ret (replaced) */
    put(NATIVE, (const uint8_t[]){0xb8, 1, 0, 0, 0, 0xc3}, 6);
    /* mov ebx, edi; loop: mov edi, ebx; call GUARDED; dec ebx; jnz loop; ret */
    put(CALL_GUARDED, (const uint8_t[]){0x89, 0xfb, 0x89, 0xdf}, 4);
    put_call(CALL_GUARDED + 4, GUARDED);
    put(CALL_GUARDED + 9, (const uint8_t[]){0xff, 0xcb, 0x75, 0xf5, 0xc3}, 5);
    /* GUARDED: push rbp; mov eax, edi; add eax, 1; pop rbp; ret */
    put(GUARDED, (const uint8_t[]){0x55, 0x89, 0xf8, 0x83, 0xc0, 0x01, 0x5d, 0xc3}, 8);
    /* FALLS_INTO: mov eax, 5; PROBED: add eax, 2; ret */
    put(FALLS_INTO, (const uint8_t[]){0xb8, 5, 0, 0, 0, 0x83, 0xc0, 0x02, 0xc3}, 9);
    /* PROBED_INTERPRETED: cld (interpreted inside translated code); mov eax, 9; ret */
    put(PROBED_INTERPRETED, (const uint8_t[]){0xfc, 0xb8, 9, 0, 0, 0, 0xc3}, 7);
    /* GUARDED_TAIL: jmp GUARDED (both guarded calls return at once) */
    put_call(GUARDED_TAIL, GUARDED);
    guest[GUARDED_TAIL] = 0xe9;
    /* mov ebx, edi; loop: mov edi, ebx; mov rax, host_function; call rax; dec ebx; jnz loop; ret */
    const uint64_t host = (uintptr_t)&host_function;
    put(CALL_HOST, (const uint8_t[]){0x89, 0xfb, 0x89, 0xdf, 0x48, 0xb8}, 6);
    put(CALL_HOST + 6, (const uint8_t *)&host, 8);
    put(CALL_HOST + 14, (const uint8_t[]){0xff, 0xd0, 0xff, 0xcb, 0x75, 0xee, 0xc3}, 7);
    /* HOST_CHAIN: mov rax, chain_second; push rax; mov rax, chain_first; jmp rax (chain_first
     * returns into chain_second, which returns to the caller) */
    const uint64_t first = (uintptr_t)&chain_first, second = (uintptr_t)&chain_second;
    put(HOST_CHAIN, (const uint8_t[]){0x48, 0xb8}, 2);
    put(HOST_CHAIN + 2, (const uint8_t *)&second, 8);
    put(HOST_CHAIN + 10, (const uint8_t[]){0x50, 0x48, 0xb8}, 3);
    put(HOST_CHAIN + 13, (const uint8_t *)&first, 8);
    put(HOST_CHAIN + 21, (const uint8_t[]){0xff, 0xe0}, 2);

    const uintptr_t base = (uintptr_t)guest;
    bbcpu_add_guest_code(base, 4096);
    bbcpu_set_native(base + NATIVE, (const void *)&native_function);
    bbcpu_set_guard(base + GUARDED, guard_enter, guard_leave, 7);
    bbcpu_set_guard(base + GUARDED_TAIL, tail_enter, tail_leave, 8);
    bbcpu_set_probe(base + PROBED, probe, 3);
    bbcpu_set_probe(base + PROBED_INTERPRETED, interpreted_probe, 4);

    for (uint64_t i = 1; i <= 3; ++i) assert(bbcpu_call(base + CALL_NATIVE, 1, &i) == i * 3);

    const uint64_t calls = CALLS;
    bbcpu_call(base + CALL_GUARDED, 1, &calls);
    assert(guard_enters == CALLS && guard_leaves == CALLS);
    assert(guard_arguments == (uint64_t)CALLS * (CALLS + 1) / 2);
    assert(bbcpu_call(base + GUARDED, 1, &calls) == CALLS + 1);
    assert(guard_enters == CALLS + 1 && guard_leaves == CALLS + 1);
    for (uint64_t i = 1; i <= 3; ++i) {
        assert(bbcpu_call(base + GUARDED_TAIL, 1, &i) == i + 1);
        assert(tail_enters == i && tail_leaves == i);
        assert(guard_leaves == CALLS + 1 + i && inner_leaves_at_tail_leave == guard_leaves);
    }

    assert(bbcpu_call(base + CALL_HOST, 1, &calls) == 3);
    assert(host_calls == CALLS && host_arguments == (uint64_t)CALLS * (CALLS + 1) / 2);
    for (uint64_t i = 1; i <= 3; ++i) assert(bbcpu_call(base + HOST_CHAIN, 0, NULL) == i);

    for (int i = 1; i <= 3; ++i) {
        assert(bbcpu_call(base + FALLS_INTO, 0, NULL) == 7);
        assert(probe_hits == (uint64_t)i && probe_rax == 5);
    }
    bbcpu_call(base + PROBED, 0, NULL);
    assert(probe_hits == 4);
    for (uint64_t i = 1; i <= 3; ++i) {
        assert(bbcpu_call(base + PROBED_INTERPRETED, 0, NULL) == 9);
        assert(interpreted_probe_hits == i);
    }
    puts("PASS: native functions, host functions, guards and probes");
    return 0;
}
