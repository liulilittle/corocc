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
