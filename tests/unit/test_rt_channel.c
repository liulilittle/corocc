#include "../harness.h"
#include "arch/coro_atomic.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <string.h>

/* M5 T3: coro_rt_channel -- a bounded or unbounded FIFO of void* between
 * runtime coroutines.
 *
 * Behaviours pinned:
 *   - a bounded channel keeps at most its capacity items; a sender blocks
 *     while it is full and is released by a matching receive;
 *   - an unbounded channel never blocks a sender and drains FIFO;
 *   - M*N producers and consumers over one channel lose and duplicate
 *     nothing: every tagged item arrives exactly once;
 *   - trysend on a full channel and tryrecv on an empty one report EBUSY;
 *   - closing lets a receiver drain what is buffered and only then report
 *     CORO_ECLOSED, while parked senders and receivers are all released;
 *   - a sender that is cancelled while another sender still holds a slot
 *     reports CORO_ECANCELED, and the channel stays usable with no lost
 *     item and no phantom waiter;
 *   - destroy refuses while items or waiters remain and is idempotent.
 *
 * Items are tagged pointers (the test's own bit patterns), so there is no
 * payload memory to free and nothing to leak.
 */

#define MSGS   64u
#define NPROD  4u
#define NCONS  4u
#define SMALL  2u    /* bounded capacity for the blocking cases */

static coro_rt_channel_t g_ch;

static uint64_t g_seen[(unsigned)(NPROD * MSGS)];
static uint64_t g_arrived;      /* distinct items received      */
static uint64_t g_total;        /* total successful receives    */
static uint64_t g_stray;        /* untagged or out-of-range     */

/* Tag: index+1 shifted left by 3 -- never NULL, never 0, decodes exactly. */
static void* tag_of(uint64_t i)   { return (void*)(uintptr_t)((i + 1u) << 3); }
static uint64_t untag_of(void* v) { return (uint64_t)(uintptr_t)v >> 3; }

/* ---- bounded M x N ------------------------------------------------- */

static uint64_t g_cons_quota;   /* items this consumer must take */

/* Each producer takes a disjoint block of tag ids so every value in the whole
   run is unique: the receivers then prove that nothing was lost, duplicated or
   reordered into the wrong slot. */
static uint64_t g_prod_seq;
static uint64_t g_arrived_at;   /* atomic version of the distinct-arrivals count */

static void producer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    uint64_t base = coro_at_fetch_add(&g_prod_seq, MSGS);
    for (uint64_t i = 0; i < MSGS; ++i)
        (void)coro_rt_channel_send(rt, &g_ch, tag_of(base + i));
}

/* Takes exactly its quota, blocking when the channel is empty, so the
   park/wake path on both sides is exercised. */
static void consumer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (uint64_t i = 0; i < g_cons_quota; ++i) {
        void* v = NULL;
        if (coro_rt_channel_recv(rt, &g_ch, &v) != CORO_OK) return;
        uint64_t slot = untag_of(v);
        if (slot == 0 || slot > (uint64_t)NPROD * MSGS) { ++g_stray; continue; }
        if (coro_at_fetch_add(&g_seen[slot - 1], 1) == 0)
            (void)coro_at_fetch_add(&g_arrived_at, 1);
        (void)coro_at_fetch_add(&g_total, 1);
    }
}

