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

/* The common switch path; drv == NULL means the built-in backend.
 *
 * A same-family switch (BUILTIN <-> SIGNAL <-> SIGNAL) is O(1) and never parks
 * a worker: under drv_lock it withdraws each worker's outstanding alarm from
 * the old driver and re-arms it, at the same stored deadline, on the new one,
 * then publishes the new driver and bumps drv_gen. Serializing on drv_lock is
 * what makes that safe against a worker arming or cancelling its own alarm in
 * worker_sleep (which takes the same lock); the generation bump is the worker's
 * self-heal -- worker_sleep re-evaluates whenever drv_gen changed since it read
 * it, so a worker mid-sleep abandons the old mechanism and re-arms on the new
 * driver at its next pass.
 *
 * The migration withdraws the old alarm before dropping it (never leaves one
 * outstanding), so the old driver cannot fire a stale callback after the
 * switch -- the invariant the N2 probe pins down. With no barrier to wait on,
 * attach has no wall-clock dependence and succeeds immediately on a fresh
 * runtime whose workers have not parked yet.
 *
 * A cross-family switch (LOOP vs. non-LOOP) is refused with CORO_EBUSY in v1:
 * LOOP changes who runs the worker loop, so a live switch needs the worker
 * quiesce barrier that runtime cross-class switching (B4) will bring back. */
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
    /* LOOP is a different execution model: v1 refuses every cross-family
       switch (ruling #4; runtime cross-class switching is B4). */
    if ((ck == RT_DRV_LOOP) != (nk == RT_DRV_LOOP)) {
        coro_plat_mutex_unlock(&rt->drv_lock);
        return CORO_EBUSY;
    }
    uint64_t new_gen = rt->drv_gen + 1;
    for (size_t i = 0; i < rt->nworkers; ++i) {
        rt_driver_rearm_locked(rt, &rt->workers[i], cur, drv, new_gen);
    }
    rt->drv_kind = nk;
    coro_atp_store((volatile uintptr_t*)&rt->drv, (uintptr_t)drv);
    coro_at_store((volatile uint64_t*)&rt->drv_gen, new_gen);
    coro_plat_mutex_unlock(&rt->drv_lock);

    rt_signal_activity(rt); /* release any parked workers / raise the doorbell */
    return CORO_OK;
}

coro_status coro_rt_attach_driver(coro_rt_t* rt, coro_rt_driver_t* drv) {
    if (!rt) return CORO_EINVAL;
    if (!drv) return coro_rt_detach_driver(rt);
    if (!rt_driver_registered(drv)) return CORO_EINVAL;
    unsigned nk = (drv->ops.flags & CORO_DRIVER_FLAG_LOOP) ? RT_DRV_LOOP
                                                           : RT_DRV_SIGNAL;
    return rt_driver_switch(rt, drv, nk);
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
    (void)rt_run_some(rt, &rt->workers[0], budget == 0 ? (size_t)-1 : budget);
    coro_at_store((volatile uint64_t*)&rt->pump_active, 0);
    return CORO_OK;
}
