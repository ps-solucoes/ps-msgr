/* SPDX-License-Identifier: Apache-2.0 */
/* Waitset unit tests (spec/c-api.md, state-channel.md §6.7), through the
 * public API of the shared library. Without futex_waitv (qemu-user, Linux <
 * 5.16) psmsgr_waitset_open returns NOTSUP and the tests skip. */
#include "state_util.h"

#include <pthread.h>
#include <stdbool.h>

/* Opens a waitset into `ws`, or skips the test where there is none. */
#define OPEN_SET(ws)                                              \
    do {                                                          \
        int open_rc = psmsgr_waitset_open(&(ws));                 \
        if (open_rc == PSMSGR_E_NOTSUP) {                         \
            print_message("futex_waitv is not available here\n"); \
            skip();                                               \
        }                                                         \
        assert_rc(open_rc, PSMSGR_OK);                            \
    } while (0)

/* A thread blocked in psmsgr_waitset_wait. As with the wait tests, nothing is
 * asserted between set_waiter_start and set_waiter_join. */
typedef struct set_waiter {
    pthread_t thread;
    psmsgr_waitset *ws;
    int32_t timeout_ms;
    psmsgr_waitset_event ev[8];
    uint32_t n;
    int rc;
    int done; /* atomic */
} set_waiter;

