/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L unit.c: tests that need no controller, no PipeWire and no root.
 * Copyright (c) 2026 Macauley Lim
 *
 * Wire format helpers, the stream-name address matcher, and the control
 * socket against a manager with no devices.
 */
#define _GNU_SOURCE

#include "ctl.h"
#include "manager.h"
#include "pw_route.h"
#include "ds5/ds5_protocol.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_crc(void)
{
    /* CRC-32 check value: crc32("123456789") = 0xCBF43926. */
    CHECK(ds5_report_crc('1', (const uint8_t *)"23456789", 8) == 0xCBF43926u);

    struct ds5_output_common st;
    memset(&st, 0, sizeof(st));
    st.valid_flag1 = DS5_VF1_LIGHTBAR_CONTROL;
    st.lightbar_red = 0xff;

    uint8_t buf[DS5_OUT_REPORT_BT_SIZE];
    CHECK(ds5_build_bt_state(buf, &st, 0x13) == DS5_OUT_REPORT_BT_SIZE);
    CHECK(buf[0] == DS5_OUT_REPORT_BT);
    CHECK(buf[1] == 0x30);                       /* seq in the high nibble */
    CHECK(buf[2] == DS5_OUTPUT_TAG);
    CHECK(buf[3 + 44] == 0xff);                  /* lightbar_red */
    CHECK(ds5_check_crc(DS5_CRC_SEED_OUTPUT, buf, sizeof(buf)));
    buf[10] ^= 1;
    CHECK(!ds5_check_crc(DS5_CRC_SEED_OUTPUT, buf, sizeof(buf)));

    uint8_t usb[DS5_OUT_REPORT_USB_SIZE];
    CHECK(ds5_build_usb_state(usb, &st) == DS5_OUT_REPORT_USB_SIZE);
    CHECK(usb[0] == DS5_OUT_REPORT_USB && usb[1 + 44] == 0xff);
}

static void test_audio_reports(void)
{
    struct ds5_output_common st;
    memset(&st, 0, sizeof(st));
    struct ds5_audio_frame f[2];
    memset(f, 0, sizeof(f));
    f[0].haptics[0] = 5;
    f[1].opus[DS5_SPEAKER_SEG_BYTES - 1] = 7;

    uint8_t buf[DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE];
    CHECK(ds5_build_bt_audio(buf, 1, &st, f, 1, 9, 0, 0) ==
          DS5_OUT_REPORT_BT_AUDIO_SIZE);
    CHECK(buf[0] == DS5_OUT_REPORT_BT_AUDIO);
    CHECK(ds5_check_crc(DS5_CRC_SEED_OUTPUT, buf, DS5_OUT_REPORT_BT_AUDIO_SIZE));

    CHECK(ds5_build_bt_audio(buf, 1, NULL, f, 2, 9, 1, 0) ==
          DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE);
    CHECK(buf[0] == DS5_OUT_REPORT_BT_AUDIO_DBL);
    /* Last byte of the second speaker frame: one pad byte, then the CRC. */
    CHECK(buf[DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE - 6] == 7);
    CHECK(ds5_check_crc(DS5_CRC_SEED_OUTPUT, buf,
                        DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE));

    CHECK(ds5_build_bt_haptics(buf, 2, f, 3, 1) == DS5_OUT_REPORT_BT_CTL_SIZE);
    CHECK(ds5_check_crc(DS5_CRC_SEED_OUTPUT, buf, DS5_OUT_REPORT_BT_CTL_SIZE));
    CHECK(ds5_build_bt_mic(buf, 2, 1) == DS5_OUT_REPORT_BT_CTL_SIZE);
    CHECK(buf[4] == 0x03);
}

static void test_triggers(void)
{
    uint8_t b[DS5_TRIGGER_BLOCK_BYTES];

    ds5_trig_off(b);
    CHECK(b[0] == DS5_TRIGGER_OFF && b[1] == 0);

    ds5_trig_feedback(b, 8, 8);                 /* zones 8 and 9, strength 8 */
    CHECK(b[0] == DS5_TRIGGER_FEEDBACK);
    CHECK(b[1] == 0x00 && b[2] == 0x03);
    uint32_t packed = (uint32_t)b[3] | (uint32_t)b[4] << 8 |
                      (uint32_t)b[5] << 16 | (uint32_t)b[6] << 24;
    CHECK(packed == (7u << 24 | 7u << 27));

    ds5_trig_weapon(b, 0, 0, 0);                /* clamped to 2, 3, 1 */
    CHECK(b[0] == DS5_TRIGGER_WEAPON);
    CHECK(b[1] == ((1u << 2) | (1u << 3)) && b[3] == 0);

    uint8_t p[12];
    for (int i = 0; i < 12; i++)
        p[i] = (uint8_t)(i + 1);
    ds5_trig_raw(b, 0x26, p, sizeof(p));        /* capped at 10 params */
    CHECK(b[0] == 0x26 && b[1] == 1 && b[10] == 10);
}

