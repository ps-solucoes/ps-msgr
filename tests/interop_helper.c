/* SPDX-License-Identifier: Apache-2.0 */
/* The C side of the binding tests and the C agent of the interop suite; not
 * run by CTest.
 *
 *   interop_helper layout
 *     Prints the public struct layouts and constants, one "kind key value"
 *     per line, so that the bindings compare their mirrors with this header.
 *   interop_helper payload-layout | write | read | wait | alive ...
 *   interop_helper
 *     The interop agent protocol (interop/README.md): one command from the
 *     arguments, or without arguments one command per line of stdin.
 */
#define _POSIX_C_SOURCE 200809L

#include <psmsgr/psmsgr.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE(type)          printf("sizeof %s %zu\n", #type, sizeof(type))
#define OFFSET(type, field) printf("offsetof %s.%s %zu\n", #type, #field, offsetof(type, field))
#define CONST(name)         printf("const %s %lld\n", #name, (long long)(name))

static int layout(void)
{
    SIZE(psmsgr_state_options);
    OFFSET(psmsgr_state_options, struct_size);
    OFFSET(psmsgr_state_options, capacity);
    OFFSET(psmsgr_state_options, slot_count);
    OFFSET(psmsgr_state_options, payload_type);
    OFFSET(psmsgr_state_options, mode);
    OFFSET(psmsgr_state_options, flags);
    OFFSET(psmsgr_state_options, dir);
    SIZE(psmsgr_state_info);
    OFFSET(psmsgr_state_info, generation);
    OFFSET(psmsgr_state_info, length);
    OFFSET(psmsgr_state_info, timestamp_ns);
    OFFSET(psmsgr_state_info, flags);
    OFFSET(psmsgr_state_info, reserved);
    SIZE(psmsgr_state_desc);
    OFFSET(psmsgr_state_desc, capacity);
    OFFSET(psmsgr_state_desc, slot_count);
    OFFSET(psmsgr_state_desc, payload_type);
    OFFSET(psmsgr_state_desc, flags);
    SIZE(psmsgr_waitset_event);
    OFFSET(psmsgr_waitset_event, token);
    OFFSET(psmsgr_waitset_event, status);
    OFFSET(psmsgr_waitset_event, generation);
    OFFSET(psmsgr_waitset_event, sys_errno);
    OFFSET(psmsgr_waitset_event, reserved);

    CONST(PSMSGR_VERSION_MAJOR);
    CONST(PSMSGR_VERSION_MINOR);
    CONST(PSMSGR_OK);
    CONST(PSMSGR_E_INVAL);
    CONST(PSMSGR_E_SYS);
    CONST(PSMSGR_E_NODATA);
    CONST(PSMSGR_E_TOOSMALL);
    CONST(PSMSGR_E_TOOBIG);
    CONST(PSMSGR_E_BUSY);
    CONST(PSMSGR_E_TIMEOUT);
    CONST(PSMSGR_E_INTR);
    CONST(PSMSGR_E_WRITER_EXISTS);
    CONST(PSMSGR_E_MISMATCH);
    CONST(PSMSGR_E_FORMAT);
    CONST(PSMSGR_E_NOTSUP);
    CONST(PSMSGR_E_STATE);
    CONST(PSMSGR_NAME_MAX);
    CONST(PSMSGR_STATE_MAX_CAPACITY);
    CONST(PSMSGR_STATE_DEFAULT_SLOTS);
    CONST(PSMSGR_STATE_RECREATE);
    CONST(PSMSGR_STATE_NO_NOTIFY);
    CONST(PSMSGR_INFO_ATTACHED);
    CONST(PSMSGR_WAITSET_MAX);
    return 0;
}

/* ---- interop agent ------------------------------------------------------ */

struct motor_status {
    uint64_t sequence;
    float speed_rpm;
    float current_a;
    float temperature_c;
};
#define MOTOR_STATUS_V1 0x00010001u

#define READ_MAX 4096

typedef struct out {
    size_t len;
    char s[64 + 2 * READ_MAX + 256];
} out;

static void put(out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->s + o->len, sizeof o->s - o->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += (size_t)n;
    if (o->len >= sizeof o->s)
        o->len = sizeof o->s - 1;
}

/* Ends a command whose result is printed: 1 if it was an error. */
static int done(int rc)
{
    fflush(stdout);
    return rc < 0;
}

/* Ends a command, printing rc if it is an error. */
static int emit(int rc)
{
    if (rc < 0)
        printf("{\"error\":%d,\"exception\":null}\n", rc);
    return done(rc);
}