static void *set_waiter_main(void *arg)
{
    set_waiter *wt = arg;
    do /* STATE: the test thread was in a wait of its own */
        wt->rc = psmsgr_waitset_wait(wt->ws, wt->timeout_ms, wt->ev, 8, &wt->n);
    while (wt->rc == PSMSGR_E_STATE);
    __atomic_store_n(&wt->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int set_waiter_start(set_waiter *wt, psmsgr_waitset *ws, int32_t timeout_ms)
{
    *wt = (set_waiter){ .ws = ws, .timeout_ms = timeout_ms };
    return pthread_create(&wt->thread, NULL, set_waiter_main, wt);
}

static int set_waiter_join(set_waiter *wt)
{
    pthread_join(wt->thread, NULL);
    return wt->rc;
}

static bool set_waiter_done(set_waiter *wt)
{
    return __atomic_load_n(&wt->done, __ATOMIC_ACQUIRE) != 0;
}

/* Waits up to timeout_ms for one event and returns it. */
static psmsgr_waitset_event wait_one(psmsgr_waitset *ws, int32_t timeout_ms)
{
    psmsgr_waitset_event ev;
    uint32_t n = 0;
    assert_rc(psmsgr_waitset_wait(ws, timeout_ms, &ev, 1, &n), PSMSGR_OK);
    assert_uint_equal(n, 1);
    return ev;
}

/* Asserts that the waiter returned exactly one event, for `token`. */
static void check_one(const set_waiter *wt, uint64_t token)
{
    assert_rc(wt->rc, PSMSGR_OK);
    assert_uint_equal(wt->n, 1);
    assert_uint_equal(wt->ev[0].token, token);
    assert_rc(wt->ev[0].status, PSMSGR_OK);
}

/* Channel "c<i>" with one value published, and a reader of it. */
static void open_channel(int i, psmsgr_state_writer **w, psmsgr_state_reader **r, uint32_t *gen)
{
    char name[8];
    snprintf(name, sizeof name, "c%d", i);
    assert_rc(open_writer(name, 8, 2, 0, w), PSMSGR_OK);
    assert_rc(publish_str(*w, "a", gen), PSMSGR_OK);
    assert_non_null(*r = open_reader(name));
}

static void waitset_arguments(void **state)
{
    assert_rc(psmsgr_waitset_open(NULL), PSMSGR_E_INVAL);
    psmsgr_waitset_close(NULL);
    psmsgr_waitset_wake(NULL);

    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset_event ev;
    uint32_t n = 7;
    assert_rc(psmsgr_waitset_add(NULL, r, 0, 0), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_add(ws, NULL, 0, 0), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_remove(NULL, r), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_remove(ws, NULL), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_wait(NULL, 0, &ev, 1, &n), PSMSGR_E_INVAL);
    assert_uint_equal(n, 0);
    assert_rc(psmsgr_waitset_wait(ws, 0, NULL, 1, &n), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 0, &n), PSMSGR_E_INVAL);
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, NULL), PSMSGR_E_INVAL);

    /* An empty set times out like any other. */
    n = 7;
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_uint_equal(n, 0);
    uint64_t t0 = psmsgr_now_ns();
    assert_rc(psmsgr_waitset_wait(ws, 50, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_true(elapsed_ms(t0) >= 50);
    psmsgr_waitset_close(ws);
    psmsgr_state_reader_close(r);
}

static void waitset_add_and_remove(void **state)
{
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    psmsgr_state_reader *r[PSMSGR_WAITSET_MAX + 1];
    for (int i = 0; i <= PSMSGR_WAITSET_MAX; ++i)
        assert_non_null(r[i] = open_reader("absent"));

    for (int i = 0; i < PSMSGR_WAITSET_MAX; ++i)
        assert_rc(psmsgr_waitset_add(ws, r[i], 0, (uint64_t)i), PSMSGR_OK);
    assert_rc(psmsgr_waitset_add(ws, r[0], 0, 0), PSMSGR_E_STATE);
    assert_rc(psmsgr_waitset_add(ws, r[PSMSGR_WAITSET_MAX], 0, 0), PSMSGR_E_TOOBIG);

    assert_rc(psmsgr_waitset_remove(ws, r[3]), PSMSGR_OK);
    assert_rc(psmsgr_waitset_remove(ws, r[3]), PSMSGR_E_NODATA);
    assert_rc(psmsgr_waitset_remove(ws, r[PSMSGR_WAITSET_MAX]), PSMSGR_E_NODATA);
    assert_rc(psmsgr_waitset_add(ws, r[PSMSGR_WAITSET_MAX], 0, 0), PSMSGR_OK);
    assert_rc(psmsgr_waitset_add(ws, r[3], 0, 0), PSMSGR_E_TOOBIG);

    /* Unattached readers are pending, not reported. */
    psmsgr_waitset_event ev;
    uint32_t n;
    assert_rc(psmsgr_waitset_wait(ws, 20, &ev, 1, &n), PSMSGR_E_TIMEOUT);

    /* Closing the set leaves the readers to the caller. */
    psmsgr_waitset_close(ws);
    for (int i = 0; i <= PSMSGR_WAITSET_MAX; ++i)
        psmsgr_state_reader_close(r[i]);
}

static void waitset_reports_once(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    psmsgr_waitset_event ev;
    uint32_t n, gen;

    /* Nothing published: NODATA counts as unchanged, also for 0. */
    assert_rc(psmsgr_waitset_add(ws, r, 0, 7), PSMSGR_OK);
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    ev = wait_one(ws, 0);
    assert_uint_equal(ev.token, 7);
    assert_rc(ev.status, PSMSGR_OK);
    assert_uint_equal(ev.generation, gen);
    assert_int_equal(ev.sys_errno, 0);
    assert_uint_equal(ev.reserved, 0);

    /* Reported means unregistered. */
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_rc(psmsgr_waitset_remove(ws, r), PSMSGR_E_NODATA);

    assert_rc(psmsgr_waitset_add(ws, r, gen, 8), PSMSGR_OK);
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_rc(publish_str(w, "b", NULL), PSMSGR_OK);
    ev = wait_one(ws, 0);
    assert_uint_equal(ev.token, 8);
    assert_uint_equal(ev.generation, gen_after(gen, 1));

    /* The reader is the caller's again. */
    psmsgr_state_info info;
    assert_rc(psmsgr_state_peek(r, &info), PSMSGR_OK);
    assert_uint_equal(info.generation, gen_after(gen, 1));
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void waitset_cap_and_order(void **state)
{
    enum { N = 5 };
    psmsgr_state_writer *w[N];
    psmsgr_state_reader *r[N];
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    for (int i = 0; i < N; ++i) {
        uint32_t gen;
        open_channel(i, &w[i], &r[i], &gen);
        assert_rc(psmsgr_waitset_add(ws, r[i], 0, (uint64_t)i), PSMSGR_OK);
    }

    /* In registration order, cap at a time; the rest stay registered. */
    psmsgr_waitset_event ev[2];
    uint32_t n;
    uint64_t next = 0;
    const uint32_t batches[] = { 2, 2, 1 };
    for (int b = 0; b < 3; ++b) {
        assert_rc(psmsgr_waitset_wait(ws, 0, ev, 2, &n), PSMSGR_OK);
        assert_uint_equal(n, batches[b]);
        for (uint32_t i = 0; i < n; ++i)
            assert_uint_equal(ev[i].token, next++);
    }
    assert_rc(psmsgr_waitset_wait(ws, 0, ev, 2, &n), PSMSGR_E_TIMEOUT);
    psmsgr_waitset_close(ws);
    for (int i = 0; i < N; ++i) {
        psmsgr_state_writer_close(w[i]);
        psmsgr_state_reader_close(r[i]);
    }
}

static void waitset_wakes_on_publish(void **state)
{
    enum { N = 3 };
    psmsgr_state_writer *w[N];
    psmsgr_state_reader *r[N];
    uint32_t gen[N];
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    for (int i = 0; i < N; ++i) {
        open_channel(i, &w[i], &r[i], &gen[i]);
        assert_rc(psmsgr_waitset_add(ws, r[i], gen[i], (uint64_t)i), PSMSGR_OK);
    }

    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50); /* likely blocked by now; the test holds either way */
    int rc_pub = publish_str(w[1], "b", NULL);
    set_waiter_join(&wt);
    assert_rc(rc_pub, PSMSGR_OK);
    check_one(&wt, 1);
    assert_uint_equal(wt.ev[0].generation, gen_after(gen[1], 1));

    assert_rc(psmsgr_waitset_remove(ws, r[0]), PSMSGR_OK);
    assert_rc(psmsgr_waitset_remove(ws, r[1]), PSMSGR_E_NODATA);
    assert_rc(psmsgr_waitset_remove(ws, r[2]), PSMSGR_OK);
    psmsgr_waitset_close(ws);
    for (int i = 0; i < N; ++i) {
        psmsgr_state_writer_close(w[i]);
        psmsgr_state_reader_close(r[i]);
    }
}

static void waitset_add_while_blocked(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);

    /* A reader that is ready already, added to an empty set being waited on. */
    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    int rc_add = psmsgr_waitset_add(ws, r, 0, 1);
    set_waiter_join(&wt);
    assert_rc(rc_add, PSMSGR_OK);
    check_one(&wt, 1);

    /* One that is not: the waiter must now sleep on its futex too. */
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    rc_add = psmsgr_waitset_add(ws, r, gen, 2);
    sleep_ms(50);
    int early = set_waiter_done(&wt);
    int rc_pub = publish_str(w, "b", NULL);
    set_waiter_join(&wt);
    assert_rc(rc_add, PSMSGR_OK);
    assert_int_equal(early, 0);
    assert_rc(rc_pub, PSMSGR_OK);
    check_one(&wt, 2);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void waitset_remove_while_blocked(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    assert_rc(psmsgr_waitset_add(ws, r, gen, 1), PSMSGR_OK);

    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    /* The reader may be closed, and its mapping gone, as soon as remove returns. */
    int rc_remove = psmsgr_waitset_remove(ws, r);
    psmsgr_state_reader_close(r);
    int rc_pub = publish_str(w, "b", NULL);
    sleep_ms(50);
    int early = set_waiter_done(&wt);
    psmsgr_waitset_wake(ws);
    int rc_wait = set_waiter_join(&wt);
    assert_rc(rc_remove, PSMSGR_OK);
    assert_rc(rc_pub, PSMSGR_OK);
    assert_int_equal(early, 0);
    assert_rc(rc_wait, PSMSGR_OK);
    assert_uint_equal(wt.n, 0);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
}

static void waitset_wake(void **state)
{
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    psmsgr_waitset_event ev;
    uint32_t n = 7;

    /* Sticky: a wake with nobody waiting ends the next wait, once. */
    psmsgr_waitset_wake(ws);
    psmsgr_waitset_wake(ws);
    assert_rc(psmsgr_waitset_wait(ws, -1, &ev, 1, &n), PSMSGR_OK);
    assert_uint_equal(n, 0);
    assert_rc(psmsgr_waitset_wait(ws, 0, &ev, 1, &n), PSMSGR_E_TIMEOUT);

    /* One waiting thread at a time; a wake from another thread ends its wait. */
    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, -1), 0);
    /* TIMEOUT until the thread is in wait: then STATE. */
    int rc_second = PSMSGR_E_TIMEOUT;
    for (int i = 0; i < 500 && rc_second == PSMSGR_E_TIMEOUT; ++i) {
        sleep_ms(10);
        rc_second = psmsgr_waitset_wait(ws, 0, &ev, 1, &n);
    }
    psmsgr_waitset_wake(ws);
    int rc_wait = set_waiter_join(&wt);
    assert_rc(rc_second, PSMSGR_E_STATE);
    assert_rc(rc_wait, PSMSGR_OK);
    assert_uint_equal(wt.n, 0);
    psmsgr_waitset_close(ws);
}

