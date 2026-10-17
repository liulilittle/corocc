#include "../harness.h"
#include "../palt_threads.h"          /* pt_thread_* for the C2 storm cases */
#include "coroutine/coro_runtime.h"   /* internal: the current coroutine */
#include "coroutine/coro_internal.h"  /* internal: coro_get_sched_tag (SV dump) */
#include "scheduler/rt.h"             /* internal: SV frozen-state walk shapes */
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>        /* coro_monotonic_ns */
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h> /* Sleep in the thread-side wait below */
#else
#include <sched.h>   /* sched_yield in the C2 storm helpers */
#include <unistd.h>  /* _exit on the watchdog timeout path */
#endif

/* A cancellation consumed at a park's safe point -- the yield never switches
 * out -- must leave the node usable.
 * Every park site retires RT_WAITING and RT_PARKED in its cancel branch, so
 * the coroutine's next plain yield requeues it normally. Without the marker
 * clear the driver mistakes the node for a parked one at its next settle,
 * drops it off the run queue with no releaser left to requeue it, and the
 * coroutine is silently lost (wait_all hangs); coro_rt_sleep additionally
 * strands its deadline in the timer heap, which later drives the node a
 * second time.
 *
 * Each site is pinned on a single worker with a deterministic shape:
 *   controller: sets up the blocking state, spawns the victim, yields;
 *   victim:     latches a self-cancellation, calls the primitive (it parks,
 *               the safe point consumes the request and the cancel branch
 *               reports), then does ONE plain coro_rt_yield and raises its
 *               flag.
 * The victim's flag, wait_all's return and the clean destroy are the green
 * evidence; removing any marker clear strands the victim and hangs.
 */

/* A short thread-side wait, for the sleep site's post-deadline window. */
static void sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---- mutex ---- */

static coro_rt_mutex_t cp_mutex;
static coro_status     cp_mutex_st;
static unsigned        cp_mutex_ran;

static void cp_mutex_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_mutex_st = coro_rt_mutex_lock(rt, &cp_mutex);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_mutex_ran;
}

static void cp_mutex_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_mutex_lock(rt, &cp_mutex) != CORO_OK) return;
    if (coro_rt_spawn(rt, NULL, cp_mutex_victim, rt) != CORO_OK) return;
    (void)coro_yield();   /* the victim parks, is cancelled, abstains */
    (void)coro_yield();   /* the victim runs again after its plain yield */
    (void)coro_rt_mutex_unlock(rt, &cp_mutex);
}

/* ---- condition variable ---- */

static coro_rt_cond_t  cp_cond;
static coro_rt_mutex_t cp_cond_m;
static coro_status     cp_cond_st;
static unsigned        cp_cond_ran;

static void cp_cond_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_mutex_lock(rt, &cp_cond_m) != CORO_OK) return;
    coro_cancel(coro_runtime_current()->current);
    /* The cancel branch re-acquires the mutex before reporting, so the
       status is the re-acquisition (CORO_OK), not the cancellation. */
    cp_cond_st = coro_rt_cond_wait(rt, &cp_cond, &cp_cond_m);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_cond_ran;
    (void)coro_rt_mutex_unlock(rt, &cp_cond_m);
}

static void cp_cond_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_spawn(rt, NULL, cp_cond_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
}

/* ---- counting semaphore ---- */

static coro_rt_sem_t cp_sem;
static coro_status  cp_sem_st;
static unsigned     cp_sem_ran;

static void cp_sem_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_sem_st = coro_rt_sem_wait(rt, &cp_sem);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_sem_ran;
}

static void cp_sem_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_spawn(rt, NULL, cp_sem_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
}

/* ---- read/write lock: reader behind a writer ---- */

static coro_rt_rwlock_t cp_rw;
static coro_status      cp_rd_st;
static unsigned         cp_rd_ran;

static void cp_rd_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_rd_st = coro_rt_rwlock_rdlock(rt, &cp_rw);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_rd_ran;
}

static void cp_rd_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_rwlock_wrlock(rt, &cp_rw) != CORO_OK) return;
    if (coro_rt_spawn(rt, NULL, cp_rd_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
    (void)coro_rt_rwlock_wrunlock(rt, &cp_rw);
}

/* ---- read/write lock: writer behind a reader ---- */

static coro_status cp_wr_st;
static unsigned    cp_wr_ran;

static void cp_wr_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_wr_st = coro_rt_rwlock_wrlock(rt, &cp_rw);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_wr_ran;
}

static void cp_wr_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_rwlock_rdlock(rt, &cp_rw) != CORO_OK) return;
    if (coro_rt_spawn(rt, NULL, cp_wr_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
    (void)coro_rt_rwlock_rdunlock(rt, &cp_rw);
}

/* ---- channel: sender on a full bounded channel ---- */

static coro_rt_channel_t cp_ch;
static coro_status       cp_send_st;
static unsigned          cp_send_ran;

static void cp_send_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_send_st = coro_rt_channel_send(rt, &cp_ch, (void*)1);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_send_ran;
}

static void cp_send_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_channel_trysend(rt, &cp_ch, (void*)2) != CORO_OK) return;
    if (coro_rt_spawn(rt, NULL, cp_send_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
}

/* ---- channel: receiver on an empty channel ---- */

static coro_status cp_recv_st;
static unsigned    cp_recv_ran;

static void cp_recv_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    void* v = NULL;
    coro_cancel(coro_runtime_current()->current);
    cp_recv_st = coro_rt_channel_recv(rt, &cp_ch, &v);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_recv_ran;
}

static void cp_recv_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_spawn(rt, NULL, cp_recv_victim, rt) != CORO_OK) return;
    (void)coro_yield();
    (void)coro_yield();
}

/* ---- sleep: the deadline must leave the heap with the cancellation ----
 *
 * The single plain-yield form does not discriminate on its own: pre-fix
 * the stranded node is "late-revived" by the abandoned deadline cas_clear
 * at +50ms (resumes becomes 1 and wait_all returns, just late), so the
 * site needs independent discriminators:
 *   (a) the double-yield snapshot (primary, no clock margin): after spawn,
 *       two controller yields and a snapshot of the resume flag. FIXED:
 *       the victim's plain yield is requeued at once; drain_inbox puts the
 *       victim above the controller, so it runs to completion before the
 *       controller's second resume -- snapshot == 1. UNFIXED: the node sits
 *       off the queue until the deadline revives it -- snapshot == 0.
 *   (b) the timing check (auxiliary): the controller re-checks the flag
 *       after a coroutine sleep well before the deadline; a late revival
 *       still shows 0 there. (No thread-side wait inside a coroutine: it
 *       would strand the single worker, hence coro_rt_sleep.)
 *   (c) the victim that RETURNS right after the cancelled sleep: pre-fix
 *       the heap entry then drives the already-freed node (ASan
 *       heap-use-after-free) -- the definitive red signal.
 */

static coro_status cp_sleep_st;
static unsigned    cp_sleepy_resumes;
static unsigned    cp_sleepy_early;     /* snapshot after two yields */
static unsigned    cp_sleepy_late;      /* set when the flag is late */

static void cp_sleep_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    cp_sleep_st = coro_rt_sleep(rt, coro_monotonic_ns() + 50000000ull);
    /* No yield: the coroutine ends here. Pre-fix the deadline is still in
       the heap, pointing at the node this death is about to free. */
}

static void cp_sleep_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_spawn(rt, NULL, cp_sleep_victim, rt) != CORO_OK) return;
    (void)coro_yield();   /* the victim runs: cancelled sleep, then death */
    (void)coro_yield();
}

static void cp_sleepy_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_cancel(coro_runtime_current()->current);
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 50000000ull);
    if (coro_rt_yield() != CORO_OK) return;
    ++cp_sleepy_resumes;
}

static void cp_sleepy_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_spawn(rt, NULL, cp_sleepy_victim, rt) != CORO_OK) return;
    (void)coro_yield();   /* the victim parks; the safe point cancels it */
    (void)coro_yield();   /* fixed: the victim's plain yield requeued it and
                             it has finished by now; unfixed: it is off the
                             queue, waiting for the deadline */
    cp_sleepy_early = cp_sleepy_resumes;
    /* (b) auxiliary: re-check well before the +50ms deadline. */
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 10000000ull);  /* ~10ms */
    if (cp_sleepy_resumes != 1u) cp_sleepy_late = 1;
    (void)coro_yield();   /* let the (possibly revived) victim finish */
}

