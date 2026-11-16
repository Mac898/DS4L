/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — ctl.c : line-oriented Unix-socket control protocol.
 * Copyright (c) 2026 Macauley Lim
 *
 * Protocol (one command per line, replies one line each, prefix-tagged):
 *   ping
 *   list                          -> "dev <status-line>"* then "ok N device(s)"
 *   dkms [status|install]
 *   <device-id|-> <command> ...   -> device commands (see manager.c)
 * Replies: "ok [text]" / "err [text]".
 * "-" as the device id selects the first (only) controller.
 */
#define _GNU_SOURCE

#include "ctl.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The /tmp fallback lives in a 0700 directory owned by us. Anything else
 * there (another user's dir, a symlink) is refused rather than reused. */
static int private_tmp_dir(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    *slash = '\0';

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    struct stat st;
    if (lstat(dir, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int ds4l_ctl_listen(const char *path)
{
    if (!getenv("XDG_RUNTIME_DIR") &&
        strcmp(path, ds4l_ctl_default_path()) == 0 &&
        private_tmp_dir(path) != 0)
        return -1;

    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, path);

    mode_t old = umask(077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (rc != 0 || listen(fd, 8) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static void send_line(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)   /* truncated */
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    ssize_t rc;
    do {
        rc = write(fd, buf, (size_t)n);
    } while (rc < 0 && errno == EINTR);
}

/* Handle one command line; writes 1..N reply lines. */
static void handle_line(ds4l_mgr *m, char *line, int fd)
{
    /* Tokenize. */
    char *argv[32];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &save); tok && argc < 32;
         tok = strtok_r(NULL, " \t\r\n", &save))
        argv[argc++] = tok;
    if (argc == 0)
        return; /* blank line */

    char resp[1024];

    if (!strcmp(argv[0], "ping")) {
        send_line(fd, "ok pong");
        return;
    }
    if (!strcmp(argv[0], "help")) {
        send_line(fd, "ok commands: ping | list | dkms status|install | "
                      "<id|-> status input audio keepalive lightbar playerleds rumble "
                      "trigger micled power volume micvol route mute");
        return;
    }
    if (!strcmp(argv[0], "list")) {
        struct ds4l_devinfo infos[DS4L_MAX_DEVS];
        int n = ds4l_mgr_list(m, infos, DS4L_MAX_DEVS);
        for (int i = 0; i < n; i++) {
            ds4l_devinfo_status(&infos[i], resp, sizeof(resp));
            send_line(fd, "dev %s", resp);
        }
        send_line(fd, "ok %d device(s)", n);
        return;
    }
    if (!strcmp(argv[0], "dkms")) {
        int rc = ds4l_mgr_admin(m, argc, argv, resp, sizeof(resp));
        send_line(fd, "%s %s", rc == 0 ? "ok" : "err", resp);
        return;
    }

    /* Device command: "<id|-> <cmd> [args]" */
    const char *id = strcmp(argv[0], "-") == 0 ? NULL : argv[0];
    struct ds4l_dev *dev = ds4l_mgr_find(m, id);
    if (!dev) {
        send_line(fd, "err no such device: %s (see 'list')", argv[0]);
        return;
    }
    int rc = ds4l_mgr_command(m, dev, argc - 1, argv + 1, resp, sizeof(resp));
    if (resp[0])
        send_line(fd, "%s %s", rc == 0 ? "ok" : "err", resp);
    else
        send_line(fd, rc == 0 ? "ok" : "err");
}

void ds4l_ctl_serve(ds4l_mgr *m, int listen_fd)
{
    int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return;

    /* Bound the session so a stuck client can't wedge the daemon loop. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char buf[2048];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;  /* timeout: drop the client */
        }
        if (n == 0)
            break;  /* client done */

        have += (size_t)n;
        buf[have] = '\0';

        /* Process every complete line; keep any partial tail buffered. */
        char *p = buf, *nl;
        while ((nl = strchr(p, '\n')) != NULL) {
            *nl = '\0';
            handle_line(m, p, fd);
            p = nl + 1;
        }
        size_t rem = (size_t)(buf + have - p);
        memmove(buf, p, rem);
        have = rem;

        if (have >= sizeof(buf) - 1) {  /* absurdly long line: flush it */
            buf[0] = '\0';
            have = 0;
            send_line(fd, "err line too long");
        }
    }

    close(fd);
}