static int usage(void)
{
    fputs("usage: interop_helper layout\n"
          "       interop_helper payload-layout\n"
          "       interop_helper write DIR NAME [--capacity N] [--payload-type T] [--recreate]\n"
          "                            [--no-notify] [--rate HZ] [--hold] SEQUENCE...\n"
          "       interop_helper read DIR NAME [--for MS]\n"
          "       interop_helper wait DIR NAME LAST_GENERATION TIMEOUT_MS\n"
          "       interop_helper alive DIR NAME\n"
          "       interop_helper    (the commands above but write, one per line of stdin)\n",
          stderr);
    return 2;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void motor_status_of(uint64_t n, struct motor_status *s)
{
    memset(s, 0, sizeof *s); /* the padding is published too */
    s->sequence = n;
    s->speed_rpm = (float)(n % 65536u) / 2.0f;
    s->current_a = (float)(n % 1024u) / 64.0f;
    s->temperature_c = (float)(n % 2048u) / 8.0f - 40.0f;
}

static int payload_layout(void)
{
    printf("{\"sizeof\":%zu,\"offsets\":{\"sequence\":%zu,\"speed_rpm\":%zu,\"current_a\":%zu,"
           "\"temperature_c\":%zu},\"payload_type\":%u}\n",
           sizeof(struct motor_status), offsetof(struct motor_status, sequence),
           offsetof(struct motor_status, speed_rpm), offsetof(struct motor_status, current_a),
           offsetof(struct motor_status, temperature_c), MOTOR_STATUS_V1);
    return emit(0);
}

static int write_values(int argc, char **argv)
{
    psmsgr_state_options o;
    psmsgr_state_options_init(&o);
    o.dir = argv[0];
    o.capacity = sizeof(struct motor_status);
    o.payload_type = MOTOR_STATUS_V1;
    double rate = 0;
    bool hold = false;
    int i = 2;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; ++i) {
        if (strcmp(argv[i], "--recreate") == 0)
            o.flags |= PSMSGR_STATE_RECREATE;
        else if (strcmp(argv[i], "--no-notify") == 0)
            o.flags |= PSMSGR_STATE_NO_NOTIFY;
        else if (strcmp(argv[i], "--hold") == 0)
            hold = true;
        else if (i + 1 < argc && strcmp(argv[i], "--capacity") == 0)
            o.capacity = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (i + 1 < argc && strcmp(argv[i], "--payload-type") == 0)
            o.payload_type = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (i + 1 < argc && strcmp(argv[i], "--rate") == 0)
            rate = strtod(argv[++i], NULL);
        else
            return usage();
    }

    psmsgr_state_writer *w = NULL;
    int rc = psmsgr_state_writer_open(argv[1], &o, &w);
    if (rc != PSMSGR_OK)
        return emit(rc);
    printf("{\"opened\":true}\n");
    fflush(stdout);
    uint64_t start = now_ns();
    for (int k = 0; i + k < argc; ++k) {
        if (rate > 0) {
            uint64_t t = start + (uint64_t)((double)k * 1e9 / rate);
            struct timespec ts = { .tv_sec = (time_t)(t / 1000000000u),
                                   .tv_nsec = (long)(t % 1000000000u) };
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
            }
        }
        struct motor_status s;
        motor_status_of(strtoull(argv[i + k], NULL, 0), &s);
        uint32_t gen;
        rc = psmsgr_state_publish(w, &s, sizeof s, &gen);
        if (rc != PSMSGR_OK) {
            psmsgr_state_writer_close(w);
            return emit(rc);
        }
        printf("{\"generation\":%" PRIu32 ",\"sequence\":%" PRIu64 "}\n", gen, s.sequence);
        fflush(stdout);
    }
    if (hold)
        while (getchar() != EOF) {
        }
    psmsgr_state_writer_close(w);
    return 0;
}

/* Formats one read result into *o and returns its code. */
static int format_read(psmsgr_state_reader *r, out *o)
{
    static unsigned char buf[READ_MAX];
    psmsgr_state_info info;
    psmsgr_state_desc desc;
    o->len = 0;
    int rc = psmsgr_state_read(r, buf, sizeof buf, &info);
    if (rc == PSMSGR_E_NODATA) {
        put(o, "{\"nodata\":true}");
        return rc;
    }
    if (rc == PSMSGR_OK)
        rc = psmsgr_state_describe(r, &desc);
    if (rc != PSMSGR_OK) {
        put(o, "{\"error\":%d,\"exception\":null}", rc);
        return rc;
    }
    put(o,
        "{\"generation\":%" PRIu32 ",\"length\":%" PRIu32 ",\"timestamp_ns\":%" PRIu64
        ",\"attached\":%s,\"payload_type\":%" PRIu32 ",\"value\":",
        info.generation, info.length, info.timestamp_ns,
        (info.flags & PSMSGR_INFO_ATTACHED) ? "true" : "false", desc.payload_type);
    if (info.length == sizeof(struct motor_status)) {
        struct motor_status s;
        memcpy(&s, buf, sizeof s);
        put(o,
            "{\"sequence\":%" PRIu64 ",\"speed_rpm\":%.9g,\"current_a\":%.9g,"
            "\"temperature_c\":%.9g}",
            s.sequence, (double)s.speed_rpm, (double)s.current_a, (double)s.temperature_c);
    } else {
        put(o, "null");
    }
    put(o, ",\"hex\":\"");
    for (uint32_t k = 0; k < info.length; ++k)
        put(o, "%02x", buf[k]);
    put(o, "\"}");
    return rc;
}