/* ==========================================================================
 * C2 concurrency regressions (red-first, watchdog-bounded).
 *
 * Shared root cause (audit C2): a park's publication and a re-queue wakeup
 * have no node-level arbitration, so a cancellation racing the release paths
 * can duplicate a node inside a worker inbox (MPSC self-loop) or re-append a
 * still-linked node (waiter-chain self-loop); both spin inside the runtime
 * (drain_inbox / q_append) and hang wait_all, or free memory still queued
 * (UAF). Each case below races a pure-thread canceler against the release
 * paths. On the unfixed tree these are expected RED (hang / crash); with the
 * parking-delivery arbitration fix every case must complete bounded.
 *
 * Every case runs under an internal watchdog thread: when its budget expires
 * the process prints a frozen-state summary, writes a best-effort minidump
 * (Windows) and exits 124 -- a defect is an immediate FAIL, never a real
 * infinite hang. Budgets are generous multiples of the expected run time so
 * no assertion depends on wall-clock precision.
 * ========================================================================== */

#if defined(_WIN32)
static long c2_at_load(volatile long* p) {
    return InterlockedCompareExchange((volatile LONG*)p, 0, 0);
}
static void c2_at_store(volatile long* p, long v) {
    (void)InterlockedExchange((volatile LONG*)p, v);
}
static long c2_at_add(volatile long* p, long v) {
    return InterlockedExchangeAdd((volatile LONG*)p, v);
}
static uint64_t c2_at_load_u64(volatile uint64_t* p) {
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)p, 0, 0);
}
static void c2_at_store_u64(volatile uint64_t* p, uint64_t v) {
    (void)InterlockedExchange64((volatile LONG64*)p, (LONG64)v);
}
static void c2_thread_yield(void) { (void)SwitchToThread(); }
#else
#if defined(__clang__)
/* GNU/i686 (SysV) gives uint64_t 4-byte alignment; clang lowers these
   under-aligned 64-bit atomics to libatomic and flags -Watomic-alignment,
   which -Werror makes fatal. Silence only the advisory (same operation;
   mirrors src/arch/coro_atomic.h:32-43) -- GCC is unaffected. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif
static long c2_at_load(volatile long* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void c2_at_store(volatile long* p, long v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static long c2_at_add(volatile long* p, long v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}
static uint64_t c2_at_load_u64(volatile uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void c2_at_store_u64(volatile uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static void c2_thread_yield(void) { (void)sched_yield(); }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

static void c2_spin_us(unsigned us) {
    uint64_t t0 = coro_monotonic_ns();
    uint64_t d = (uint64_t)us * 1000ull;
    while (coro_monotonic_ns() - t0 < d) { /* calibrate against the clock */ }
}

#define C2_WATCHDOG_MS 4500u

static coro_rt_t*    c2_rt;
static pt_thread_t   c2_wd_thread;
static volatile long c2_wd_stop;
static int           c2_wd_live;
static uint64_t      c2_wd_deadline_ns;
static volatile long* c2_wd_progress;   /* NULL = fixed budget; else watched */
static uint64_t      c2_wd_stall_ns;
static uint64_t      c2_wd_cap_ns;
static const char*   c2_wd_case;
static void        (*c2_wd_dump)(void);
static pt_thread_t    c2_hardcap_thread;
static int            c2_hardcap_live;
static volatile uint64_t c2_hardcap_at_ns;  /* dependency-free exit deadline */

#if defined(_WIN32)
typedef BOOL (WINAPI* c2_mdw_fn)(HANDLE, DWORD, HANDLE, int,
                                 void*, void*, void*);

/* Best-effort only. On a corrupted runtime dbghelp can itself raise an
   access violation while walking the process (observed: the watchdog
   printed its dump, the minidump attempt then took the process down with
   0xC0000005 before _exit). SEH keeps the exit path deterministic: whatever
   the dump attempt does, the watchdog must still reach _exit(124). */
static void c2_wd_minidump_body(const char* name) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    union { FARPROC p; c2_mdw_fn fn; } u;
    const char* dir = getenv("C2_DUMP_DIR");
    char path[1024];
    HANDLE f;
    if (!dbg) return;
    u.p = GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!u.fn) { (void)FreeLibrary(dbg); return; }
    if (dir && *dir)
        (void)snprintf(path, sizeof path, "%s\\c2-wd-%s.dmp", dir, name);
    else
        (void)snprintf(path, sizeof path, "c2-wd-%s.dmp", name);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        (void)u.fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0,
                   NULL, NULL, NULL);
        (void)CloseHandle(f);
        fprintf(stderr, "[watchdog] minidump written: %s\n", path);
    }
    (void)FreeLibrary(dbg);
}

#if defined(_MSC_VER)
static void c2_wd_minidump(const char* name) {
    __try {
        c2_wd_minidump_body(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[watchdog] minidump attempt raised an exception; "
                        "continuing to exit\n");
    }
}
#else
static void c2_wd_minidump(const char* name) { c2_wd_minidump_body(name); }
#endif
#else
static void c2_wd_minidump(const char* name) { (void)name; }
#endif

static void* c2_watchdog_main(void* arg) {
    (void)arg;
    if (c2_wd_progress) {
        /* Bounded-progress watchdog (round-based cases): the case must keep
           advancing its progress counter; a run that merely needs more time
           is allowed up to the safety cap, a frozen counter fails after the
           stall window. This extends slow-but-healthy hosts without ever
           relaxing the stall detection the defect cases rely on. The two
           exits are reported DIFFERENTLY: a cap hit (still progressing, just
           slower than the budget) must never be read as a frozen stall --
           confusing them once hid a healthy run behind a "STALL" line. */
        uint64_t t0 = coro_monotonic_ns();
        uint64_t last_change = t0;
        long last = c2_at_load(c2_wd_progress);
        int cap_hit = 0;
        while (!c2_at_load(&c2_wd_stop)) {
            uint64_t now;
            long cur;
            sleep_ms(10);
            if (c2_at_load(&c2_wd_stop)) return NULL;
            cur = c2_at_load(c2_wd_progress);
            now = coro_monotonic_ns();
            if (cur != last) { last = cur; last_change = now; }
            if (now - last_change >= c2_wd_stall_ns) break;
            if (now - t0 >= c2_wd_cap_ns) { cap_hit = 1; break; }
        }
        if (c2_at_load(&c2_wd_stop)) return NULL;
        if (cap_hit)
            fprintf(stderr, "\n[watchdog] CAP case=%s safety cap=%llums "
                            "reached while still progressing (last=%ld)\n",
                    c2_wd_case, (unsigned long long)(c2_wd_cap_ns / 1000000ull),
                    last);
        else
            fprintf(stderr, "\n[watchdog] STALL case=%s last_progress=%ld "
                            "frozen_for>=%llums cap=%llums\n",
                    c2_wd_case, last,
                    (unsigned long long)(c2_wd_stall_ns / 1000000ull),
                    (unsigned long long)(c2_wd_cap_ns / 1000000ull));
    } else {
        while (!c2_at_load(&c2_wd_stop)) {
            if (coro_monotonic_ns() >= c2_wd_deadline_ns) break;
            sleep_ms(5);
        }
        if (c2_at_load(&c2_wd_stop)) return NULL;
        fprintf(stderr, "\n[watchdog] TIMEOUT case=%s budget=%lums\n",
                c2_wd_case, (long)C2_WATCHDOG_MS);
    }
    if (c2_wd_dump) c2_wd_dump();
    /* The minidump is best-effort evidence: bound it first, so a wedged
       dbghelp (or any stall on this path) cannot keep the process alive --
       the hard-cap killer terminates it a few seconds after the dump
       attempt begins. */
    c2_at_store_u64(&c2_hardcap_at_ns, coro_monotonic_ns() + 5000000000ull);
    c2_wd_minidump(c2_wd_case);
    fflush(stderr);
    _exit(124);
}

