#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M2 Task 1: the coroutine object skeleton -- create, destroy and
   state query. Credentials (start/resume/yield) land with Task 2; this test
   pins the lifetime and error contracts they must preserve. */

static int g_ran = 0;

static void entry(void* arg) {
    (void)arg;
    g_ran = 1;
}

static void run_tests(void) {
    coro_t* c = NULL;

    /* defaults: nothing runs until someone starts the coroutine */
    coro_attr_t a = coro_attr_default();
    CHECK(a.stack_size == 0);
    CHECK(a.name == NULL);
    CHECK(a.flags == CORO_F_NONE);

    CHECK(coro_create(&c, NULL, entry, NULL) == CORO_OK);
    CHECK(c != NULL);
    CHECK(coro_get_state(c) == CORO_NEW);
    CHECK(g_ran == 0);

    /* explicit default size is accepted and honoured */
    coro_t* c2 = NULL;
    a.stack_size = CORO_DEFAULT_STACK_SIZE;
    CHECK(coro_create(&c2, &a, entry, NULL) == CORO_OK);
    CHECK(coro_get_state(c2) == CORO_NEW);

    /* invalid arguments leave *out untouched and allocate nothing */
    coro_t* untouched = (coro_t*)&untouched;
    CHECK(coro_create(NULL, NULL, entry, NULL) == CORO_EINVAL);
    CHECK(coro_create(&untouched, NULL, NULL, NULL) == CORO_EINVAL);
    CHECK(untouched == (coro_t*)&untouched);

    a.flags = 0xdeadbeefu;
    CHECK(coro_create(&untouched, &a, entry, NULL) == CORO_EINVAL);
    CHECK(untouched == (coro_t*)&untouched);

    /* a NULL handle never crashes the query */
    CHECK(coro_get_state(NULL) == CORO_NEW);

    /* destroy is allowed for a coroutine that never ran */
    CHECK(coro_destroy(c) == CORO_OK);
    CHECK(coro_destroy(c2) == CORO_OK);
    CHECK(coro_destroy(NULL) == CORO_EINVAL);

    /* nothing leaked: a fresh create after destroying the batch still works */
    CHECK(coro_create(&c, NULL, entry, NULL) == CORO_OK);
    CHECK(coro_get_state(c) == CORO_NEW);
    CHECK(coro_destroy(c) == CORO_OK);
}

TEST_MAIN()
