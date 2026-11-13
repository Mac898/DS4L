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
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

const char *ds4l_ctl_default_path(void)
{
    static char path[256];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt)
        snprintf(path, sizeof(path), "%s/ds4l.sock", rt);
    else
        snprintf(path, sizeof(path), "/tmp/ds4l-%d.sock", getuid());
    return path;
}

int ds4l_ctl_listen(const char *path)
{
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

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
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

/* client (used by the ds4l CLI) */

/* Does `prefix` name exactly one connected device (id prefix match)?
 * Returns 1 yes, 0 no, -1 daemon unreachable. */
int ds4l_ctl_resolve_device(const char *path, const char *prefix)
{
    if (!prefix || !*prefix)
        return 0;

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
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (write(fd, "list\n", 5) != 5) {
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -1;
    }

    int matches = 0;
    char ln[1024];
    size_t plen = strlen(prefix);
    while (fgets(ln, sizeof(ln), f)) {
        if (strncmp(ln, "dev id=", 7) != 0)
            continue;
        const char *id = ln + 7;
        const char *sp = strchr(id, ' ');
        size_t idlen = sp ? (size_t)(sp - id) : strlen(id);
        if (idlen >= plen && strncmp(id, prefix, plen) == 0)
            matches++;
    }
    fclose(f);
    return matches == 1 ? 1 : 0;
}

int ds4l_ctl_client(const char *path, int argc, char **argv)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        close(fd);
        return 1;
    }
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "cannot connect to DS4L daemon at %s: %s\n"
                        "(is ds4ld running?)\n",
                path, strerror(errno));
        close(fd);
        return 1;
    }

    /* Send the command as one line. */
    char line[2048];
    size_t len = 0;
    for (int i = 0; i < argc && len + 2 < sizeof(line); i++) {
        int n = snprintf(line + len, sizeof(line) - len, "%s%s",
                         i ? " " : "", argv[i]);
        if (n < 0)
            break;
        len += (size_t)n;
    }
    line[len++] = '\n';
    if (write(fd, line, len) != (ssize_t)len) {
        perror("write");
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);

    /* Print every reply line; exit status follows the last ok/err. */
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return 1;
    }
    char ln[1024];
    int last_ok = 1;
    while (fgets(ln, sizeof(ln), f)) {
        fputs(ln, stdout);
        if (!strncmp(ln, "err", 3))
            last_ok = 0;
        else if (!strncmp(ln, "ok", 2))
            last_ok = 1;
    }
    fclose(f);
    return last_ok ? 0 : 1;
}