/* Dependency-free last resort: a separate always-runnable thread whose only
   job is to force-exit at the absolute hard deadline, no matter what state
   the rest of the process (heaps, locks, streams, dbghelp) is in. On POSIX
   an alarm() backs it up at the OS level (default SIGALRM action terminates
   the process even if no test thread ever runs again). */
static void* c2_hardcap_main(void* arg) {
    (void)arg;
    for (;;) {
        if (c2_at_load(&c2_wd_stop)) return NULL;
        if (coro_monotonic_ns() >= c2_at_load_u64(&c2_hardcap_at_ns)) break;
        sleep_ms(50);
    }
    fprintf(stderr, "\n[watchdog] HARDCAP case=%s: termination path stalled; "
                    "forcing exit\n", c2_wd_case);
    _exit(124);
}

static void c2_wd_begin_common(const char* case_name, void (*dump)(void)) {
    uint64_t now = coro_monotonic_ns();
    uint64_t limit = c2_wd_progress ? now + c2_wd_cap_ns : c2_wd_deadline_ns;
    uint64_t hard = limit + 15000000000ull;   /* +15s margin over the budget */
    c2_wd_case = case_name;
    c2_wd_dump = dump;
    c2_at_store(&c2_wd_stop, 0);
    c2_at_store_u64(&c2_hardcap_at_ns, hard);
    c2_hardcap_live = (pt_thread_create(&c2_hardcap_thread, c2_hardcap_main,
                                        NULL) == 0);
    CHECK(c2_hardcap_live);
#if !defined(_WIN32)
    (void)alarm((unsigned)((hard - now + 999999999ull) / 1000000000ull));
#endif
    c2_wd_live = (pt_thread_create(&c2_wd_thread, c2_watchdog_main, NULL) == 0);
    CHECK(c2_wd_live);
}

static void c2_wd_begin(const char* case_name, void (*dump)(void)) {
    c2_wd_progress = NULL;
    c2_wd_deadline_ns = coro_monotonic_ns() + (uint64_t)C2_WATCHDOG_MS * 1000000ull;
    c2_wd_begin_common(case_name, dump);
}

static void c2_wd_begin_progress(const char* case_name, void (*dump)(void),
                                 volatile long* progress,
                                 unsigned stall_ms, unsigned cap_ms) {
    c2_wd_progress = progress;
    c2_wd_stall_ns = (uint64_t)stall_ms * 1000000ull;
    c2_wd_cap_ns = (uint64_t)cap_ms * 1000000ull;
    c2_wd_begin_common(case_name, dump);
}

static void c2_wd_end(void) {
    c2_at_store(&c2_wd_stop, 1);
    if (c2_wd_live) {
        (void)pt_thread_join(c2_wd_thread);
        c2_wd_live = 0;
    }
    if (c2_hardcap_live) {
        (void)pt_thread_join(c2_hardcap_thread);
        c2_hardcap_live = 0;
    }
#if !defined(_WIN32)
    (void)alarm(0);
#endif
}

/* Failure with actual values on stderr (harness CHECK prints the expression
   text only; these keep the numbers). */
#define C2_FAIL_MSG(...) do { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    g_failures++; \
} while (0)

#define C2_CHECK_MSG(cond, ...) do { \
    if (!(cond)) C2_FAIL_MSG(__VA_ARGS__); \
} while (0)

/* Case filter for evidence collection: C2_CASES="r1,r4" runs only those.
   Unset (the default) runs every case. Tags match whole comma-separated
   items, so "sv" and "svh" stay distinct. */
static int c2_case_selected(const char* tag) {
    const char* sel = getenv("C2_CASES");
    size_t n;
    const char* p;
    if (!sel || !*sel) return 1;
    n = strlen(tag);
    p = sel;
    while ((p = strstr(p, tag)) != NULL) {
        char before = (p == sel) ? ',' : p[-1];
        char after = p[n];
        if ((before == ',' || before == ' ') &&
            (after == ',' || after == ' ' || after == '\0'))
            return 1;
        p += n;
    }
    return 0;
}

/* ---------------- R1: mutex canceler storm ---------------- */

#define C2_WORKERS    6u
#define C2_R1_VICTIMS 16u
#define C2_R1_HOLDERS 2u

static coro_rt_mutex_t c2_r1_m;
static volatile long   c2_r1_stop;
static volatile long   c2_r1_alive;
static volatile long   c2_r1_cancel_run;
static volatile long   c2_r1_cancel_calls;
static coro_t*         c2_r1_vh[C2_R1_VICTIMS];

static void c2_r1_holder(void* pv) {
    unsigned long i = 0;
    (void)pv;
    while (!c2_at_load(&c2_r1_stop)) {
        if (coro_rt_mutex_lock(c2_rt, &c2_r1_m) != CORO_OK) return;
        for (volatile int j = 0; j < 120; ++j) { }
        if (coro_rt_mutex_unlock(c2_rt, &c2_r1_m) != CORO_OK) return;
        if ((++i & 0x3fu) == 0) (void)coro_rt_yield();
    }
}

static void c2_r1_victim(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_r1_stop)) {
        coro_status st = coro_rt_mutex_lock(c2_rt, &c2_r1_m);
        if (st != CORO_OK) break;   /* the cancellation consumed at the park */
        if (coro_rt_mutex_unlock(c2_rt, &c2_r1_m) != CORO_OK) break;
    }
    (void)c2_at_add(&c2_r1_alive, -1);
}

static void* c2_r1_canceler(void* pv) {
    (void)pv;
    while (c2_at_load(&c2_r1_cancel_run)) {
        for (unsigned i = 0; i < C2_R1_VICTIMS; ++i)
            if (coro_rt_cancel(c2_rt, c2_r1_vh[i]) == CORO_OK)
                (void)c2_at_add(&c2_r1_cancel_calls, 1);
        c2_thread_yield();
    }
    return NULL;
}

static void c2_r1_dump(void) {
    fprintf(stderr, "[watchdog] R1 pending=%zu alive=%ld stop=%ld "
                    "canceler_run=%ld cancel_calls=%ld\n",
            coro_rt_pending(c2_rt), c2_at_load(&c2_r1_alive),
            c2_at_load(&c2_r1_stop), c2_at_load(&c2_r1_cancel_run),
            c2_at_load(&c2_r1_cancel_calls));
    fprintf(stderr, "[watchdog] R1 mutex locked=%llu waitq=%p\n",
            (unsigned long long)c2_at_load_u64((volatile uint64_t*)&c2_r1_m.locked),
            c2_r1_m.waitq);
}

static void c2_case_r1(void) {
    pt_thread_t canc;
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    c2_wd_begin("r1", c2_r1_dump);

    coro_rt_mutex_init(&c2_r1_m);
    c2_at_store(&c2_r1_stop, 0);
    c2_at_store(&c2_r1_alive, (long)C2_R1_VICTIMS);
    c2_at_store(&c2_r1_cancel_calls, 0);
    for (unsigned i = 0; i < C2_R1_VICTIMS; ++i) {
        c2_r1_vh[i] = NULL;
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r1_victim, NULL,
                             &c2_r1_vh[i]) == CORO_OK);
    }
    for (unsigned i = 0; i < C2_R1_HOLDERS; ++i)
        CHECK(coro_rt_spawn(c2_rt, NULL, c2_r1_holder, NULL) == CORO_OK);
    sleep_ms(2);

    c2_at_store(&c2_r1_cancel_run, 1);
    CHECK(pt_thread_create(&canc, c2_r1_canceler, NULL) == 0);
    sleep_ms(4);
    c2_at_store(&c2_r1_cancel_run, 0);
    (void)pt_thread_join(canc);

    c2_at_store(&c2_r1_stop, 1);
    CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
    C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                 "R1 bounded wind-down: pending=%zu", coro_rt_pending(c2_rt));
    C2_CHECK_MSG(c2_at_load(&c2_r1_alive) == 0,
                 "R1 alive=%ld", c2_at_load(&c2_r1_alive));
    C2_CHECK_MSG(coro_rt_mutex_trylock(c2_rt, &c2_r1_m) == CORO_OK,
                 "R1 mutex not free: locked=%llu",
                 (unsigned long long)c2_at_load_u64(
                     (volatile uint64_t*)&c2_r1_m.locked));
    for (unsigned i = 0; i < C2_R1_VICTIMS; ++i)
        C2_CHECK_MSG(coro_rt_collect(c2_rt, c2_r1_vh[i]) == CORO_OK,
                     "R1 collect(%u) failed", i);

    c2_wd_end();
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