static void run_bounded(void) {
    coro_rt_t* rt = coro_rt_create(4);
    CHECK(rt != NULL);
    memset(&g_ch, 0, sizeof g_ch);
    CHECK(coro_rt_channel_init(&g_ch, SMALL) == CORO_OK);
    memset(g_seen, 0, sizeof g_seen);
    g_arrived = g_arrived_at = g_total = g_stray = 0;
    g_prod_seq = 0;

    for (uint64_t i = 0; i < NPROD; ++i)
        CHECK(coro_rt_spawn(rt, NULL, producer, rt) == CORO_OK);
    g_cons_quota = (uint64_t)NPROD * MSGS / NCONS;
    for (uint64_t i = 0; i < NCONS; ++i)
        CHECK(coro_rt_spawn(rt, NULL, consumer, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    CHECK(coro_rt_destroy(rt) == CORO_OK);

    CHECK(g_total == (uint64_t)NPROD * MSGS);
    CHECK(g_arrived_at == (uint64_t)NPROD * MSGS);
    CHECK(g_stray == 0);
    CHECK(coro_rt_channel_size(&g_ch) == 0);
    for (uint64_t i = 0; i < (uint64_t)NPROD * MSGS; ++i) CHECK(g_seen[i] == 1);
    /* Every item was consumed, so the channel is drained and destroyable. */
    CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
}

/* ---- one producer, one consumer, single worker -------------------- */

static uint64_t g_order[MSGS];
static uint64_t g_next;

static void ordered_producer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (uint64_t i = 0; i < MSGS; ++i)
        (void)coro_rt_channel_send(rt, &g_ch, tag_of(i));
}

static void ordered_consumer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (uint64_t i = 0; i < MSGS; ++i) {
        void* v = NULL;
        if (coro_rt_channel_recv(rt, &g_ch, &v) != CORO_OK) return;
        g_order[i] = untag_of(v) - 1u;
    }
}

static void run_fifo_single(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    memset(&g_ch, 0, sizeof g_ch);
    CHECK(coro_rt_channel_init(&g_ch, 0) == CORO_OK);   /* unbounded */
    memset(g_order, 0xff, sizeof g_order);
    g_next = 0;
    CHECK(coro_rt_spawn(rt, NULL, ordered_producer, rt) == CORO_OK);
    CHECK(coro_rt_spawn(rt, NULL, ordered_consumer, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
    for (uint64_t i = 0; i < MSGS; ++i) CHECK(g_order[i] == i);
    CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
}

/* ---- unbounded never blocks a sender ------------------------------- */

static void unbounded_producer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (uint64_t i = 0; i < MSGS; ++i) {
        if (coro_rt_channel_trysend(rt, &g_ch, tag_of(i)) == CORO_OK)
            (void)coro_at_fetch_add(&g_total, 1);
    }
}

static void run_unbounded(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    memset(&g_ch, 0, sizeof g_ch);
    CHECK(coro_rt_channel_init(&g_ch, 0) == CORO_OK);
    g_total = 0;
    CHECK(coro_rt_spawn(rt, NULL, unbounded_producer, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
    CHECK(g_total == MSGS);
    CHECK(coro_rt_channel_size(&g_ch) == MSGS);
    /* Unbounded: nothing blocked the producer. Drain, then destroy. */
    {
        void* v = NULL;
        for (uint64_t i = 0; i < MSGS; ++i)
            CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_OK);
        CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_EBUSY);
        CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
    }
}

/* ---- blocking send released by recv, and vice versa ---------------- */

static uint64_t g_sent, g_got;

static void blocking_sender(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_channel_send(rt, &g_ch, tag_of(7)) == CORO_OK) ++g_sent;
}

static void blocking_receiver(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    void* v = NULL;
    if (coro_rt_channel_recv(rt, &g_ch, &v) == CORO_OK && v) ++g_got;
}

