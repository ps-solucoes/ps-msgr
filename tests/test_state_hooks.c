/* SPDX-License-Identifier: Apache-2.0 */
/* State channel tests that need library internals (src/internal.h). They
 * link the static library: the hooks are not exported from the .so. */
#include "internal.h"
#include "state_util.h"

static void generation_wraps_skipping_zero(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmi_writer_set_generation(w, UINT32_MAX - 1);

    uint32_t gen;
    psmsgr_state_info info;
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    assert_uint_equal(gen, UINT32_MAX - 1);
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    assert_uint_equal(gen, UINT32_MAX);
    assert_rc(psmsgr_state_peek(r, &info), PSMSGR_OK);
    assert_uint_equal(info.generation, UINT32_MAX);
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    assert_uint_equal(gen, 1);
    assert_rc(psmsgr_state_peek(r, &info), PSMSGR_OK);
    assert_uint_equal(info.generation, 1);

    /* The skip also applies when the generation is carried over. */
    psmi_writer_set_generation(w, UINT32_MAX);
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    assert_uint_equal(gen, UINT32_MAX);
    psmsgr_state_writer_close(w);
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    assert_uint_equal(gen, 1);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- lock identity (state-channel.md §4) --------------------------------------- */

static psmsgr_state_writer *hook_a, *hook_c;
static int hook_unlink_rc, hook_c_rc;

/* Runs inside B's open, after B opened the lock file and before it locks
 * it: A goes away, the channel is unlinked, and C starts a fresh one. */
static void unlink_and_restart(void)
{
    psmi_test_lock_opened = NULL; /* one shot; the calls below lock too */
    psmsgr_state_writer_close(hook_a);
    hook_unlink_rc = psmsgr_state_unlink(CHAN, test_dir);
    hook_c_rc = open_writer(CHAN, 8, 2, 0, &hook_c);
}

static void unlink_racing_open_never_gives_two_writers(void **state)
{
    assert_rc(open_writer(CHAN, 8, 2, 0, &hook_a), PSMSGR_OK);
    psmi_test_lock_opened = unlink_and_restart;
    psmsgr_state_writer *b = NULL;
    int rc = open_writer(CHAN, 8, 2, 0, &b);
    psmi_test_lock_opened = NULL;

    assert_rc(hook_unlink_rc, PSMSGR_OK);
    assert_rc(hook_c_rc, PSMSGR_OK);
    /* B locked the orphaned inode, noticed, and found C's lock. */
    assert_rc(rc, PSMSGR_E_WRITER_EXISTS);
    assert_null(b);
    psmsgr_state_writer_close(b);
    psmsgr_state_writer_close(hook_c);
}

static int held_fd = -1;

/* An unlink in progress: holds the old lock inode and has removed its path. */
static void hold_and_remove_lock(void)
{
    psmi_test_lock_opened = NULL;
    held_fd = open(lock_path(CHAN), O_RDWR | O_CLOEXEC);
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (held_fd < 0 || fcntl(held_fd, F_OFD_SETLK, &fl) != 0 || unlink(lock_path(CHAN)) != 0)
        fprintf(stderr, "hold_and_remove_lock: %s\n", strerror(errno));
}

static void lock_held_on_old_inode_is_not_a_writer(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 8, 2, 0, &w), PSMSGR_OK);
    psmsgr_state_writer_close(w);

    psmi_test_lock_opened = hold_and_remove_lock;
    int rc = open_writer(CHAN, 8, 2, 0, &w); /* retried on a fresh lock file */
    psmi_test_lock_opened = NULL;
    assert_rc(rc, PSMSGR_OK);
    assert_true(held_fd >= 0);

    psmsgr_state_reader *r = open_reader(CHAN);
    assert_int_equal(psmsgr_state_writer_alive(r), 1);
    psmsgr_state_writer_close(w);
    assert_int_equal(psmsgr_state_writer_alive(r), 0);
    psmsgr_state_reader_close(r);
    close(held_fd);
}

/* ---- waitset ------------------------------------------------------------------ */

static void waitset_needs_futex_waitv(void **state)
{
    psmsgr_waitset *ws = (psmsgr_waitset *)1;
    psmi_test_waitv_enosys = true;
    int rc = psmsgr_waitset_open(&ws);
    psmi_test_waitv_enosys = false;
    assert_rc(rc, PSMSGR_E_NOTSUP);
    assert_null(ws);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        TEST(generation_wraps_skipping_zero),
        TEST(unlink_racing_open_never_gives_two_writers),
        TEST(lock_held_on_old_inode_is_not_a_writer),
        TEST(waitset_needs_futex_waitv),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