/* ---------------- R2: sem canceler storm + token conservation ------------ */

#define C2_R2_CONSUMERS 8u
#define C2_R2_POSTS     8000l
#define C2_R2_ROUNDS    16u

/* (a) targeted phase: one victim parks on a fresh sem(0); two host threads
   leave a barrier together, one cancelling the victim and one posting a
   token -- the cancel-vs-post race at its tightest point. */
#define C2_R2T_ROUNDS 2000u

static coro_rt_sem_t* c2_r2t_s;
static coro_t*        c2_r2t_vh;
static volatile long  c2_r2t_status;
static long           c2_r2t_round;
static pt_barrier_t   c2_r2t_bars[C2_R2T_ROUNDS];

static void c2_r2t_victim(void* pv) {
    (void)pv;
    coro_status st = coro_rt_sem_wait(c2_rt, c2_r2t_s);
    c2_at_store(&c2_r2t_status, (long)st);
}

static void* c2_r2t_cancel(void* pv) {
    pt_barrier_t* b = (pt_barrier_t*)pv;
    (void)pt_barrier_wait(b);
    (void)coro_rt_cancel(c2_rt, c2_r2t_vh);
    return NULL;
}

static void* c2_r2t_post(void* pv) {
    pt_barrier_t* b = (pt_barrier_t*)pv;
    (void)pt_barrier_wait(b);
    (void)coro_rt_sem_post(c2_rt, c2_r2t_s);
    return NULL;
}

static void c2_r2_targeted(void) {
    uint64_t start = coro_monotonic_ns();
    for (unsigned r = 0; r < C2_R2T_ROUNDS; ++r) {
        coro_rt_sem_t s;
        pt_thread_t tc, tp;
        uint64_t t0;
        /* Machine-speed guard (audit-B' budget note): the race lands within
           the first few hundred rounds on every host measured; a slow host
           stops early instead of running the per-case watchdog out. */
        if (r >= 100 && coro_monotonic_ns() - start > 2000000000ull) break;
        c2_r2t_round = (long)r;
        coro_rt_sem_init(&s, 0);
        c2_r2t_s = &s;
        c2_r2t_vh = NULL;
        c2_at_store(&c2_r2t_status, (long)CORO_OK);
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r2t_victim, NULL,
                              &c2_r2t_vh) == CORO_OK);
        c2_spin_us(120);   /* let the victim park on the semaphore */
        CHECK(pt_barrier_init(&c2_r2t_bars[r], 3) == 0);
        CHECK(pt_thread_create(&tc, c2_r2t_cancel, &c2_r2t_bars[r]) == 0);
        CHECK(pt_thread_create(&tp, c2_r2t_post, &c2_r2t_bars[r]) == 0);
        (void)pt_barrier_wait(&c2_r2t_bars[r]);
        (void)pt_thread_join(tc);
        (void)pt_thread_join(tp);
        t0 = coro_monotonic_ns();
        for (;;) {
            if (coro_rt_collect(c2_rt, c2_r2t_vh) == CORO_OK) break;
            if (coro_monotonic_ns() - t0 > 1000000000ull) {
                C2_FAIL_MSG("R2(targeted) round=%ld collect never succeeded "
                            "(pending=%zu status=%ld)",
                            (long)r, coro_rt_pending(c2_rt),
                            c2_at_load(&c2_r2t_status));
                return;
            }
            c2_thread_yield();
        }
    }
}

static coro_rt_sem_t c2_r2_s;
static volatile long c2_r2_stop;
static volatile long c2_r2_consumed;
static coro_t*       c2_r2_ch[C2_R2_CONSUMERS];
static unsigned      c2_r2_nc;
static long          c2_r2_rounds;
static long          c2_r2_extras;
static pt_barrier_t  c2_r2_bars[C2_R2_ROUNDS];

static void c2_r2_consumer(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_r2_stop)) {
        if (coro_rt_sem_wait(c2_rt, &c2_r2_s) != CORO_OK) break;
        (void)c2_at_add(&c2_r2_consumed, 1);
    }
}

static void c2_r2_producer(void* pv) {
    long n = (long)(intptr_t)pv;
    for (long i = 0; i < n; ++i) {
        if (coro_rt_sem_post(c2_rt, &c2_r2_s) != CORO_OK) return;
        if ((i & 0x3fl) == 0) (void)coro_rt_yield();
    }
}

static void* c2_r2_cancel_all(void* pv) {
    pt_barrier_t* b = (pt_barrier_t*)pv;
    (void)pt_barrier_wait(b);
    for (unsigned i = 0; i < c2_r2_nc; ++i)
        (void)coro_rt_cancel(c2_rt, c2_r2_ch[i]);
    return NULL;
}

static void* c2_r2_post_all(void* pv) {
    pt_barrier_t* b = (pt_barrier_t*)pv;
    (void)pt_barrier_wait(b);
    for (unsigned i = 0; i < c2_r2_nc; ++i)
        (void)coro_rt_sem_post(c2_rt, &c2_r2_s);
    return NULL;
}

static void c2_r2_dump(void) {
    fprintf(stderr, "[watchdog] R2 pending=%zu consumed=%ld sem_count=%zu "
                    "stop=%ld rounds=%ld extras=%ld posted=%ld "
                    "targeted_round=%ld targeted_status=%ld\n",
            coro_rt_pending(c2_rt), c2_at_load(&c2_r2_consumed),
            coro_rt_sem_count(&c2_r2_s), c2_at_load(&c2_r2_stop),
            c2_r2_rounds, c2_r2_extras, (long)C2_R2_POSTS,
            c2_r2t_round, c2_at_load(&c2_r2t_status));
}

static void c2_case_r2(void) {
    unsigned long long left;
    long want;
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    c2_wd_begin("r2", c2_r2_dump);

    c2_r2_targeted();   /* (a) the targeted cancel-vs-post race */

    coro_rt_sem_init(&c2_r2_s, 0);
    c2_at_store(&c2_r2_stop, 0);
    c2_at_store(&c2_r2_consumed, 0);
    c2_r2_nc = 0;
    for (unsigned i = 0; i < C2_R2_CONSUMERS; ++i) {
        c2_r2_ch[i] = NULL;
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r2_consumer, NULL,
                              &c2_r2_ch[i]) == CORO_OK);
        ++c2_r2_nc;
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(coro_rt_spawn(c2_rt, NULL, c2_r2_producer,
                            (void*)(intptr_t)(C2_R2_POSTS / 2)) == CORO_OK);
    sleep_ms(3);
    c2_at_store(&c2_r2_stop, 1);

    c2_r2_rounds = 0;
    c2_r2_extras = 0;
    for (unsigned r = 0; r < C2_R2_ROUNDS; ++r) {
        pt_thread_t tc, tp;
        if (coro_rt_pending(c2_rt) == 0) break;
        c2_r2_rounds = (long)r + 1;
        CHECK(pt_barrier_init(&c2_r2_bars[r], 3) == 0);
        CHECK(pt_thread_create(&tc, c2_r2_cancel_all, &c2_r2_bars[r]) == 0);
        CHECK(pt_thread_create(&tp, c2_r2_post_all, &c2_r2_bars[r]) == 0);
        (void)pt_barrier_wait(&c2_r2_bars[r]);
        (void)pt_thread_join(tc);
        (void)pt_thread_join(tp);
        c2_r2_extras += (long)c2_r2_nc;
        sleep_ms(1);
    }
    C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                 "R2 bounded drain: pending=%zu after %ld rounds extras=%ld",
                 coro_rt_pending(c2_rt), c2_r2_rounds, c2_r2_extras);
    CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
    for (unsigned i = 0; i < c2_r2_nc; ++i)
        C2_CHECK_MSG(coro_rt_collect(c2_rt, c2_r2_ch[i]) == CORO_OK,
                     "R2 collect(%u) failed", i);
    left = (unsigned long long)coro_rt_sem_count(&c2_r2_s);
    want = C2_R2_POSTS + c2_r2_extras;
    C2_CHECK_MSG((long long)c2_at_load(&c2_r2_consumed) + (long long)left ==
                     (long long)want,
                 "R2 token conservation: consumed=%ld left=%llu posted=%ld "
                 "extras=%ld", c2_at_load(&c2_r2_consumed), left,
                 (long)C2_R2_POSTS, c2_r2_extras);

    c2_wd_end();
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

