#include "../harness.h"
#include "coroutine/coro_runtime.h"
#include "platform/threads.h"

/* Milestone M3 Task 4, part 2: the runtime's own concurrency, under
 * ThreadSanitizer.
 *
 * ThreadSanitizer cannot follow a stack switch performed by assembly, so the
 * drive path itself is not instrumentable; the parts that CAN race -- the
 * delivery queue, the identity registry and the atomic helpers -- live in the
 * runtime and are exercised here without any context switch. This is the
 * TSan-visible half of the milestone's concurrency work.
 *
 * What is checked: many threads append deliveries to one owner's queue and
 * read the registry while other threads create and destroy runtimes, and the
 * owner then drains every delivery exactly once.
 */

#if defined(_WIN32)
static void run_tests(void) {
    CHECK(0 && "runtime concurrency suite: POSIX threads only in M3");
}
TEST_MAIN()
#else

#include <pthread.h>
#include <string.h>

#define PRODUCERS 4u
#define PER_THREAD 500u

static coro_runtime* g_owner;
static coro_delivery g_nodes[PRODUCERS][PER_THREAD];
static unsigned       g_seen_count;

static void* producer(void* pv) {
    unsigned id = (unsigned)(uintptr_t)pv;
    for (unsigned i = 0; i < PER_THREAD; ++i) {
        coro_delivery* node = &g_nodes[id][i];
        node->coro = NULL; /* queued, not driven, in this suite */
        node->tok = (coro_token)(id * PER_THREAD + i + 1u);
        node->published = CORO_TOKEN_NONE;
        node->done = 0;
        node->status = CORO_OK;
        node->signal = NULL;
        /* registry reads race with other threads creating runtimes */
        (void)coro_runtime_by_id(id + 1u);
        coro_runtime_enqueue(g_owner, node);
    }
    return NULL;
}

static void* churner(void* pv) {
    (void)pv;
    /* creates and uses a runtime repeatedly, racing the registry */
    for (unsigned i = 0; i < 50u; ++i) {
        coro_runtime* r = coro_runtime_current();
        CHECK(r != NULL);
        CHECK(coro_runtime_by_id(r->id) == r);
    }
    return NULL;
}

static void run_tests(void) {
    pthread_t p[PRODUCERS], ch;
    static unsigned seen[PRODUCERS][PER_THREAD];
    size_t drained = 0;

    memset(seen, 0, sizeof seen);
    g_owner = coro_runtime_current();
    CHECK(g_owner != NULL);
    CHECK(coro_runtime_by_id(g_owner->id) == g_owner);
    CHECK(coro_runtime_pending(g_owner) == 0u);

    for (unsigned i = 0; i < PRODUCERS; ++i) {
        CHECK(pthread_create(&p[i], NULL, producer, (void*)(uintptr_t)i) == 0);
    }
    CHECK(pthread_create(&ch, NULL, churner, NULL) == 0);

    for (unsigned i = 0; i < PRODUCERS; ++i) pthread_join(p[i], NULL);
    pthread_join(ch, NULL);

    /* drain and account: every queued delivery must come out exactly once */
    for (;;) {
        coro_delivery* d = coro_runtime_pop(g_owner);
        unsigned id, idx;
        if (!d) break;
        ++drained;
        CHECK(d->tok != CORO_TOKEN_NONE);
        /* the token encodes its producer and index, so a duplicate or a lost
           item is detectable */
        id = (unsigned)((d->tok - 1u) / PER_THREAD);
        idx = (unsigned)((d->tok - 1u) % PER_THREAD);
        CHECK(id < PRODUCERS);
        CHECK(idx < PER_THREAD);
        CHECK(seen[id][idx] == 0u); /* exactly once */
        seen[id][idx] = 1u;
        ++g_seen_count;
    }
    CHECK(drained == (size_t)PRODUCERS * PER_THREAD);
    CHECK(g_seen_count == PRODUCERS * PER_THREAD);
    CHECK(coro_runtime_pending(g_owner) == 0u);
}

TEST_MAIN()
#endif
