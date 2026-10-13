#include "../harness.h"
#include <corocc/coro.h>
#include <stdlib.h>
#include <string.h>

#define ALLOC_MAGIC 0xA110C801u

static int g_alloc_stack_calls;
static int g_free_stack_calls;
static int g_alloc_handle_calls;
static int g_free_handle_calls;

static coro_status test_alloc_stack(void** mem, size_t size) {
    ++g_alloc_stack_calls;
    unsigned* p = (unsigned*)malloc(size + sizeof(unsigned));
    if (!p) return CORO_ENOMEM;
    p[0] = ALLOC_MAGIC;
    *mem = (void*)(p + 1);
    return CORO_OK;
}

static void test_free_stack(void* mem) {
    ++g_free_stack_calls;
    unsigned* p = (unsigned*)mem - 1;
    CHECK(p[0] == ALLOC_MAGIC);
    free(p);
}

static void* test_alloc_handle(size_t size) {
    ++g_alloc_handle_calls;
    return malloc(size);
}

static void test_free_handle(void* p) {
    ++g_free_handle_calls;
    free(p);
}

static void yield_entry(void* pv) {
    (void)pv;
    for (int i = 0; i < 5; ++i) {
        coro_status st = coro_yield();
        CHECK(st == CORO_OK);
    }
}

static void run_tests(void) {
    {
        g_alloc_stack_calls = 0;
        g_free_stack_calls = 0;
        g_alloc_handle_calls = 0;
        g_free_handle_calls = 0;

        coro_attr_t a = coro_attr_default();
        a.allocator = NULL;

        coro_t* c = NULL;
        CHECK(coro_create(&c, &a, yield_entry, NULL) == CORO_OK);
        CHECK(c != NULL);
        coro_token next = CORO_TOKEN_NONE;
        CHECK(coro_start(c, &next) == CORO_OK);
        CHECK(coro_destroy(c) == CORO_OK);

        CHECK(g_alloc_stack_calls == 0);
        CHECK(g_free_stack_calls == 0);
        CHECK(g_alloc_handle_calls == 0);
        CHECK(g_free_handle_calls == 0);
    }

    {
        g_alloc_stack_calls = 0;
        g_free_stack_calls = 0;
        g_alloc_handle_calls = 0;
        g_free_handle_calls = 0;

        coro_attr_t a = coro_attr_default();
        coro_allocator_t alloc;
        alloc.alloc_stack = test_alloc_stack;
        alloc.free_stack = test_free_stack;
        alloc.alloc_handle = test_alloc_handle;
        alloc.free_handle = test_free_handle;
        a.allocator = &alloc;

        coro_t* c = NULL;
        CHECK(coro_create(&c, &a, yield_entry, NULL) == CORO_OK);
        CHECK(c != NULL);
        coro_token next = CORO_TOKEN_NONE;
        CHECK(coro_start(c, &next) == CORO_OK);
        CHECK(coro_destroy(c) == CORO_OK);

        CHECK(g_alloc_stack_calls == 1);
        CHECK(g_free_stack_calls == 1);
        CHECK(g_alloc_handle_calls == 1);
        CHECK(g_free_handle_calls == 1);
    }
}

TEST_MAIN()