static void on_signal(int sig)
{
    (void)sig;
}

static void waitset_timeouts_and_signals(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_reader *none = open_reader("absent");
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    psmsgr_waitset_event ev;
    uint32_t n;

    /* Lower bounds only, as for psmsgr_state_wait. */
    assert_rc(psmsgr_waitset_add(ws, r, gen, 1), PSMSGR_OK);
    uint64_t t0 = psmsgr_now_ns();
    assert_rc(psmsgr_waitset_wait(ws, 120, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_true(elapsed_ms(t0) >= 120);
    assert_rc(psmsgr_waitset_add(ws, none, 0, 2), PSMSGR_OK);
    t0 = psmsgr_now_ns();
    assert_rc(psmsgr_waitset_wait(ws, 50, &ev, 1, &n), PSMSGR_E_TIMEOUT);
    assert_true(elapsed_ms(t0) >= 50);

    struct sigaction sa = { .sa_handler = on_signal }, old; /* no SA_RESTART */
    sigemptyset(&sa.sa_mask);
    assert_int_equal(sigaction(SIGUSR1, &sa, &old), 0);
    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, -1), 0);
    /* Repeat: a signal that lands before the thread blocks is not an error. */
    for (int i = 0; i < 500 && !set_waiter_done(&wt); ++i) {
        sleep_ms(10);
        pthread_kill(wt.thread, SIGUSR1);
    }
    if (!set_waiter_done(&wt))
        psmsgr_waitset_wake(ws);
    int rc_wait = set_waiter_join(&wt);
    sigaction(SIGUSR1, &old, NULL);
    assert_rc(rc_wait, PSMSGR_E_INTR);
    assert_uint_equal(wt.n, 0);

    /* Still registered after the interruption. */
    assert_rc(psmsgr_waitset_remove(ws, r), PSMSGR_OK);
    assert_rc(psmsgr_waitset_remove(ws, none), PSMSGR_OK);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
    psmsgr_state_reader_close(none);
}

