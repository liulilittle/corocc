/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/rt_driver.c -- the process-level runtime driver registry.
 *
 * A driver instance is created by coro_rt_driver_register (the ops block is
 * copied, the name is copied), linked into one process-wide list, and freed by
 * coro_rt_driver_unregister. The registry also holds the process default, which
 * coro_rt_create reads; a NULL default is the built-in EDSM backend.
 *
 * The registry is mutable process state, so every operation takes one lock. That
 * lock is a zero-initialized atomic spin (0 free, 1 held) rather than a
 * coro_plat_mutex_t, so it needs no platform init call and no static-init order
 * (design alternative A, zero shim change): the first acquirer wins a
 * compare-and-exchange on an otherwise zero word. Registry traffic -- register,
 * find, set_default, unregister -- is configuration-time activity; the runtime
 * hot path never touches this lock.
 *
 * This translation unit compiles as C99.
 */
#include "scheduler/rt_driver.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "arch/coro_atomic.h"

/* Process-level registry state. Zero-initialized statics are the whole
 * initialization contract: an all-zero word is a free lock, an empty list and a
 * built-in default. */
static coro_rt_driver_t* g_registry;  /* list head, NULL when empty */
static coro_rt_driver_t* g_default;   /* process default, NULL = built-in */
static uintptr_t         g_reg_lock;  /* 0 = free, 1 = held */

/* Acquire the registry spin. The winning compare-and-exchange is an
 * acquire/release RMW, so the previous holder's list writes are visible to this
 * thread. */
static void reg_lock(void) {
    uintptr_t exp = 0;
    while (!coro_atp_cas((volatile uintptr_t*)&g_reg_lock, &exp, 1)) {
        exp = 0; /* lost: coro_atp_cas left the observed value in exp */
    }
}

/* Release the registry spin (release store). */
static void reg_unlock(void) {
    coro_atp_store((volatile uintptr_t*)&g_reg_lock, 0);
}

/* ---- hold counts (drv->refs) ----
 *
 * refs counts the runtimes that currently hold a registered instance as their
 * driver (rt->drv): one per create-time default still in effect and one per
 * live attach. coro_rt_driver_unregister refuses while it is non-zero, exactly
 * as the header contract demands ("the process default ... or ... attached to
 * any runtime"): a runtime created from a default keeps holding that instance
 * even after coro_rt_driver_set_default clears the process default.
 *
 * refs is read and written only under g_reg_lock, so it can never race the
 * unregister decision. Lock order: rt_driver_publish_locked takes g_reg_lock
 * while already holding a runtime's drv_lock, so the one nesting edge in the
 * system is drv_lock -> g_reg_lock. No path holds g_reg_lock and then acquires
 * drv_lock (or any other runtime lock): the registry entry points take
 * g_reg_lock alone and never call back into a runtime, and g_reg_lock is a
 * leaf that nothing acquires while held. The order is therefore acyclic (D11).
 */
static void refs_dec(coro_rt_driver_t* drv) {
    if (!drv) return;
    reg_lock();
    if (drv->refs > 0) drv->refs--;
    reg_unlock();
}