/* ---------------- R3: cond storm + continuous canceler ------------------- */

#define C2_R3_VICTIMS 8u
#define C2_R3_ROUNDS  20l

static coro_rt_cond_t  c2_r3_c;
static coro_rt_mutex_t c2_r3_m;
static volatile long   c2_r3_stop;
static volatile long   c2_r3_alive;
static volatile long   c2_r3_cancel_run;
static volatile long   c2_r3_cancel_calls;
static coro_t*         c2_r3_vh[C2_R3_VICTIMS];
static unsigned        c2_r3_nv;
static long            c2_r3_cycle;

static void c2_r3_victim(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_r3_stop)) {
        if (coro_rt_mutex_lock(c2_rt, &c2_r3_m) != CORO_OK) break;
        (void)coro_rt_cond_wait(c2_rt, &c2_r3_c, &c2_r3_m);
        if (coro_rt_mutex_unlock(c2_rt, &c2_r3_m) != CORO_OK) break;
    }
    (void)c2_at_add(&c2_r3_alive, -1);
}

static void c2_r3_notifier(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_r3_stop)) {
        if (coro_rt_mutex_lock(c2_rt, &c2_r3_m) != CORO_OK) return;
        (void)coro_rt_cond_notify(c2_rt, &c2_r3_c);
        if (coro_rt_mutex_unlock(c2_rt, &c2_r3_m) != CORO_OK) return;
        (void)coro_rt_yield();
    }
}

static void* c2_r3_canceler(void* pv) {
    (void)pv;
    while (c2_at_load(&c2_r3_cancel_run)) {
        for (unsigned i = 0; i < c2_r3_nv; ++i)
            if (coro_rt_cancel(c2_rt, c2_r3_vh[i]) == CORO_OK)
                (void)c2_at_add(&c2_r3_cancel_calls, 1);
        c2_thread_yield();
    }
    return NULL;
}

static void c2_r3_dump(void) {
    fprintf(stderr, "[watchdog] R3 cycle=%ld pending=%zu alive=%ld stop=%ld "
                    "canceler_run=%ld cancel_calls=%ld\n",
            c2_r3_cycle, coro_rt_pending(c2_rt), c2_at_load(&c2_r3_alive),
            c2_at_load(&c2_r3_stop), c2_at_load(&c2_r3_cancel_run),
            c2_at_load(&c2_r3_cancel_calls));
}

static void c2_case_r3(void) {
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    c2_wd_begin("r3", c2_r3_dump);

    for (long c = 0; c < C2_R3_ROUNDS; ++c) {
        pt_thread_t canc;
        long r;
        c2_r3_cycle = c;
        coro_rt_cond_init(&c2_r3_c);
        coro_rt_mutex_init(&c2_r3_m);
        c2_at_store(&c2_r3_stop, 0);
        c2_at_store(&c2_r3_alive, (long)C2_R3_VICTIMS);
        c2_at_store(&c2_r3_cancel_calls, 0);
        c2_r3_nv = 0;
        for (unsigned i = 0; i < C2_R3_VICTIMS; ++i) {
            c2_r3_vh[i] = NULL;
            CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r3_victim, NULL,
                                  &c2_r3_vh[i]) == CORO_OK);
            ++c2_r3_nv;
        }
        CHECK(coro_rt_spawn(c2_rt, NULL, c2_r3_notifier, NULL) == CORO_OK);
        sleep_ms(1);

        c2_at_store(&c2_r3_cancel_run, 1);
        CHECK(pt_thread_create(&canc, c2_r3_canceler, NULL) == 0);
        sleep_ms(4);
        c2_at_store(&c2_r3_cancel_run, 0);
        (void)pt_thread_join(canc);

        c2_at_store(&c2_r3_stop, 1);
        (void)coro_rt_cond_broadcast(c2_rt, &c2_r3_c);
        for (r = 0; r < 20000 && coro_rt_pending(c2_rt) > 0; ++r) {
            (void)coro_rt_cond_notify(c2_rt, &c2_r3_c);
            if ((r & 0x3fl) == 0) sleep_ms(1);
            else c2_thread_yield();
        }
        C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                     "R3 cycle=%ld bounded exit: pending=%zu alive=%ld "
                     "cancel_calls=%ld",
                     c, coro_rt_pending(c2_rt), c2_at_load(&c2_r3_alive),
                     c2_at_load(&c2_r3_cancel_calls));
        if (coro_rt_pending(c2_rt) != 0) break;
        CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
        for (unsigned i = 0; i < c2_r3_nv; ++i)
            C2_CHECK_MSG(coro_rt_collect(c2_rt, c2_r3_vh[i]) == CORO_OK,
                         "R3 cycle=%ld collect(%u) failed", c, i);
        C2_CHECK_MSG(c2_at_load(&c2_r3_alive) == 0,
                     "R3 cycle=%ld alive=%ld", c, c2_at_load(&c2_r3_alive));
    }

    c2_wd_end();
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

/* ---------------- R4: join cancel vs retire (walk wake) ------------------ */

/* 512 rounds proved flaky on the unfixed tree (a WSL run completed green:
   the cancel-vs-retire window per round is narrow). 2048 keeps the red
   probability high while the progress watchdog covers slow hosts. */
#define C2_R4_ROUNDS  2048l
#define C2_R4_JOINERS 4u

static volatile long c2_r4_done;
static long          c2_r4_round;

static void c2_r4_target(void* pv) {
    (void)pv;
    (void)coro_rt_sleep(c2_rt, coro_monotonic_ns() + 300000ull);
}

static void c2_r4_joiner(void* pv) {
    coro_t* t = (coro_t*)pv;
    (void)coro_rt_join(c2_rt, t);
    (void)c2_at_add(&c2_r4_done, 1);
}

static void c2_r4_dump(void) {
    fprintf(stderr, "[watchdog] R4 round=%ld joiners_done=%ld pending=%zu\n",
            c2_r4_round, c2_at_load(&c2_r4_done), coro_rt_pending(c2_rt));
}

static void c2_case_r4(void) {
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    /* Round-based case on the audit-B' budget note: the fixed 4.5s budget
       false-failed a slow host that was still completing rounds (pending=0,
       joiners_done climbing). The input for the defect is the race in the
       first rounds, so the bound is progress, not wall clock: a frozen
       joiners_done counter fails after a 2s stall window; a healthy run may
       take up to the 30s safety cap. Stall detection is as strict as before
       (the current tree freezes round 0/1 with joiners_done stuck). */
    c2_wd_begin_progress("r4", c2_r4_dump, &c2_r4_done, 2000u, 60000u);

    c2_at_store(&c2_r4_done, 0);
    for (long c = 0; c < C2_R4_ROUNDS; ++c) {
        coro_t* t = NULL;
        coro_t* js[C2_R4_JOINERS];
        c2_r4_round = c;
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r4_target, NULL, &t) == CORO_OK);
        for (unsigned i = 0; i < C2_R4_JOINERS; ++i) {
            js[i] = NULL;
            CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r4_joiner, t,
                                  &js[i]) == CORO_OK);
        }
        /* Busy-wait INSIDE the target's 300us lifetime: the joiners park and
           the cancels then race the retire. A millisecond sleep overshot the
           deadline on fast hosts (target already dead -> no race at all ->
           the case went flaky-green on WSL). */
        c2_spin_us(150);
        for (unsigned i = 0; i < C2_R4_JOINERS; ++i)
            (void)coro_rt_cancel(c2_rt, js[i]);
        CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
        C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                     "R4 round=%ld pending=%zu", c, coro_rt_pending(c2_rt));
        if (coro_rt_pending(c2_rt) != 0) break;
        C2_CHECK_MSG(coro_rt_collect(c2_rt, t) == CORO_OK,
                     "R4 round=%ld collect(target) failed", c);
        for (unsigned i = 0; i < C2_R4_JOINERS; ++i)
            C2_CHECK_MSG(coro_rt_collect(c2_rt, js[i]) == CORO_OK,
                         "R4 round=%ld collect(joiner %u) failed", c, i);
    }
    C2_CHECK_MSG(c2_at_load(&c2_r4_done) ==
                     (long)C2_R4_ROUNDS * (long)C2_R4_JOINERS,
                 "R4 joiners_done=%ld expected=%ld", c2_at_load(&c2_r4_done),
                 (long)C2_R4_ROUNDS * (long)C2_R4_JOINERS);

    c2_wd_end();
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