static void test_route_names(void)
{
    const uint8_t a[6] = { 0x00, 0x11, 0x22, 0x33, 0xaa, 0xbb };

    CHECK(ds4l_route_name_has_addr("HD haptics (00-11-22-33-AA-BB)", a));
    CHECK(ds4l_route_name_has_addr("00:11:22:33:aa:bb", a));
    CHECK(ds4l_route_name_has_addr("pad 00112233aabb", a));
    CHECK(!ds4l_route_name_has_addr("00:11:22:33:aa:bc", a));
    CHECK(!ds4l_route_name_has_addr("00-11:22-33-aa-bb", a));     /* mixed */
    CHECK(!ds4l_route_name_has_addr("ff00112233aabb", a));        /* longer run */
    CHECK(!ds4l_route_name_has_addr("00112233aabbcc", a));
    CHECK(!ds4l_route_name_has_addr("00:11:22:33:aa:bb:cc", a));
    CHECK(!ds4l_route_name_has_addr("", a));
    CHECK(!ds4l_route_name_has_addr(NULL, a));
}

/* control socket */

struct client {
    const char *path;
    const char *send;
    char        reply[4096];
};

static void *client_thread(void *arg)
{
    struct client *c = arg;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", c->path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    size_t len = strlen(c->send);
    if (write(fd, c->send, len) != (ssize_t)len) {
        close(fd);
        return NULL;
    }
    shutdown(fd, SHUT_WR);
    size_t have = 0;
    ssize_t n;
    while ((n = read(fd, c->reply + have, sizeof(c->reply) - 1 - have)) > 0)
        have += (size_t)n;
    c->reply[have] = '\0';
    close(fd);
    return NULL;
}

static void roundtrip(ds4l_mgr *m, int lfd, struct client *c)
{
    pthread_t th;
    c->reply[0] = '\0';
    if (pthread_create(&th, NULL, client_thread, c) != 0) {
        failures++;
        return;
    }
    ds4l_ctl_serve(m, lfd);
    pthread_join(th, NULL);
}

static void test_ctl(void)
{
    const char *tmp = getenv("TMPDIR");
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/ds4l-test-XXXXXX",
             tmp && *tmp && strlen(tmp) < 60 ? tmp : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        failures++;
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/ctl.sock", dir);

    ds4l_mgr *m = ds4l_mgr_create(NULL, NULL);
    CHECK(m != NULL);
    int lfd = ds4l_ctl_listen(path);
    CHECK(lfd >= 0);
    if (!m || lfd < 0)
        goto out;

    struct client c = { .path = path };
    static const char want[] =
        "ok pong\n"
        "ok 0 device(s)\n"
        "err no such device: - (see 'list')\n"
        "err no such device: zz (see 'list')\n"
        "ok commands:";
    c.send = "ping\n\nlist\n- status\nzz rumble 1 2\nhelp\n";
    roundtrip(m, lfd, &c);
    CHECK(strncmp(c.reply, want, sizeof(want) - 1) == 0);

    /* An over-long line is refused and the session carries on. */
    static char big[4096];
    memset(big, 'x', 3000);
    strcpy(big + 3000, "\nping\n");
    c.send = big;
    roundtrip(m, lfd, &c);
    CHECK(strstr(c.reply, "err line too long\n") != NULL);
    CHECK(strstr(c.reply, "ok pong\n") != NULL);

    /* Without a trailing newline nothing is run. */
    c.send = "ping";
    roundtrip(m, lfd, &c);
    CHECK(c.reply[0] == '\0');

    /* A second daemon must not take over a live socket. Its probe leaves a
     * connection behind, so this goes last. */
    errno = 0;
    CHECK(ds4l_ctl_listen(path) == -1 && errno == EADDRINUSE);

out:
    if (lfd >= 0)
        close(lfd);
    ds4l_mgr_destroy(m);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    test_crc();
    test_audio_reports();
    test_triggers();
    test_route_names();
    test_ctl();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