/* Copy a NUL-terminated string onto the heap; NULL in, NULL out. */
static char* dup_cstr(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

coro_status coro_rt_driver_register(const char* name,
                                    const struct coro_rt_driver_ops* ops,
                                    void* ctx, coro_rt_driver_t** out) {
    if (out) *out = NULL;
    if (!out || !ops) return CORO_EINVAL;
    if (!ops->wake) return CORO_EINVAL;
    if (ops->struct_size < (uint32_t)sizeof(struct coro_rt_driver_ops)) {
        return CORO_EINVAL;
    }
    /* timer_arm and timer_cancel come as a pair: an arm without a cancel could
     * not withdraw an in-flight alarm, which the runtime relies on. */
    if ((ops->timer_arm == NULL) != (ops->timer_cancel == NULL)) {
        return CORO_EINVAL;
    }

    coro_rt_driver_t* d = (coro_rt_driver_t*)calloc(1, sizeof *d);
    if (!d) return CORO_ENOMEM;
    d->ops = *ops;
    d->ctx = ctx;
    d->refs = 0;
    if (name) {
        d->name = dup_cstr(name);
        if (!d->name) {
            free(d);
            return CORO_ENOMEM;
        }
    }

    reg_lock();
    if (name) {
        for (coro_rt_driver_t* p = g_registry; p; p = p->next) {
            if (p->name && strcmp(p->name, name) == 0) {
                reg_unlock();
                free(d->name);
                free(d);
                return CORO_ESTATE; /* ruling #2: names are unique */
            }
        }
    }
    d->next = g_registry;
    g_registry = d;
    reg_unlock();

    *out = d;
    return CORO_OK;
}

coro_status coro_rt_driver_unregister(coro_rt_driver_t* drv) {
    if (!drv) return CORO_OK; /* idempotent */

    reg_lock();
    coro_rt_driver_t** link = &g_registry;
    while (*link && *link != drv) link = &(*link)->next;
    if (!*link) {
        reg_unlock();
        return CORO_EINVAL; /* not a driver this registry owns */
    }
    if (drv->refs > 0 || g_default == drv) {
        reg_unlock();
        return CORO_EBUSY; /* in use: default now, attached from B2 on */
    }
    *link = drv->next;
    reg_unlock();

    free(drv->name);
    free(drv);
    return CORO_OK;
}

coro_rt_driver_t* coro_rt_driver_find(const char* name) {
    if (!name) return NULL;
    reg_lock();
    coro_rt_driver_t* found = NULL;
    for (coro_rt_driver_t* p = g_registry; p; p = p->next) {
        if (p->name && strcmp(p->name, name) == 0) {
            found = p;
            break;
        }
    }
    reg_unlock();
    return found;
}

coro_status coro_rt_driver_set_default(coro_rt_driver_t* drv) {
    reg_lock();
    if (drv) {
        int registered = 0;
        for (coro_rt_driver_t* p = g_registry; p; p = p->next) {
            if (p == drv) {
                registered = 1;
                break;
            }
        }
        if (!registered) {
            reg_unlock();
            return CORO_EINVAL;
        }
    }
    g_default = drv;
    reg_unlock();
    return CORO_OK;
}

coro_rt_driver_t* coro_rt_driver_default(void) {
    reg_lock();
    coro_rt_driver_t* d = g_default;
    reg_unlock();
    return d;
}

/* ---- runtime-side hold management ---- */

/* Pin a registered instance so unregister refuses until rt_driver_unpin. The
 * ownership test and the increment share one g_reg_lock critical section, so a
 * concurrent unregister cannot free drv between "is it registered?" and "hold
 * it" -- the race coro_rt_attach_driver would otherwise lose. Returns 1 when
 * pinned. */
int rt_driver_pin(coro_rt_driver_t* drv) {
    if (!drv) return 0;
    reg_lock();
    int found = 0;
    for (coro_rt_driver_t* p = g_registry; p; p = p->next) {
        if (p == drv) { found = 1; break; }
    }
    if (found) drv->refs++;
    reg_unlock();
    return found;
}

/* Drop one hold. Used for attach's transient guard and for a runtime releasing
 * the driver it held at coro_rt_destroy. */
void rt_driver_unpin(coro_rt_driver_t* drv) { refs_dec(drv); }

/* Pin the current process default and return it, or NULL when the built-in
 * EDSM path is in effect. The pin shares the read's critical section, so a
 * concurrent set_default(NULL)+unregister cannot free the instance between the
 * read and coro_rt_create storing it. */
coro_rt_driver_t* rt_driver_default_pin(void) {
    reg_lock();
    coro_rt_driver_t* d = g_default;
    if (d) d->refs++;
    reg_unlock();
    return d;
}

/* ---- attach / detach ---- */

int rt_driver_registered(const coro_rt_driver_t* drv) {
    int found = 0;
    if (!drv) return 0;
    reg_lock();
    for (coro_rt_driver_t* p = g_registry; p; p = p->next) {
        if (p == drv) { found = 1; break; }
    }
    reg_unlock();
    return found;
}

/* Migrate one worker's outstanding alarm from old to new (drv_lock held). Only
   the worker's own alarm token is touched -- never an rt_node. */
static void rt_driver_rearm_locked(coro_rt_t* rt, rt_worker* w,
                                   coro_rt_driver_t* old,
                                   coro_rt_driver_t* nw, uint64_t new_gen) {
    if (!w->alarm_armed) return;
    if (old && old->ops.timer_cancel) {
        (void)old->ops.timer_cancel(old->ctx, w->alarm_req);
    }
    w->alarm_armed = 0;
    if (nw && nw->ops.timer_arm) {
        if (nw->ops.timer_arm(nw->ctx, w->alarm_req, w->alarm_deadline,
                              rt_alarm_wake, rt) == 0) {
            w->alarm_armed = 1;
            w->alarm_driver = nw;
            w->alarm_gen = new_gen;
        }
    }
}

/* Wait until every worker has entered its loop and stopped at the barrier.
 *
 * The caller has set rt->drv_barrier and must clear it on failure. Two phases:
 * first the start handshake (every worker thread has reached worker_main), then
 * the park itself (every worker has reached the barrier and is off the run
 * queue). Each phase has a generous upper bound -- five seconds -- so a slow or
 * emulated host, or a worker the OS simply has not scheduled yet, cannot turn a
 * healthy switch into a spurious EBUSY. A real timeout means a worker is stuck
 * mid-drive and cannot be parked, which is exactly when the switch must refuse.
 *
 * Cross-class switches only: a same-family switch never parks a worker and has
 * no wall-clock dependence at all (the B2fix property preserved here). */
static int rt_quiesce_wait(coro_rt_t* rt) {
    rt_signal_activity(rt); /* prod every worker toward the barrier */
    coro_plat_mutex_lock(&rt->act_lock);
    uint64_t d1 = coro_monotonic_ns() + 5000000000ull; /* 5 s: start handshake */
    while (coro_at_load((volatile uint64_t*)&rt->drv_started) < rt->nworkers) {
        if (coro_plat_cond_timedwait_ns(&rt->act_cond, &rt->act_lock, d1) != 0) {
            break;
        }
    }
    uint64_t d2 = coro_monotonic_ns() + 5000000000ull; /* 5 s: park */
    while (coro_at_load((volatile uint64_t*)&rt->drv_parked) < rt->nworkers) {
        if (coro_plat_cond_timedwait_ns(&rt->act_cond, &rt->act_lock, d2) != 0) {
            break;
        }
    }
    int ok = coro_at_load((volatile uint64_t*)&rt->drv_started) >= rt->nworkers &&
             coro_at_load((volatile uint64_t*)&rt->drv_parked) >= rt->nworkers;
    coro_plat_mutex_unlock(&rt->act_lock);
    return ok;
}

/* The common switch path; drv == NULL means the built-in backend.
 *
 * Same family (BUILTIN <-> SIGNAL <-> SIGNAL), or LOOP <-> LOOP: O(1), no
 * worker parked, no wall-clock wait. Under drv_lock each worker's outstanding
 * alarm is withdrawn from the old driver and re-armed on the new one at the
 * same stored deadline, then the new driver is published and drv_gen is bumped.
 * (A LOOP target arms nothing: the host pump's collect_expired observes
 * deadlines, so the rearm is a withdraw.) Serializing on drv_lock is what makes
 * that safe against a worker arming or cancelling its own alarm in worker_sleep
 * (the same lock); the generation bump is the worker's self-heal -- worker_sleep
 * re-evaluates whenever drv_gen changed since it read it, so a worker mid-sleep
 * abandons the old mechanism and re-arms on the new driver at its next pass.
 * The migration always withdraws the old alarm before dropping it, so the old
 * driver cannot fire a stale callback after the switch.
 *
 * Cross family is a live hot switch (ruling #4, completed by B5):
 *   - threaded -> LOOP quiesces every worker at the barrier, then withdraws
 *     their alarms and leaves the barrier set for the whole LOOP phase, so the
 *     parked worker threads never touch a queue while the host pumps worker[0]
 *     through coro_rt_pump. Every in-flight deadline survives: the timer heaps
 *     are left untouched (B3's pump pattern observes them via collect_expired).
 *   - LOOP -> threaded releases the parked workers (starting them first if the
 *     runtime was created in LOOP and never had threads), clears the barrier and
 *     re-arms nothing: each worker re-arms its own alarm on the new driver the
 *     next time it sleeps. No quiesce is needed because the workers are already
 *     parked.
 *
 * Any switch that touches the LOOP family refuses with CORO_EBUSY while a pump
 * is running: the pump drives worker[0], so moving the runtime in or out of
 * LOOP underneath it would let two threads drive the same queue. A quiesce
 * timeout likewise rolls back to CORO_EBUSY with the runtime unchanged. */

/* Publish nw as rt->drv and move the hold count with it: the previous driver
 * loses one hold, the new one gains one. Must be called with rt->drv_lock held;
 * it takes g_reg_lock inside, which is the single nesting edge drv_lock ->
 * g_reg_lock (see the hold-count comment above). A same-instance republish (or
 * a switch that keeps the instance and only changes the family) moves nothing:
 * the runtime holds that one instance either way. */
static void rt_driver_publish_locked(coro_rt_t* rt, coro_rt_driver_t* nw) {
    coro_rt_driver_t* old =
        (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
    coro_atp_store((volatile uintptr_t*)&rt->drv, (uintptr_t)nw);
    if (old != nw) {
        reg_lock();
        if (old && old->refs > 0) old->refs--;
        if (nw) nw->refs++;
        reg_unlock();
    }
}

static coro_status rt_driver_switch(coro_rt_t* rt, coro_rt_driver_t* drv,
                                    unsigned nk) {
    coro_plat_mutex_lock(&rt->drv_lock);
    coro_rt_driver_t* cur =
        (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
    if (cur == drv && rt->drv_kind == nk) {
        coro_plat_mutex_unlock(&rt->drv_lock);
        return CORO_OK; /* idempotent */
    }
    unsigned ck = rt->drv_kind;
    int cur_loop = (ck == RT_DRV_LOOP);
    int new_loop = (nk == RT_DRV_LOOP);

    if ((cur_loop || new_loop) &&
        coro_at_load((volatile uint64_t*)&rt->pump_active) != 0) {
        coro_plat_mutex_unlock(&rt->drv_lock);
        return CORO_EBUSY; /* a pump owns worker[0] right now */
    }

    if (cur_loop == new_loop) {
        /* Same family: migrate alarms in place and publish. A LOOP target has
           no outstanding alarm, so it is a bare republish. */
        uint64_t new_gen = rt->drv_gen + 1;
        for (size_t i = 0; i < rt->nworkers; ++i) {
            rt_driver_rearm_locked(rt, &rt->workers[i], cur,
                                   new_loop ? NULL : drv, new_gen);
        }
        rt->drv_kind = nk;
        rt_driver_publish_locked(rt, drv);
        coro_at_store((volatile uint64_t*)&rt->drv_gen, new_gen);
        coro_plat_mutex_unlock(&rt->drv_lock);
        rt_signal_activity(rt);
        return CORO_OK;
    }

    if (!cur_loop && new_loop) {
        /* threaded -> LOOP: quiesce, then hold the barrier for the LOOP phase. */
        coro_at_store((volatile uint64_t*)&rt->drv_barrier, 1);
        coro_plat_mutex_unlock(&rt->drv_lock);

        if (!rt_quiesce_wait(rt)) {
            coro_plat_mutex_lock(&rt->drv_lock);
            coro_at_store((volatile uint64_t*)&rt->drv_barrier, 0);
            coro_plat_mutex_unlock(&rt->drv_lock);
            rt_signal_activity(rt);
            return CORO_EBUSY; /* rollback: no state was changed */
        }

        coro_plat_mutex_lock(&rt->drv_lock);
        uint64_t new_gen = rt->drv_gen + 1;
        for (size_t i = 0; i < rt->nworkers; ++i) {
            rt_driver_rearm_locked(rt, &rt->workers[i], cur, NULL, new_gen);
        }
        rt->drv_kind = nk;
        rt_driver_publish_locked(rt, drv);
        coro_at_store((volatile uint64_t*)&rt->drv_gen, new_gen);
        /* barrier stays 1: the workers remain parked while LOOP is in effect */
        coro_plat_mutex_unlock(&rt->drv_lock);
        return CORO_OK;
    }

    /* LOOP -> threaded: release the parked workers back into thread mode. */
    if (!coro_at_load((volatile uint64_t*)&rt->drv_threads)) {
        /* A LOOP-created runtime never had threads; start them now. They hold
           at the still-raised barrier until the release below. */
        if (!rt_start_workers(rt)) {
            coro_plat_mutex_unlock(&rt->drv_lock);
            return CORO_EBUSY; /* rollback: still LOOP, barrier still held */
        }
    }
    {
        uint64_t new_gen = rt->drv_gen + 1;
        for (size_t i = 0; i < rt->nworkers; ++i) {
            rt_driver_rearm_locked(rt, &rt->workers[i], cur, drv, new_gen);
        }
        rt->drv_kind = nk;
        rt_driver_publish_locked(rt, drv);
        coro_at_store((volatile uint64_t*)&rt->drv_gen, new_gen);
        coro_at_store((volatile uint64_t*)&rt->drv_barrier, 0); /* release */
    }
    coro_plat_mutex_unlock(&rt->drv_lock);

    rt_signal_activity(rt); /* wake the released workers */
    return CORO_OK;
}

coro_status coro_rt_attach_driver(coro_rt_t* rt, coro_rt_driver_t* drv) {
    if (!rt) return CORO_EINVAL;
    if (!drv) return coro_rt_detach_driver(rt);
    /* Pin the driver across the ownership check and the switch: it must stay
       alive; the publish inside rt_driver_switch moves the permanent hold onto
       the runtime. A pin that is not consumed by a publish (idempotent switch
       or a refused one) is dropped below, leaving the hold count unchanged. */
    if (!rt_driver_pin(drv)) return CORO_EINVAL;
    unsigned nk = (drv->ops.flags & CORO_DRIVER_FLAG_LOOP) ? RT_DRV_LOOP
                                                           : RT_DRV_SIGNAL;
    coro_status st = rt_driver_switch(rt, drv, nk);
    rt_driver_unpin(drv);
    return st;
}

coro_status coro_rt_detach_driver(coro_rt_t* rt) {
    if (!rt) return CORO_EINVAL;
    return rt_driver_switch(rt, NULL, RT_DRV_BUILTIN);
}

coro_rt_driver_t* coro_rt_attached_driver(const coro_rt_t* rt) {
    if (!rt) return NULL;
    return (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
}

coro_status coro_rt_pump(coro_rt_t* rt, size_t budget) {
    if (!rt) return CORO_EINVAL;
    if (rt->drv_kind != RT_DRV_LOOP) return CORO_EBUSY;
    if (coro_at_exchange((volatile uint64_t*)&rt->pump_active, 1) != 0) {
        return CORO_EBUSY; /* a pump is already running on this runtime */
    }
    /* The peer workers hold at the LOOP barrier, so their expired deadlines and
       pending inbox work are swept here (the pump thread acts as their owner)
       and then stolen by worker[0]'s run. This is what lets a runtime switched
       into LOOP from worker mode keep every in-flight sleep on time. */
    rt_loop_sweep(rt);
    (void)rt_run_some(rt, &rt->workers[0], budget == 0 ? (size_t)-1 : budget);
    coro_at_store((volatile uint64_t*)&rt->pump_active, 0);
    return CORO_OK;
}