/* ---------------- R5: external cancel of a sleep (far deadline) ---------- */

#define C2_R5_ROUNDS 12l

static uint64_t      c2_r5_deadline;
static volatile long c2_r5_status;
static long          c2_r5_round;

static void c2_r5_victim(void* pv) {
    (void)pv;
    coro_status st = coro_rt_sleep(c2_rt, c2_r5_deadline);
    c2_at_store(&c2_r5_status, (long)st);
}

static void c2_r5_dump(void) {
    fprintf(stderr, "[watchdog] R5 round=%ld status=%ld pending=%zu "
                    "deadline_delta_ns=%lld\n",
            c2_r5_round, c2_at_load(&c2_r5_status), coro_rt_pending(c2_rt),
            (long long)(c2_r5_deadline - coro_monotonic_ns()));
}

static void c2_case_r5(void) {
    long c;
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    c2_wd_begin("r5", c2_r5_dump);

    /* (a) far deadline: the cancellation must win, well before the 5s. */
    for (c = 0; c < C2_R5_ROUNDS; ++c) {
        coro_t* v = NULL;
        uint64_t t0;
        long t;
        c2_r5_round = c;
        c2_r5_deadline = coro_monotonic_ns() + 5000000000ull;
        c2_at_store(&c2_r5_status, (long)CORO_OK);
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r5_victim, NULL, &v) == CORO_OK);
        sleep_ms(1);   /* let the victim park on the deadline */
        t0 = coro_monotonic_ns();
        CHECK(coro_rt_cancel(c2_rt, v) == CORO_OK);
        for (t = 0; t < 5000; ++t) {
            if (coro_rt_collect(c2_rt, v) == CORO_OK) break;
            sleep_ms(1);
        }
        C2_CHECK_MSG(t < 5000, "R5 round=%ld collect never succeeded "
                               "(pending=%zu)", c, coro_rt_pending(c2_rt));
        C2_CHECK_MSG(c2_at_load(&c2_r5_status) == (long)CORO_ECANCELED,
                     "R5 round=%ld status=%ld (want ECANCELED=%ld)", c,
                     c2_at_load(&c2_r5_status), (long)CORO_ECANCELED);
        C2_CHECK_MSG(coro_monotonic_ns() - t0 < 1000000000ull,
                     "R5 round=%ld cancel-to-collect=%llu ns while the "
                     "deadline was 5s away", c,
                     (unsigned long long)(coro_monotonic_ns() - t0));
        if (t >= 5000) break;
    }

    /* (b) deadline stripping: cancel early on a short deadline and then run
       past it -- a stranded heap entry would fire at +150ms and drive the
       already-freed node (UAF / second drive). */
    {
        coro_t* v = NULL;
        long t;
        c2_r5_round = -1;
        c2_r5_deadline = coro_monotonic_ns() + 150000000ull;  /* 150ms */
        c2_at_store(&c2_r5_status, (long)CORO_OK);
        CHECK(coro_rt_spawn_h(c2_rt, NULL, c2_r5_victim, NULL, &v) == CORO_OK);
        sleep_ms(1);
        CHECK(coro_rt_cancel(c2_rt, v) == CORO_OK);
        for (t = 0; t < 5000; ++t) {
            if (coro_rt_collect(c2_rt, v) == CORO_OK) break;
            sleep_ms(1);
        }
        C2_CHECK_MSG(t < 5000, "R5 short-deadline collect never succeeded");
        C2_CHECK_MSG(c2_at_load(&c2_r5_status) == (long)CORO_ECANCELED,
                     "R5 short status=%ld", c2_at_load(&c2_r5_status));
        sleep_ms(300);   /* the abandoned deadline lies in this window */
        C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                     "R5 stale-deadline drive: pending=%zu after the deadline "
                     "passed", coro_rt_pending(c2_rt));
    }

    c2_wd_end();
    CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

/* ---------------- survivor: mutex storm, victims retry after ECANCELED --- */

#define C2_SV_VICTIMS 8u
/* 300 is the default per the joint A'/B' criterion review (B' red-team):
   the same shape at 40 cycles runs GREEN on the BT-L tree while the 300
   cycle form still wedges (residual BT-design §5 open), so 40-cycle green
   must NOT be taken as a pass. C2_SV_ROUNDS=<n> keeps the 40-cycle fast
   smoke form for quick runs. */
#define C2_SV_ROUNDS  300l

static coro_rt_mutex_t c2_sv_m;
static volatile long   c2_sv_stop;
static volatile long   c2_sv_alive;
static volatile long   c2_sv_cancel_run;
static volatile long   c2_sv_cancel_calls;
static volatile long   c2_sv_progress;   /* exits + completed cycles */
static coro_t*         c2_sv_vh[C2_SV_VICTIMS];
static void*           c2_sv_node[C2_SV_VICTIMS];   /* rt_node* at spawn */
static int             c2_sv_gate_mode;  /* extended walk only for the gate */
static volatile uint64_t c2_sv_t0_ns;    /* case start (timing markers) */
static volatile uint64_t c2_sv_stop_ns;
static volatile uint64_t c2_sv_cancel_last_ns;
static volatile uint64_t c2_sv_exit_last_ns;
static long            c2_sv_cycle;
static long            c2_sv_rounds_total;

static long long c2_sv_ms_since(uint64_t t_ns) {
    if (!t_ns) return -1;
    return (long long)((coro_monotonic_ns() - t_ns) / 1000000ull);
}

static const char* c2_sv_owner(const rt_node* n) {
    static char buf[24];
    for (unsigned i = 0; i < C2_SV_VICTIMS; ++i)
        if (c2_sv_node[i] && c2_sv_node[i] == (const void*)n) {
            (void)snprintf(buf, sizeof buf, "victim#%u", i);
            return buf;
        }
    return "holder/other";
}

/* audit-A' spec: walk the parked chain and every worker so a frozen SV
   sample opens up protocol-chain vs lost-delivery vs wind-down directly.
   Best-effort under corruption: bounded walk; MSVC guards it with SEH so a
   bad read cannot keep the watchdog from exiting. */
