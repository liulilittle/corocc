#include "../harness.h"
#include "coroutine/coro_runtime.h"   /* internal: the current coroutine */
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>        /* coro_monotonic_ns */
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h> /* Sleep in the thread-side wait below */
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
}

TEST_MAIN()