static void waitset_per_entry_results(void **state)
{
    psmsgr_state_writer *quiet = NULL, *nonotify = NULL;
    assert_rc(open_writer("quiet", 8, 2, 0, &quiet), PSMSGR_OK);
    assert_rc(open_writer("nonotify", 8, 2, PSMSGR_STATE_NO_NOTIFY, &nonotify), PSMSGR_OK);
    assert_rc(publish_str(nonotify, "a", NULL), PSMSGR_OK);
    FILE *f = fopen(data_path("junk"), "w");
    assert_non_null(f);
    for (int i = 0; i < 4096; ++i)
        fputc(0xA5, f);
    assert_int_equal(fclose(f), 0);

    psmsgr_state_reader *r[3] = { open_reader("quiet"), open_reader("nonotify"),
                                  open_reader("junk") };
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    for (int i = 0; i < 3; ++i)
        assert_rc(psmsgr_waitset_add(ws, r[i], 0, (uint64_t)i), PSMSGR_OK);

    /* One wait reports both failures; the healthy reader stays registered. */
    psmsgr_waitset_event ev[4];
    uint32_t n;
    assert_rc(psmsgr_waitset_wait(ws, 0, ev, 4, &n), PSMSGR_OK);
    assert_uint_equal(n, 2);
    assert_uint_equal(ev[0].token, 1);
    assert_rc(ev[0].status, PSMSGR_E_NOTSUP);
    assert_uint_equal(ev[1].token, 2);
    assert_rc(ev[1].status, PSMSGR_E_FORMAT);
    assert_uint_equal(ev[1].generation, 0);
    assert_rc(psmsgr_waitset_wait(ws, 0, ev, 4, &n), PSMSGR_E_TIMEOUT);
    assert_rc(psmsgr_waitset_remove(ws, r[0]), PSMSGR_OK);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(quiet);
    psmsgr_state_writer_close(nonotify);
    for (int i = 0; i < 3; ++i)
        psmsgr_state_reader_close(r[i]);
}