static void c2_sv_walk(void) {
    const rt_node* n = (const rt_node*)c2_sv_m.waitq;
    const rt_node* head0 = n;
    unsigned k = 0;
    fprintf(stderr, "[watchdog] SV walk waitq=%p:\n", c2_sv_m.waitq);
    while (n && k < 64) {
        uint64_t f = c2_at_load_u64((volatile uint64_t*)&n->flags);
        fprintf(stderr,
                "[watchdog]   q[%u] node=%p own=%s flags=0x%llx%s%s%s%s%s%s "
                "worker=%p wnext=%p\n",
                k, (const void*)n, c2_sv_owner(n), (unsigned long long)f,
                (f & RT_STARTED) ? " STARTED" : "",
                (f & RT_WAITING) ? " WAITING" : "",
                (f & RT_RETAINED) ? " RETAINED" : "",
                (f & RT_DONE) ? " DONE" : "",
                (f & RT_WAKE_REQ) ? " WAKE_REQ" : "",
                (f & RT_PARKED) ? " PARKED" : "",
                (const void*)n->worker, (const void*)n->waiter_next);
        if (n->waiter_next == n) {
            fprintf(stderr, "[watchdog]   q chain SELF-LOOP at q[%u]\n", k);
            break;
        }
        if (n->waiter_next == head0) {
            fprintf(stderr, "[watchdog]   q chain cycles back to head after "
                            "q[%u]\n", k);
            break;
        }
        n = n->waiter_next;
        ++k;
    }
    if (k == 64)
        fprintf(stderr, "[watchdog]   q walk truncated at 64 (cycle?)\n");
    if (c2_rt) {
        size_t wi;
        for (wi = 0; wi < c2_rt->nworkers; ++wi) {
            const rt_worker* w = &c2_rt->workers[wi];
            const rt_node* r = w->running;
            const rt_node* m = w->inbox.head;
            unsigned ic = 0;
            while (m && ic < 64) {
                const rt_node* nx = m->next;
                ++ic;
                if (nx == m) break;   /* self-loop: stop counting */
                m = nx;
            }
            fprintf(stderr,
                    "[watchdog]   w%zu running=%p(%s) inbox(head=%p n=%u%s) "
                    "deque(top=%llu bottom=%llu n=%llu)\n",
                    wi, (const void*)r, r ? c2_sv_owner(r) : "-",
                    (const void*)w->inbox.head, ic, w->inbox.head ? "" : " empty",
                    (unsigned long long)w->queue.top,
                    (unsigned long long)w->queue.bottom,
                    (unsigned long long)(w->queue.bottom - w->queue.top));
        }
    }
}

static void c2_sv_walk_guarded(void) {
#if defined(_MSC_VER)
    __try {
        c2_sv_walk();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[watchdog] SV walk raised an exception (corrupted "
                        "chain/state); walk aborted\n");
    }
#else
    c2_sv_walk();
#endif
}

static long c2_sv_rounds_cfg(void) {
    const char* s = getenv("C2_SV_ROUNDS");
    long v = (s && *s) ? atol(s) : (long)C2_SV_ROUNDS;
    if (v < 1) v = 1;
    if (v > 100000) v = 100000;
    return v;
}

/* Strict gate form (B'/parent ruling: the STAMPING criterion): the victim
   exits on stop with NO extra handoff pass -- the chain must be woken by the
   release paths themselves; the audit red team traced the collapse-trust
   residual here (a final unlock that never re-wakes the parked holder). */
static void c2_sv_victim(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_sv_stop)) {
        coro_status st = coro_rt_mutex_lock(c2_rt, &c2_sv_m);
        if (st == CORO_OK) {
            if (coro_rt_mutex_unlock(c2_rt, &c2_sv_m) != CORO_OK) break;
        } else if (st == CORO_ECANCELED) {
            continue;   /* survivor: keep contending after the cancellation */
        } else {
            break;
        }
    }
    c2_at_store_u64(&c2_sv_exit_last_ns, coro_monotonic_ns());
    (void)c2_at_add(&c2_sv_alive, -1);
    (void)c2_at_add(&c2_sv_progress, 1);
}

/* Supplementary regression form (audit-A' wind-down handoff): before exit
   the victim passes the handoff once (trylock/unlock). Per the joint ruling
   this MASKS the collapse-trust residual, so it is kept alongside the gate
   only as a supplementary case, never as the pass criterion. */
static void c2_sv_victim_h(void* pv) {
    (void)pv;
    while (!c2_at_load(&c2_sv_stop)) {
        coro_status st = coro_rt_mutex_lock(c2_rt, &c2_sv_m);
        if (st == CORO_OK) {
            if (coro_rt_mutex_unlock(c2_rt, &c2_sv_m) != CORO_OK) break;
        } else if (st == CORO_ECANCELED) {
            continue;   /* survivor: keep contending after the cancellation */
        } else {
            break;
        }
    }
    if (c2_at_load(&c2_sv_stop)) {
        if (coro_rt_mutex_trylock(c2_rt, &c2_sv_m) == CORO_OK)
            (void)coro_rt_mutex_unlock(c2_rt, &c2_sv_m);
    }
    c2_at_store_u64(&c2_sv_exit_last_ns, coro_monotonic_ns());
    (void)c2_at_add(&c2_sv_alive, -1);
    (void)c2_at_add(&c2_sv_progress, 1);
}

/* The holder unlocks until every survivor has exited, so a victim parked at
   wind-down is always handed the lock (the S3 shape). */
static void c2_sv_holder(void* pv) {
    unsigned long i = 0;
    (void)pv;
    while (c2_at_load(&c2_sv_alive) > 0) {
        if (coro_rt_mutex_lock(c2_rt, &c2_sv_m) != CORO_OK) return;
        for (volatile int j = 0; j < 120; ++j) { }
        if (coro_rt_mutex_unlock(c2_rt, &c2_sv_m) != CORO_OK) return;
        if ((++i & 0x3fu) == 0) (void)coro_rt_yield();
    }
}

static void* c2_sv_canceler(void* pv) {
    (void)pv;
    while (c2_at_load(&c2_sv_cancel_run)) {
        for (unsigned i = 0; i < C2_SV_VICTIMS; ++i)
            if (coro_rt_cancel(c2_rt, c2_sv_vh[i]) == CORO_OK)
                (void)c2_at_add(&c2_sv_cancel_calls, 1);
        c2_at_store_u64(&c2_sv_cancel_last_ns, coro_monotonic_ns());
        c2_thread_yield();
    }
    return NULL;
}

static const char* c2_sv_form = "sv";

static void c2_sv_dump(void) {
    fprintf(stderr, "[watchdog] SV form=%s cycle=%ld/%ld progress=%ld "
                    "pending=%zu alive=%ld stop=%ld canceler_run=%ld "
                    "cancel_calls=%ld\n",
            c2_sv_form, c2_sv_cycle, c2_sv_rounds_total,
            c2_at_load(&c2_sv_progress), coro_rt_pending(c2_rt),
            c2_at_load(&c2_sv_alive), c2_at_load(&c2_sv_stop),
            c2_at_load(&c2_sv_cancel_run), c2_at_load(&c2_sv_cancel_calls));
    fprintf(stderr, "[watchdog] SV mutex locked=%llu waitq=%p\n",
            (unsigned long long)c2_at_load_u64((volatile uint64_t*)&c2_sv_m.locked),
            c2_sv_m.waitq);
    fprintf(stderr, "[watchdog] SV timing ms since start: now=%lld stop=%lld "
                    "canceler_last=%lld last_exit=%lld\n",
            c2_sv_ms_since(c2_sv_t0_ns), c2_sv_ms_since(c2_sv_stop_ns),
            c2_sv_ms_since(c2_sv_cancel_last_ns),
            c2_sv_ms_since(c2_sv_exit_last_ns));
#ifdef COROCC_WAKE_DBG
    /* Diagnostic only (compiled under -DCOROCC_WAKE_DBG; the default build is
       byte-identical to before). Pairs with the wake/park invariant D1: a
       frozen dump whose wake_claimed lags park (with wakes refused/absorbed)
       names the missing handoff. Does NOT relax any gate. */
    fprintf(stderr, "[watchdog] SV dbg park=%llu claimed=%llu absorbed=%llu "
                    "refused=%llu pop_discard=%llu retire_spin=%llu\n",
            (unsigned long long)rt_dbg.park,
            (unsigned long long)rt_dbg.wake_claimed,
            (unsigned long long)rt_dbg.wake_absorbed,
            (unsigned long long)rt_dbg.wake_refused,
            (unsigned long long)rt_dbg.pop_discard,
            (unsigned long long)rt_dbg.retire_spin);
#endif
    if (c2_sv_gate_mode) c2_sv_walk_guarded();
}

/* handoff=0: strict gate (old bt_p8 shape, stamping criterion).
   handoff=1: supplementary regression with the A' exit-handoff pass. */