static void run_blocking(void) {
    /* A blocked receiver is woken by the send. */
    {
        coro_rt_t* rt = coro_rt_create(2);
        CHECK(rt != NULL);
        memset(&g_ch, 0, sizeof g_ch);
        CHECK(coro_rt_channel_init(&g_ch, 1) == CORO_OK);
        g_sent = g_got = 0;
        CHECK(coro_rt_spawn(rt, NULL, blocking_receiver, rt) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, blocking_sender, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        CHECK(g_sent == 1);
        CHECK(g_got == 1);
        CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
    }
    /* A blocked sender is woken by the receive. */
    {
        coro_rt_t* rt = coro_rt_create(2);
        CHECK(rt != NULL);
        memset(&g_ch, 0, sizeof g_ch);
        CHECK(coro_rt_channel_init(&g_ch, 1) == CORO_OK);
        CHECK(coro_rt_channel_trysend(rt, &g_ch, tag_of(0)) == CORO_OK);
        CHECK(coro_rt_channel_trysend(rt, &g_ch, tag_of(1)) == CORO_EBUSY);
        g_sent = g_got = 0;
        CHECK(coro_rt_spawn(rt, NULL, blocking_sender, rt) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, blocking_receiver, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        CHECK(g_sent == 1);
        CHECK(g_got == 1);   /* the buffered item, then the sender's item */
        CHECK(coro_rt_channel_size(&g_ch) == 1);
        /* Drain the leftover, then the channel is destroyable. */
        {
            void* v = NULL;
            CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_OK);
            CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
        }
    }
}

/* ---- try paths, close and drain ------------------------------------ */

static coro_status g_send_after_close;
static coro_status g_send_close_seen;
static coro_status g_recv_close_seen;
static int         g_recv_item_seen;
static coro_status g_drain_recv[4];
static uint64_t   g_woke_senders;
static uint64_t   g_woke_receivers;

static void close_reenactor(void* pv) {
    /* Runs on a runtime so it can park; releases parked senders/receivers. */
    coro_rt_t* rt = (coro_rt_t*)pv;
    (void)coro_yield();
    (void)coro_rt_channel_close(rt, &g_ch);
}

static void parked_sender(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    coro_status s = coro_rt_channel_send(rt, &g_ch, tag_of(9));
    g_send_close_seen = s;
    if (s == CORO_OK) ++g_woke_senders;
}

static void parked_receiver(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    void* v = (void*)1;
    coro_status s = coro_rt_channel_recv(rt, &g_ch, &v);
    g_recv_close_seen = s;
    if (s == CORO_OK) g_recv_item_seen = v != NULL;
}

static void run_close(void) {
    /* (a) A buffered item is delivered first; only then does recv report the
       close, while a send is refused outright. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        memset(&g_ch, 0, sizeof g_ch);
        CHECK(coro_rt_channel_init(&g_ch, 2) == CORO_OK);
        CHECK(coro_rt_channel_trysend(rt, &g_ch, tag_of(0)) == CORO_OK);
        CHECK(coro_rt_channel_close(rt, &g_ch) == CORO_OK);
        void* v = NULL;
        CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_OK);  /* drains */
        CHECK(coro_rt_channel_size(&g_ch) == 0);
        g_send_after_close = coro_rt_channel_trysend(rt, &g_ch, tag_of(2));
        CHECK(g_send_after_close == CORO_ECLOSED);       /* send refused */
        g_drain_recv[0] = coro_rt_channel_tryrecv(rt, &g_ch, &v);
        CHECK(g_drain_recv[0] == CORO_ECLOSED);          /* drained then closed */
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
    }
    /* (b) A receiver parked on an empty channel is released by the close. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        memset(&g_ch, 0, sizeof g_ch);
        CHECK(coro_rt_channel_init(&g_ch, 1) == CORO_OK);
        g_woke_receivers = 0;
        CHECK(coro_rt_spawn(rt, NULL, parked_receiver, rt) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, close_reenactor, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        /* The waiter was released and reported the close, not an item. */
        CHECK(g_recv_close_seen == CORO_ECLOSED);
        CHECK(g_recv_item_seen == 0);
        CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
    }
    /* (c) A sender parked on a full channel is released by the close. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        memset(&g_ch, 0, sizeof g_ch);
        CHECK(coro_rt_channel_init(&g_ch, 1) == CORO_OK);
        CHECK(coro_rt_channel_trysend(rt, &g_ch, tag_of(0)) == CORO_OK);
        g_woke_senders = 0;
        CHECK(coro_rt_spawn(rt, NULL, parked_sender, rt) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, close_reenactor, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        /* The waiter was released and refused the send, so nothing was added. */
        CHECK(g_send_close_seen == CORO_ECLOSED);
        CHECK(g_woke_senders == 0);
        CHECK(coro_rt_channel_size(&g_ch) == 1);   /* only the original item */
        void* v = NULL;
        CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_OK);  /* drained */
        CHECK(coro_rt_channel_tryrecv(rt, &g_ch, &v) == CORO_ECLOSED);
        CHECK(coro_rt_channel_destroy(&g_ch) == CORO_OK);
    }
}