static int read_value(psmsgr_state_reader *r, int argc, char **argv)
{
    static out line, last;
    if (argc == 2) {
        int rc = format_read(r, &line);
        puts(line.s);
        return done(rc == PSMSGR_E_NODATA ? 0 : rc);
    }
    if (argc != 4 || strcmp(argv[2], "--for") != 0)
        return usage();
    uint64_t end = now_ns() + strtoull(argv[3], NULL, 0) * 1000000u;
    unsigned long long reads = 0, busy = 0;
    last.len = 0;
    last.s[0] = '\0';
    do {
        ++reads;
        if (format_read(r, &line) == PSMSGR_E_BUSY) {
            ++busy;
        } else if (strcmp(line.s, last.s) != 0) {
            puts(line.s);
            last = line;
        }
    } while (now_ns() < end);
    printf("{\"reads\":%llu,\"busy\":%llu}\n", reads, busy);
    return emit(0);
}

static int wait_change(psmsgr_state_reader *r, int argc, char **argv)
{
    if (argc != 4)
        return usage();
    int rc = psmsgr_state_wait(r, (uint32_t)strtoul(argv[2], NULL, 0),
                               (int32_t)strtol(argv[3], NULL, 0));
    if (rc == PSMSGR_OK || rc == PSMSGR_E_TIMEOUT)
        printf("{\"changed\":%s}\n", rc == PSMSGR_OK ? "true" : "false");
    return emit(rc == PSMSGR_E_TIMEOUT ? 0 : rc);
}

static int writer_alive(psmsgr_state_reader *r, int argc)
{
    if (argc != 2)
        return usage();
    int rc = psmsgr_state_writer_alive(r);
    if (rc >= 0)
        printf("{\"alive\":%s}\n", rc ? "true" : "false");
    return emit(rc);
}

/* The readers of a session, one per channel, open until the session ends. */
static struct session_reader {
    char *dir, *name;
    psmsgr_state_reader *r;
} readers[16];

static int reader_command(const char *cmd, int argc, char **argv, bool session)
{
    if (argc < 2)
        return usage();
    psmsgr_state_reader *r = NULL;
    size_t i = 0;
    if (session) {
        for (; i < sizeof readers / sizeof *readers && readers[i].r && !r; ++i)
            if (strcmp(readers[i].dir, argv[0]) == 0 && strcmp(readers[i].name, argv[1]) == 0)
                r = readers[i].r;
        if (!r && i == sizeof readers / sizeof *readers)
            return usage();
    }
    if (!r) {
        int rc = psmsgr_state_reader_open(argv[1], argv[0], &r);
        if (rc != PSMSGR_OK)
            return emit(rc);
        if (session)
            readers[i] = (struct session_reader){ strdup(argv[0]), strdup(argv[1]), r };
    }
    int rc;
    if (strcmp(cmd, "read") == 0)
        rc = read_value(r, argc, argv);
    else if (strcmp(cmd, "wait") == 0)
        rc = wait_change(r, argc, argv);
    else
        rc = writer_alive(r, argc);
    if (!session)
        psmsgr_state_reader_close(r);
    return rc;
}

static int command(int argc, char **argv, bool session)
{
    const char *cmd = argv[0];
    if (argc == 1 && strcmp(cmd, "payload-layout") == 0)
        return payload_layout();
    if (!session && argc >= 3 && strcmp(cmd, "write") == 0)
        return write_values(argc - 1, argv + 1);
    if (strcmp(cmd, "read") == 0 || strcmp(cmd, "wait") == 0 || strcmp(cmd, "alive") == 0)
        return reader_command(cmd, argc - 1, argv + 1, session);
    return usage();
}

static int run_session(void)
{
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *argv[16];
        int argc = 0;
        for (char *save, *tok = strtok_r(line, " \t\n", &save); tok && argc < 16;
             tok = strtok_r(NULL, " \t\n", &save))
            argv[argc++] = tok;
        if (argc > 0 && command(argc, argv, true) == 2)
            return 2;
    }
    for (size_t i = 0; i < sizeof readers / sizeof *readers && readers[i].r; ++i) {
        psmsgr_state_reader_close(readers[i].r);
        free(readers[i].dir);
        free(readers[i].name);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return run_session();
    if (argc == 2 && strcmp(argv[1], "layout") == 0)
        return layout();
    return command(argc - 1, argv + 1, false);
}
