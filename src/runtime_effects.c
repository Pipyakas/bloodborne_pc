/* Live graphics effect switches (v1.09): the community patches that turn effects off are
 * applied at start (scripts/patches.py); this module switches them while the game runs, so
 * the game's options screen (runtime_menu.c) does not need a restart.
 *
 * Two kinds of switch:
 *   - Code patches read every frame (depth of field: a je that skips the pass becomes a jmp;
 *     chromatic aberration: the parameter copy becomes a store of 0). Rewriting several
 *     bytes of code that other threads may be executing is done with every other thread
 *     suspended and none of them stopped inside the bytes (else resume and retry).
 *   - Flags in the render settings objects (SSAO, motion blur: bytes +0x2968/+0x2969, set by
 *     the constructor 0x22c1050 and read every frame). The objects are tracked from their
 *     constructor (its seven call sites) to their destructor (0x22c2e10, a detour on its
 *     first instructions); a switch writes the byte in each live object, and new objects
 *     get the current setting.
 * The startup patches still set the initial state; code bytes are only rewritten when they
 * match the original or the patched form exactly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

typedef struct {
    const char *key;               /* bbport.ini / BbSettings effect key */
    uint64_t at;                   /* image offset of a code patch, 0 for a flag */
    unsigned char original[12], patched[12];
    unsigned size;
    uint32_t flag;                 /* render settings object offset, 0 for a code patch */
} LiveEffect;

static const LiveEffect effects[] = {
    {"effect_dof", 0x21d7bbc, {0x0f, 0x84, 0xde, 0x00, 0x00, 0x00}, {0xe9, 0xdf, 0x00, 0x00, 0x00, 0x90}, 6, 0},
    {"effect_chromatic_aberration", 0x229faa8,
     {0x8b, 0x85, 0x90, 0xf5, 0xff, 0xff, 0x89, 0x83, 0xac, 0x00, 0x00, 0x00},
     {0xc7, 0x83, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90}, 12, 0},
    {"effect_ssao", 0, {0}, {0}, 0, 0x2968},
    {"effect_motion_blur", 0, {0}, {0}, 0, 0x2969},
};
#define EFFECT_COUNT (sizeof(effects) / sizeof(*effects))

#define SETTINGS_CTOR 0x22c1050
#define SETTINGS_DTOR 0x22c2e10
static const uint64_t ctor_calls[] = {0x229598f, 0x229599e, 0x22c5217, 0x22c5313,
                                      0x22f8cc8, 0x22f9389, 0x22f99b3};
/* The destructor's first instructions (push rbp; mov rbp, rsp; push r14), moved into the
 * detour; the jump back lands after them. */
static const unsigned char dtor_prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};

static unsigned char *guest;
static int applied[EFFECT_COUNT]; /* 1 effect on, 0 off, -1 unknown (bytes not recognised) */

/* ---- render settings objects ---- */
static HostMutex objects_lock = HOST_MUTEX_INIT;
static unsigned char *objects[128];
static unsigned object_count;

static void write_flags(unsigned char *object) {
    for (size_t e = 0; e < EFFECT_COUNT; ++e)
        if (effects[e].flag && applied[e] >= 0)
            __atomic_store_n(object + effects[e].flag, (unsigned char)applied[e], __ATOMIC_RELAXED);
}

typedef void *(ABI *Ctor)(void *self, void *arg);
static ABI void *settings_created(void *self, void *arg) {
    void *result = ((Ctor)(guest + SETTINGS_CTOR))(self, arg);
    host_lock(&objects_lock);
    if (object_count < sizeof(objects) / sizeof(*objects)) objects[object_count++] = self;
    write_flags(self);
    host_unlock(&objects_lock);
    return result;
}

static ABI void settings_destroyed(void *self) {
    host_lock(&objects_lock);
    for (unsigned i = 0; i < object_count; ++i)
        if (objects[i] == self) { objects[i] = objects[--object_count]; break; }
    host_unlock(&objects_lock);
}

/* ---- code patches with the other threads stopped ---- */
#ifdef _WIN32
static int patch_code(unsigned char *at, const unsigned char *bytes, unsigned size) {
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    static HANDLE threads[1024];
    for (int attempt = 0; attempt < 200; ++attempt) {
        unsigned count = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 entry = {.dwSize = sizeof(entry)};
        for (BOOL more = Thread32First(snap, &entry); more && count < 1024; more = Thread32Next(snap, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
            if (h) threads[count++] = h;
        }
        CloseHandle(snap);
        /* From here until the threads resume: no allocation, no locks another thread may hold. */
        unsigned suspended = 0;
        int busy = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (SuspendThread(threads[i]) == (DWORD)-1) { CloseHandle(threads[i]); threads[i] = NULL; continue; }
            ++suspended;
            CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
            if (GetThreadContext(threads[i], &context) &&
                context.Rip + 16 > (uintptr_t)at && context.Rip < (uintptr_t)at + size) busy = 1;
        }
        if (!busy) {
            DWORD old;
            if (VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(at, bytes, size);
                VirtualProtect(at, size, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, size);
            } else busy = -1;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (!threads[i]) continue;
            ResumeThread(threads[i]);
            CloseHandle(threads[i]);
        }
        (void)suspended;
        if (busy == 0) return 1;
        if (busy < 0) return 0;
        Sleep(2);
    }
    return 0;
}
#else
static int patch_code(unsigned char *at, const unsigned char *bytes, unsigned size) {
    (void)at; (void)bytes; (void)size;
    return 0; /* Linux: the startup patch only (changes apply after a restart) */
}
#endif