static void waitset_follows_attach_and_retire(void **state)
{
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    assert_rc(psmsgr_waitset_add(ws, r, 0, 1), PSMSGR_OK);

    /* Unattached: the channel appears while the set is being waited on. */
    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    psmsgr_state_writer *w = NULL;
    int rc_open = open_writer(CHAN, 8, 2, 0, &w);
    uint32_t gen = 0;
    int rc_pub = rc_open == PSMSGR_OK ? publish_str(w, "a", &gen) : rc_open;
    set_waiter_join(&wt);
    assert_rc(rc_open, PSMSGR_OK);
    assert_rc(rc_pub, PSMSGR_OK);
    check_one(&wt, 1);
    assert_uint_equal(wt.ev[0].generation, gen);

    /* A retire alone is not reported; the replacement's first value is. */
    psmsgr_state_writer_close(w);
    assert_rc(psmsgr_waitset_add(ws, r, gen, 2), PSMSGR_OK);
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    rc_open = open_writer(CHAN, 16, 2, PSMSGR_STATE_RECREATE, &w);
    sleep_ms(50);
    int early = set_waiter_done(&wt);
    uint32_t new_gen = 0;
    rc_pub = rc_open == PSMSGR_OK ? publish_str(w, "b", &new_gen) : rc_open;
    set_waiter_join(&wt);
    assert_rc(rc_open, PSMSGR_OK);
    assert_int_equal(early, 0);
    assert_rc(rc_pub, PSMSGR_OK);
    check_one(&wt, 2);
    assert_uint_equal(wt.ev[0].generation, new_gen);

    psmsgr_state_desc d;
    assert_rc(psmsgr_state_describe(r, &d), PSMSGR_OK);
    assert_uint_equal(d.capacity, 16);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void waitset_orphaned_file(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "old", &gen), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    assert_rc(psmsgr_waitset_add(ws, r, 0, 1), PSMSGR_OK);
    assert_uint_equal(wait_one(ws, 0).generation, gen); /* attached */
    psmsgr_state_writer_close(w);

    /* Deleted behind the library's back: nothing wakes the old futex. */
    assert_int_equal(unlink(data_path(CHAN)), 0);
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t new_gen;
    assert_rc(publish_str(w, "new", &new_gen), PSMSGR_OK);
    assert_rc(psmsgr_waitset_add(ws, r, gen, 2), PSMSGR_OK);
    psmsgr_waitset_event ev = wait_one(ws, 5000); /* at its 1 s identity check */
    assert_uint_equal(ev.token, 2);
    assert_uint_equal(ev.generation, new_gen);
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void waitset_same_channel_twice(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    psmsgr_state_reader *a = open_reader(CHAN), *b = open_reader(CHAN);
    psmsgr_waitset *ws = NULL;
    OPEN_SET(ws);
    assert_rc(psmsgr_waitset_add(ws, a, gen, 1), PSMSGR_OK);
    assert_rc(psmsgr_waitset_add(ws, b, gen, 2), PSMSGR_OK);

    set_waiter wt;
    assert_int_equal(set_waiter_start(&wt, ws, 10000), 0);
    sleep_ms(50);
    int rc_pub = publish_str(w, "b", NULL);
    int rc_wait = set_waiter_join(&wt);
    assert_rc(rc_pub, PSMSGR_OK);
    assert_rc(rc_wait, PSMSGR_OK);
    assert_uint_equal(wt.n, 2); /* one futex word: one wake-up, one scan */
    psmsgr_waitset_close(ws);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(a);
    psmsgr_state_reader_close(b);
}

/* ---- concurrency -------------------------------------------------------------- */

enum { STRESS_N = 2000 };

typedef struct stress {
    psmsgr_waitset *ws;
    int stop;               /* atomic */
    int reported[STRESS_N]; /* atomic: events per token */
    int bad;                /* atomic: unexpected results */
} stress;

static void *stress_waiter(void *arg)
{
    stress *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        psmsgr_waitset_event ev[8];
        uint32_t n = 0;
        int rc = psmsgr_waitset_wait(s->ws, 50, ev, 8, &n);
        if (rc != PSMSGR_OK && rc != PSMSGR_E_TIMEOUT)
            __atomic_fetch_add(&s->bad, 1, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < n; ++i) {
            if (ev[i].status != PSMSGR_OK || ev[i].token >= STRESS_N)
                __atomic_fetch_add(&s->bad, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&s->reported[ev[i].token], 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

/* Every registration ends exactly once: in a successful remove or in one
 * event. Readers are closed right after either, while the waiter runs, which
 * is what ASan and TSan check. */
static void waitset_add_remove_race(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "a", &gen), PSMSGR_OK);
    static stress s; /* zeroed: one run per process */
    OPEN_SET(s.ws);
    pthread_t thread;
    assert_int_equal(pthread_create(&thread, NULL, stress_waiter, &s), 0);

    static bool removed[STRESS_N]; /* zeroed: one run per process */
    int failures = 0;
    for (int i = 0; i < STRESS_N; ++i) {
        psmsgr_state_reader *r = open_reader(CHAN);
        failures += r == NULL || psmsgr_waitset_add(s.ws, r, gen, (uint64_t)i) != PSMSGR_OK;
        if (i % 3 == 0)
            failures += publish_str(w, "x", &gen) != PSMSGR_OK;
        if (i % 5 == 0)
            sched_yield();
        int rc = psmsgr_waitset_remove(s.ws, r);
        removed[i] = rc == PSMSGR_OK;
        failures += rc != PSMSGR_OK && rc != PSMSGR_E_NODATA;
        psmsgr_state_reader_close(r);
    }
    __atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
    psmsgr_waitset_wake(s.ws);
    pthread_join(thread, NULL);

    assert_int_equal(failures, 0);
    assert_int_equal(s.bad, 0);
    int events = 0;
    for (int i = 0; i < STRESS_N; ++i) {
        assert_int_equal(removed[i] + s.reported[i], 1);
        events += s.reported[i];
    }
    print_message("%d of %d registrations reported, the rest removed\n", events, STRESS_N);
    psmsgr_waitset_close(s.ws);
    psmsgr_state_writer_close(w);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        TEST(waitset_arguments),
        TEST(waitset_add_and_remove),
        TEST(waitset_reports_once),
        TEST(waitset_cap_and_order),
        TEST(waitset_wakes_on_publish),
        TEST(waitset_add_while_blocked),
        TEST(waitset_remove_while_blocked),
        TEST(waitset_wake),
        TEST(waitset_timeouts_and_signals),
        TEST(waitset_per_entry_results),
        TEST(waitset_follows_attach_and_retire),
        TEST(waitset_orphaned_file),
        TEST(waitset_same_channel_twice),
        TEST(waitset_add_remove_race),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
