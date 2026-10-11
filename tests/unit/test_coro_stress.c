#include "../harness.h"
#include <corocc/coro.h>

#include <string.h>

/* Milestone M2 Task 5, part 4: randomised stress over the credential API.
 *
 * A deterministic xorshift sequence drives create/start/resume/yield/cancel/
 * destroy against a pool of coroutines for a fixed number of steps, asserting
 * the status of every operation and that every handle is either parked with a
 * live credential or dead at the end. Reproducible by construction: the same
 * seed always produces the same sequence, so a failure can be replayed.
 */

#define POOL 8u
#define STEPS 20000u

struct slot {
    coro_t*    c;
    coro_token tok;
    int        live;
    unsigned   rounds;
};

static struct slot g_pool[POOL];

/* Parks `rounds` times, then returns; a pending cancellation stops it early. */
static void entry_fuzz(void* arg) {
    struct slot* s = (struct slot*)arg;
    while (s->rounds > 0) {
        --s->rounds;
        if (coro_yield() == CORO_ECANCELED) return;
    }
}

static unsigned long g_state = 0x9e3779b97f4a7c15ul;

static unsigned long rnd(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

static unsigned pick(void) {
    return (unsigned)(rnd() % 100u);
}

static int find_free(void) {
    for (unsigned i = 0; i < POOL; ++i) {
        if (!g_pool[i].live) return (int)i;
    }
    return -1;
}

static void run_tests(void) {
    memset(g_pool, 0, sizeof g_pool);

    for (unsigned step = 0; step < STEPS; ++step) {
        unsigned op = pick();
        int slot = -1;
        for (unsigned i = 0; i < POOL; ++i) {
            if (g_pool[i].live) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0 && op >= 30u) continue; /* nothing to act on yet */

        if (op < 30u) {                                  /* create */
            int free_slot = find_free();
            if (free_slot >= 0) {
                struct slot* s = &g_pool[free_slot];
                s->rounds = 1u + (unsigned)(rnd() % 4u);
                CHECK(coro_create(&s->c, NULL, entry_fuzz, s) == CORO_OK);
                s->tok = CORO_TOKEN_NONE;
                s->live = 1;
                CHECK(coro_get_state(s->c) == CORO_NEW);
            }
        } else if (op < 50u) {                            /* start */
            struct slot* s = &g_pool[slot];
            coro_token next = CORO_TOKEN_NONE;
            coro_status st = coro_start(s->c, &next);
            if (st == CORO_OK) {
                s->tok = next;
                if (next == CORO_TOKEN_DEAD) {
                    CHECK(coro_destroy(s->c) == CORO_OK);
                    s->live = 0;
                }
            } else {
                /* only a coroutine that never ran may be started */
                CHECK(st == CORO_ESTALE || st == CORO_EBUSY ||
                      st == CORO_EDESTROYED);
            }
        } else if (op < 75u) {                            /* resume */
            struct slot* s = &g_pool[slot];
            coro_token next = CORO_TOKEN_NONE;
            coro_status st = coro_resume(s->c, s->tok, &next);
            if (st == CORO_OK) {
                s->tok = next;
                if (next == CORO_TOKEN_DEAD) {
                    CHECK(coro_destroy(s->c) == CORO_OK);
                    s->live = 0;
                }
            } else {
                CHECK(st == CORO_ESTALE || st == CORO_EBUSY ||
                      st == CORO_EDESTROYED || st == CORO_EINVAL);
            }
        } else if (op < 90u) {                            /* cancel */
            struct slot* s = &g_pool[slot];
            CHECK(coro_cancel(s->c) == CORO_OK);
        } else {                                          /* destroy */
            struct slot* s = &g_pool[slot];
            coro_status st = coro_destroy(s->c);
            if (st == CORO_OK) {
                s->live = 0;
            } else {
                CHECK(st == CORO_EBUSY);
            }
        }
    }

    /* settle: drive every survivor to completion, then free everything. A
       live slot holds either the credential of its last suspension, or no
       credential at all when it was created but never started. */
    for (unsigned i = 0; i < POOL; ++i) {
        struct slot* s = &g_pool[i];
        if (!s->live) continue;
        for (;;) {
            coro_token next = CORO_TOKEN_NONE;
            coro_status st = (s->tok == CORO_TOKEN_NONE)
                                 ? coro_start(s->c, &next)
                                 : coro_resume(s->c, s->tok, &next);
            CHECK(st == CORO_OK);
            if (st != CORO_OK) break;
            s->tok = next;
            if (next == CORO_TOKEN_DEAD) break;
        }
        CHECK(coro_join(s->c) == CORO_OK);
        CHECK(coro_get_state(s->c) == CORO_DEAD);
        CHECK(coro_destroy(s->c) == CORO_OK);
        s->live = 0;
    }
    CHECK(find_free() == 0);
}

TEST_MAIN()