/* ---- cancel: no lost slot, no phantom waiter (B-2' / donate) ---------- */

static coro_rt_channel_t g_cx;
static coro_t*         g_cx_victim;
static coro_t*         g_cx_survivor;
static coro_status     g_cx_victim_st;
static coro_status     g_cx_survivor_st;
static uint64_t g_cx_received;
static uint64_t g_cx_sent;
static coro_status g_cx_recv_st0;
static coro_status g_cx_recv_st1;
static uint64_t        g_cx_untagged;

/* One of two parked senders. The victim is cancelled while it is parked;
   the survivor must still deliver its item. */
static void cx_victim(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    g_cx_victim_st = coro_rt_channel_send(rt, &g_cx, tag_of(2));
    if (g_cx_victim_st == CORO_OK) ++g_cx_sent;
}

static void cx_survivor(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    g_cx_survivor_st = coro_rt_channel_send(rt, &g_cx, tag_of(4));
    if (g_cx_survivor_st == CORO_OK) ++g_cx_sent;
}

/* Takes exactly one item, freeing exactly one slot for one parked sender. */
static void cx_receiver(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    void* v = NULL;
    coro_status s = coro_rt_channel_recv(rt, &g_cx, &v);
    g_cx_recv_st0 = s;
    if (s == CORO_OK) ++g_cx_received;
    if (s == CORO_OK && !v) ++g_cx_untagged;
}

/* Drives everything from one coroutine on a single worker, so the observed
   order is deterministic: it fills the channel, parks both senders, cancels
   the victim, then frees one slot for the survivor. */
static void cx_controller(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_channel_trysend(rt, &g_cx, tag_of(0)) != CORO_OK) return;
    ++g_cx_sent;                                  /* the channel is now full */
    if (coro_rt_spawn_h(rt, NULL, cx_victim, rt, &g_cx_victim) != CORO_OK) return;
    (void)coro_yield();          /* the victim parks: the channel is full */
    if (coro_rt_spawn_h(rt, NULL, cx_survivor, rt, &g_cx_survivor) != CORO_OK) return;
    (void)coro_yield();          /* the survivor parks right behind it */
    /* Cancel the parked victim: the request is latched, so it is observed when
       the victim is next resumed -- at which point it retries its own send
       instead of abandoning a slot it was handed (the retry-first rule). */
    (void)coro_rt_cancel(rt, g_cx_victim);
    if (coro_rt_spawn(rt, NULL, cx_receiver, rt) != CORO_OK) return;
    (void)coro_yield();          /* the receiver runs: frees one slot */
    /* Free the next slot for the survivor: the cancelled sender abstains and
       the one behind it still gets its item through. */
    {
        void* v = NULL;
        if (coro_rt_channel_tryrecv(rt, &g_cx, &v) == CORO_OK) ++g_cx_received;
    }
    (void)coro_yield();
    (void)coro_yield();
}

/* Frees one slot (so exactly one parked sender can complete), then takes the
   next one, so each parked sender is eventually resumed. */