static void c2_case_sv(int handoff) {
    coro_fn victim = handoff ? c2_sv_victim_h : c2_sv_victim;
    c2_sv_form = handoff ? "svh(supplementary)" : "sv(gate)";
    c2_sv_gate_mode = !handoff;
    c2_rt = coro_rt_create(C2_WORKERS);
    CHECK(c2_rt != NULL);
    if (!c2_rt) return;
    c2_sv_rounds_total = c2_sv_rounds_cfg();
    c2_at_store_u64(&c2_sv_stop_ns, 0);
    c2_at_store_u64(&c2_sv_cancel_last_ns, 0);
    c2_at_store_u64(&c2_sv_exit_last_ns, 0);
    c2_at_store_u64(&c2_sv_t0_ns, coro_monotonic_ns());
    /* Progress-bounded like R4: a slow host that keeps completing cycles is
       allowed up to the safety cap, a frozen cycle counter fails after a 2s
       stall window (the current tree freezes cycle 0). The cap SCALES with
       the configured rounds (a Windows cycle costs ~31ms: the default 300
       rounds fit 30s comfortably, but an extended C2_SV_ROUNDS=3000 run
       needs ~93s and must not be cap-killed while healthy). */
    {
        unsigned cap_ms = 30000u;
        if (c2_sv_rounds_total > 600)
            cap_ms = (unsigned)((uint64_t)c2_sv_rounds_total * 50ull);
        c2_wd_begin_progress(handoff ? "svh" : "sv", c2_sv_dump,
                             &c2_sv_progress, 2000u, cap_ms);
    }
    c2_at_store(&c2_sv_progress, 0);

    for (long c = 0; c < c2_sv_rounds_total; ++c) {
        pt_thread_t canc;
        c2_sv_cycle = c;
        coro_rt_mutex_init(&c2_sv_m);
        c2_at_store(&c2_sv_stop, 0);
        c2_at_store(&c2_sv_alive, (long)C2_SV_VICTIMS);
        c2_at_store(&c2_sv_cancel_calls, 0);
        for (unsigned i = 0; i < C2_SV_VICTIMS; ++i) {
            c2_sv_vh[i] = NULL;
            c2_sv_node[i] = NULL;
            CHECK(coro_rt_spawn_h(c2_rt, NULL, victim, NULL,
                                  &c2_sv_vh[i]) == CORO_OK);
            if (c2_sv_vh[i])
                c2_sv_node[i] = coro_get_sched_tag(c2_sv_vh[i]);
        }
        CHECK(coro_rt_spawn(c2_rt, NULL, c2_sv_holder, NULL) == CORO_OK);
        sleep_ms(1);

        c2_at_store(&c2_sv_cancel_run, 1);
        CHECK(pt_thread_create(&canc, c2_sv_canceler, NULL) == 0);
        sleep_ms(5);
        /* The red-team shape (audit-B' bt_p8.c): stop first, then let the
           canceler drain -- the residual wind-down window (a victim exiting
           with ECANCELED while the holder is parked on the wait chain) is
           exactly what the case must pin. */
        c2_at_store_u64(&c2_sv_stop_ns, coro_monotonic_ns());
        c2_at_store(&c2_sv_stop, 1);
        c2_at_store(&c2_sv_cancel_run, 0);
        (void)pt_thread_join(canc);

        CHECK(coro_rt_wait_all(c2_rt) == CORO_OK);
        C2_CHECK_MSG(coro_rt_pending(c2_rt) == 0,
                     "SV cycle=%ld bounded completion: pending=%zu alive=%ld "
                     "cancel_calls=%ld",
                     c, coro_rt_pending(c2_rt), c2_at_load(&c2_sv_alive),
                     c2_at_load(&c2_sv_cancel_calls));
        if (coro_rt_pending(c2_rt) != 0) break;
        for (unsigned i = 0; i < C2_SV_VICTIMS; ++i)
            C2_CHECK_MSG(coro_rt_collect(c2_rt, c2_sv_vh[i]) == CORO_OK,
                         "SV cycle=%ld collect(%u) failed", c, i);
        C2_CHECK_MSG(c2_at_load(&c2_sv_alive) == 0,
                     "SV cycle=%ld alive=%ld", c, c2_at_load(&c2_sv_alive));
        (void)c2_at_add(&c2_sv_progress, 1);
    }

    c2_wd_end();
    CHECK(coro_rt_destroy(c2_rt) == CORO_OK);
    c2_rt = NULL;
}

static void c2_run_concurrency(void) {
    if (c2_case_selected("r1")) c2_case_r1();
    if (c2_case_selected("r2")) c2_case_r2();
    if (c2_case_selected("r3")) c2_case_r3();
    if (c2_case_selected("r4")) c2_case_r4();
    if (c2_case_selected("r5")) c2_case_r5();
    if (c2_case_selected("sv")) c2_case_sv(0);    /* strict gate (old form) */
    if (c2_case_selected("svh")) c2_case_sv(1);   /* supplementary (handoff) */
}

static void run_tests(void) {
    /* ---- mutex park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_mutex_init(&cp_mutex);
        cp_mutex_st = CORO_OK;
        cp_mutex_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_mutex_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_mutex_st == CORO_ECANCELED);
        CHECK(cp_mutex_ran == 1u);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- cond park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_mutex_init(&cp_cond_m);
        coro_rt_cond_init(&cp_cond);
        cp_cond_st = CORO_OK;
        cp_cond_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_cond_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_cond_st == CORO_OK);   /* the re-acquired mutex */
        CHECK(cp_cond_ran == 1u);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- semaphore park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_sem_init(&cp_sem, 0);
        cp_sem_st = CORO_OK;
        cp_sem_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_sem_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_sem_st == CORO_ECANCELED);
        CHECK(cp_sem_ran == 1u);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- rwlock reader park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_rwlock_init(&cp_rw);
        cp_rd_st = CORO_OK;
        cp_rd_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_rd_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_rd_st == CORO_ECANCELED);
        CHECK(cp_rd_ran == 1u);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- rwlock writer park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_rwlock_init(&cp_rw);
        cp_wr_st = CORO_OK;
        cp_wr_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_wr_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_wr_st == CORO_ECANCELED);
        CHECK(cp_wr_ran == 1u);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- channel sender park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        CHECK(coro_rt_channel_init(&cp_ch, 1) == CORO_OK);
        cp_send_st = CORO_OK;
        cp_send_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_send_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_send_st == CORO_ECANCELED);
        CHECK(cp_send_ran == 1u);
        /* The controller's item is still queued; drain before destroy. */
        {
            void* rest = NULL;
            while (coro_rt_channel_size(&cp_ch) > 0)
                (void)coro_rt_channel_tryrecv(rt, &cp_ch, &rest);
        }
        CHECK(coro_rt_channel_destroy(&cp_ch) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- channel receiver park cancelled at its safe point ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        CHECK(coro_rt_channel_init(&cp_ch, 0) == CORO_OK); /* empty, open */
        cp_recv_st = CORO_OK;
        cp_recv_ran = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_recv_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_recv_st == CORO_ECANCELED);
        CHECK(cp_recv_ran == 1u);
        CHECK(coro_rt_channel_destroy(&cp_ch) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- sleep cancelled at its safe point, victim returns at once ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        cp_sleep_st = CORO_OK;
        CHECK(coro_rt_spawn(rt, NULL, cp_sleep_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(cp_sleep_st == CORO_ECANCELED);
        /* Past the abandoned deadline: a stale heap entry would drive the
           (already freed) victim node here. ASan reports the
           heap-use-after-free; nothing may happen at all. */
        sleep_ms(120);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- sleep cancelled at its safe point, then a plain yield ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        cp_sleepy_resumes = 0;
        cp_sleepy_early = 0;
        cp_sleepy_late = 0;
        CHECK(coro_rt_spawn(rt, NULL, cp_sleepy_controller, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        /* (a) primary: the victim was requeued by its plain yield and had
           finished before the controller's second resume (no clock
           margin); (b) auxiliary: not revived only at the deadline. */
        CHECK(cp_sleepy_early == 1u);
        CHECK(cp_sleepy_late == 0);
        CHECK(cp_sleepy_resumes == 1u);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- C2 concurrency regressions (see the section above) ---- */
    c2_run_concurrency();
}

TEST_MAIN()