/* Brings each effect to its setting. */
static void apply_effects(void) {
    for (size_t e = 0; e < EFFECT_COUNT; ++e) {
        const int want = bbgpu_effect_enabled(effects[e].key) ? 1 : 0;
        if (applied[e] < 0 || applied[e] == want) continue;
        if (effects[e].flag) {
            host_lock(&objects_lock);
            applied[e] = want;
            for (unsigned i = 0; i < object_count; ++i) write_flags(objects[i]);
            const unsigned live = object_count;
            host_unlock(&objects_lock);
            printf("Runtime: %s %s (%u render settings objects)\n", effects[e].key, want ? "on" : "off", live);
            bbgpu_effect_live(effects[e].key, want);
        } else if (patch_code(guest + effects[e].at, want ? effects[e].original : effects[e].patched,
                              effects[e].size)) {
            applied[e] = want;
            printf("Runtime: %s %s\n", effects[e].key, want ? "on" : "off");
            bbgpu_effect_live(effects[e].key, want);
        } else {
            printf("Runtime: %s: cannot switch while running; applies after a restart\n", effects[e].key);
            applied[e] = -1;
            bbgpu_effect_live(effects[e].key, -1);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI effects_thread(void *unused) {
    (void)unused;
    runtime_win_set_thread_name("bb:effects");
    for (;;) {
        Sleep(100);
        apply_effects();
    }
    return 0;
}
#else
static void *effects_thread(void *unused) {
    (void)unused;
    for (;;) {
        usleep(100000);
        apply_effects();
    }
    return NULL;
}
#endif

static int call_targets(uint64_t site, uint64_t target) {
    int32_t rel;
    memcpy(&rel, guest + site + 1, 4);
    return guest[site] == 0xe8 && site + 5 + (int64_t)rel == target;
}

/* Before the patches and before the game runs. stubs: a writable page within rel32 range of
 * the image (made executable by the loader). */
unsigned runtime_effects_install(unsigned char *image, uint64_t image_size, unsigned char *stubs) {
    guest = image;
    int known = image_size > SETTINGS_DTOR + 16 && !memcmp(image + SETTINGS_DTOR, dtor_prologue, 6);
    for (size_t i = 0; i < sizeof(ctor_calls) / sizeof(*ctor_calls); ++i)
        known = known && call_targets(ctor_calls[i], SETTINGS_CTOR);
    for (size_t e = 0; e < EFFECT_COUNT; ++e) {
        if (!effects[e].at) { applied[e] = 1; continue; } /* the object flags are written below */
        if (!memcmp(image + effects[e].at, effects[e].original, effects[e].size)) applied[e] = 1;
        else if (!memcmp(image + effects[e].at, effects[e].patched, effects[e].size)) applied[e] = 0;
        else { applied[e] = -1; known = 0; }
    }
    if (!known) {
        puts("Runtime: effects code not recognised (not v1.09?): effect switches apply after a restart");
        return 0;
    }
    /* The startup settings decide the objects' flags from their first construction. */
    for (size_t e = 0; e < EFFECT_COUNT; ++e)
        if (effects[e].flag) applied[e] = bbgpu_effect_enabled(effects[e].key) ? 1 : 0;
    unsigned char *p = stubs;
    /* Constructor call sites: call stub; stub: movabs rax, settings_created; jmp rax. */
    for (size_t i = 0; i < sizeof(ctor_calls) / sizeof(*ctor_calls); ++i) {
        const uintptr_t host = (uintptr_t)settings_created;
        p[0] = 0x48; p[1] = 0xb8; memcpy(p + 2, &host, 8); p[10] = 0xff; p[11] = 0xe0;
        const int32_t rel = (int32_t)(p - (image + ctor_calls[i] + 5));
        memcpy(image + ctor_calls[i] + 1, &rel, 4);
        p += 16;
    }
    /* Destructor detour: push rdi; movabs rax, settings_destroyed; call rax; pop rdi;
     * the moved prologue; jmp back. Entry rsp is 8 mod 16, so the call is aligned. */
    const uintptr_t host = (uintptr_t)settings_destroyed;
    unsigned char *d = p;
    *d++ = 0x57;
    *d++ = 0x48; *d++ = 0xb8; memcpy(d, &host, 8); d += 8;
    *d++ = 0xff; *d++ = 0xd0;
    *d++ = 0x5f;
    memcpy(d, dtor_prologue, 6); d += 6;
    const int32_t back = (int32_t)((image + SETTINGS_DTOR + 6) - (d + 5));
    *d++ = 0xe9; memcpy(d, &back, 4); d += 4;
    const int32_t there = (int32_t)(p - (image + SETTINGS_DTOR + 5));
    image[SETTINGS_DTOR] = 0xe9; memcpy(image + SETTINGS_DTOR + 1, &there, 4);
    image[SETTINGS_DTOR + 5] = 0x90;
    for (size_t e = 0; e < EFFECT_COUNT; ++e) bbgpu_effect_live(effects[e].key, applied[e]);
#ifdef _WIN32
    CreateThread(NULL, 0, effects_thread, NULL, 0, NULL);
#else
    pthread_t thread;
    if (!pthread_create(&thread, NULL, effects_thread, NULL)) pthread_detach(thread);
#endif
    return (unsigned)(sizeof(ctor_calls) / sizeof(*ctor_calls)) + 1;
}
