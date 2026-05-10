/*
 * of_cxxabi.cpp -- Minimal C++ ABI shim for openfpgaOS apps.
 *
 * Apps that use C++ language features (classes, virtual methods, new /
 * delete, static constructors) link this file instead of the toolchain's
 * libsupc++. We provide our own because the riscv-elf gcc's libsupc++ is
 * built against newlib and references newlib-only symbols (_impure_ptr,
 * _Unwind_Resume, ...) that musl doesn't ship.
 *
 * Build with -fno-exceptions -fno-rtti (sdk.mk does this for SRCS_CXX).
 *
 * What you DO get:
 *   - operator new / new[] / delete / delete[] (-> musl malloc/free)
 *   - __cxa_pure_virtual hook (calls abort)
 *   - Static destructor registration (__cxa_atexit / __cxa_finalize)
 *   - Static-init guards (__cxa_guard_acquire/release/abort)
 *
 * What you DON'T get:
 *   - C++ standard library (std::vector, std::string, std::cout, ...)
 *   - Exceptions (-fno-exceptions)
 *   - RTTI / dynamic_cast (-fno-rtti)
 *
 * For std::cout-style I/O, use printf from <stdio.h> instead.
 */

#include <stdlib.h>     /* musl: malloc, free, abort */

extern "C" {

/* ── operator new / delete ──────────────────────────────────────── */

void *__cxa_allocate(unsigned int size) {
    void *p = malloc(size);
    if (!p) abort();    /* match the C++ standard's "throw bad_alloc"
                         * behavior, but without exceptions enabled */
    return p;
}

/* ── Pure virtual handler ────────────────────────────────────────
 * Called if a pure-virtual method is invoked through a pointer to a
 * partially-constructed object. Should never happen in correct code. */
void __cxa_pure_virtual() {
    abort();
}

/* ── DSO handle (required by __cxa_atexit) ──────────────────────── */
void *__dso_handle = nullptr;

/* ── Static destructor registration ──────────────────────────────
 * The compiler emits calls to __cxa_atexit for each global object's
 * destructor. We record up to N callbacks and run them on program exit
 * via __cxa_finalize (called from musl's exit() through .fini_array). */
#define CXA_ATEXIT_MAX 32
static struct {
    void (*destructor)(void *);
    void *arg;
    void *dso;
} __cxa_atexit_funcs[CXA_ATEXIT_MAX];
static int __cxa_atexit_count = 0;

int __cxa_atexit(void (*destructor)(void *), void *arg, void *dso_handle) {
    if (__cxa_atexit_count >= CXA_ATEXIT_MAX) return -1;
    __cxa_atexit_funcs[__cxa_atexit_count].destructor = destructor;
    __cxa_atexit_funcs[__cxa_atexit_count].arg        = arg;
    __cxa_atexit_funcs[__cxa_atexit_count].dso        = dso_handle;
    __cxa_atexit_count++;
    return 0;
}

void __cxa_finalize(void *dso) {
    for (int i = __cxa_atexit_count - 1; i >= 0; i--) {
        if (dso == nullptr || __cxa_atexit_funcs[i].dso == dso) {
            if (__cxa_atexit_funcs[i].destructor) {
                __cxa_atexit_funcs[i].destructor(__cxa_atexit_funcs[i].arg);
                __cxa_atexit_funcs[i].destructor = nullptr;
            }
        }
    }
}

/* ── Thread-safe static-init guards ──────────────────────────────
 * Single-threaded environment -- no real locking needed. The "guard"
 * is a 64-bit value where the LSB tracks "already initialized". */
int __cxa_guard_acquire(long long *guard) {
    return !*reinterpret_cast<char *>(guard);
}

void __cxa_guard_release(long long *guard) {
    *reinterpret_cast<char *>(guard) = 1;
}

void __cxa_guard_abort(long long *) {
    /* Nothing to roll back in a single-threaded environment. */
}

}  /* extern "C" */

/* ── operator new / delete ──────────────────────────────────────── */

typedef __SIZE_TYPE__ size_t;

void *operator new(size_t size)              { return __cxa_allocate(size); }
void *operator new[](size_t size)            { return __cxa_allocate(size); }
void  operator delete(void *p) noexcept      { free(p); }
void  operator delete[](void *p) noexcept    { free(p); }
void  operator delete(void *p, size_t) noexcept   { free(p); }
void  operator delete[](void *p, size_t) noexcept { free(p); }
