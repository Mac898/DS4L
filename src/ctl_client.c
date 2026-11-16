/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L ctl_client.c: client side of the control socket, used by the ds4l CLI.
 * Copyright (c) 2026 Macauley Lim
 *
 * Kept apart from the daemon code so the CLI doesn't link PipeWire or udev.
 */
#define _GNU_SOURCE

#include "ctl_client.h"

#include <errno.h>
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
        snprintf(path, sizeof(path), "/tmp/ds4l-%u/ds4l.sock",
                 (unsigned)getuid());
    return path;
}

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