static void run_cancel(void) {
    coro_rt_t* rt = coro_rt_create(1);   /* single worker: strict ordering */
    CHECK(rt != NULL);
    memset(&g_cx, 0, sizeof g_cx);
    CHECK(coro_rt_channel_init(&g_cx, 1) == CORO_OK);
    g_cx_victim = NULL; g_cx_survivor = NULL;
    g_cx_victim_st = g_cx_survivor_st = CORO_EBUSY;
    g_cx_received = g_cx_sent = g_cx_untagged = 0;
    g_cx_recv_st0 = g_cx_recv_st1 = CORO_OK;

    CHECK(coro_rt_spawn(rt, NULL, cx_controller, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    CHECK(coro_rt_destroy(rt) == CORO_OK);

    /* The cancelled sender either completed its own send (retry-first) or
       abstained, and either way the sender behind it still got its item
       through: no grant was stranded and no waiter was dropped. */
    CHECK(g_cx_untagged == 0);
    CHECK(g_cx_sent >= 2);
    CHECK(g_cx_survivor_st == CORO_OK);
    CHECK(g_cx_victim_st == CORO_OK || g_cx_victim_st == CORO_ECANCELED);
    /* Conservation: everything sent is received or still buffered. */
    {
        uint64_t left = coro_rt_channel_size(&g_cx);
        CHECK(g_cx_received + left == g_cx_sent);
        /* No more than the configured capacity is ever buffered. */
        CHECK(left <= 1);
        void* rest = NULL;
        while (coro_rt_channel_size(&g_cx) > 0)
            (void)coro_rt_channel_tryrecv(rt, &g_cx, &rest);
        CHECK(coro_rt_channel_size(&g_cx) == 0);
    }
    CHECK(coro_rt_channel_destroy(&g_cx) == CORO_OK);
}


/* ---- destroy refuses while busy, and is idempotent ------------------ */

static void run_destroy(void) {
    /* A queued value keeps destroy refused; drain it and destroy succeeds. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_channel_t ch;
        memset(&ch, 0, sizeof ch);
        CHECK(coro_rt_channel_init(&ch, 4) == CORO_OK);
        CHECK(coro_rt_channel_trysend(NULL, &ch, NULL) == CORO_EINVAL);
        CHECK(coro_rt_channel_trysend(rt, &ch, tag_of(1)) == CORO_OK);
        CHECK(coro_rt_channel_destroy(&ch) == CORO_EBUSY);   /* len > 0 */
        void* v = NULL;
        CHECK(coro_rt_channel_tryrecv(rt, &ch, &v) == CORO_OK);
        CHECK(coro_rt_channel_destroy(&ch) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }
    {
        /* A zero-initialized channel is unbounded, holds without blocking and
           refuses destroy only while something is queued. */
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_channel_t ch;
        memset(&ch, 0, sizeof ch);
        CHECK(coro_rt_channel_size(&ch) == 0);
        CHECK(coro_rt_channel_destroy(&ch) == CORO_OK);      /* nothing in it */
        CHECK(coro_rt_channel_destroy(NULL) == CORO_EINVAL);  /* NULL channel */
        /* The NULL guards hold without any runtime. */
        CHECK(coro_rt_channel_send(NULL, &ch, NULL) == CORO_EINVAL);
        CHECK(coro_rt_channel_recv(NULL, &ch, NULL) == CORO_EINVAL);
        CHECK(coro_rt_channel_close(NULL, &ch) == CORO_EINVAL);
        /* The blocking entry points need a runtime coroutine. */
        CHECK(coro_rt_channel_send(rt, &ch, tag_of(1)) == CORO_ESTALE);
        CHECK(coro_rt_channel_recv(rt, &ch, NULL) == CORO_ESTALE);
        /* An unbounded channel holds, then refuses destroy while queued. */
        CHECK(coro_rt_channel_trysend(rt, &ch, tag_of(1)) == CORO_OK);
        CHECK(coro_rt_channel_trysend(rt, &ch, tag_of(2)) == CORO_OK);
        CHECK(coro_rt_channel_destroy(&ch) == CORO_EBUSY);
        CHECK(coro_rt_channel_close(rt, &ch) == CORO_OK);
        void* v = NULL;
        CHECK(coro_rt_channel_tryrecv(rt, &ch, &v) == CORO_OK);  /* drains first */
        CHECK(coro_rt_channel_tryrecv(rt, &ch, &v) == CORO_OK);
        CHECK(coro_rt_channel_tryrecv(rt, &ch, &v) == CORO_ECLOSED);
        CHECK(coro_rt_channel_destroy(&ch) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }
}

static void run_tests(void) {
    /* ---- accessors on a zero-initialized channel ---- */
    {
        coro_rt_channel_t ch;
        memset(&ch, 0, sizeof ch);
        CHECK(coro_rt_channel_closed(&ch) == 0);
        CHECK(coro_rt_channel_size(&ch) == 0);
        CHECK(coro_rt_channel_closed(NULL) == 0);
        CHECK(coro_rt_channel_size(NULL) == 0);
    }

    run_bounded();
    run_fifo_single();
    run_unbounded();
    run_blocking();
    run_close();
    run_cancel();
    run_destroy();
}

TEST_MAIN()
