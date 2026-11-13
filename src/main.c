/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — main.c : the ds4ld daemon.
 * Copyright (c) 2026 Macauley Lim
 *
 * One daemon for every connected DualSense, Bluetooth or USB. Gamepad input
 * stays with the kernel hid-playstation module; the daemon adds:
 *   - per-controller PipeWire nodes (speaker, haptics, mic)
 *   - a control socket (`ds4l` CLI) for triggers, LEDs, rumble, volume,
 *     routing and mute
 *   - a guard that installs the DS4L kernel module (driver/) before audio
 *     mode is opened, if it isn't already loaded
 */
#define _GNU_SOURCE

#include "ctl.h"
#include "dkms.h"
#include "manager.h"
#include "pw_nodes.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void log_cb(int lvl, const char *msg, void *ud)
{
    int *verbose = ud;
    const char *tag = lvl == 'e' ? "error" : lvl == 'w' ? "warn "
                       : lvl == 'i' ? "info " : "debug";
    if (lvl == 'd' && !*verbose)
        return;
    fprintf(stderr, "ds4ld: %s: %s\n", tag, msg);
}

/* Start the DKMS install if a controller is present and the loaded
 * hid_playstation is not the DS4L module. Returns 1 if one was started. */
static int maybe_auto_dkms(ds4l_mgr *m, int auto_dkms)
{
    if (!auto_dkms)
        return 0;
    if (ds4l_mgr_audio_safe(m))
        return 0;

    struct ds4l_devinfo infos[1];
    int n = ds4l_mgr_list(m, infos, 1);
    if (n < 1)
        return 0;

    char resp[256];
    char *av[] = { (char *)"dkms", (char *)"install" };
    ds4l_mgr_admin(m, 2, av, resp, sizeof(resp));
    fprintf(stderr, "ds4ld: info: auto DKMS install: %s\n", resp);
    return 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "ds4ld — DualSense userspace driver daemon\n"
        "\n"
        "usage: ds4ld [options]\n"
        "  -s, --socket PATH   control socket "
            "(default: $XDG_RUNTIME_DIR/ds4l.sock)\n"
        "  -v, --verbose       debug logging\n"
        "  -n, --no-auto-dkms  do not auto-install the DS4L kernel module\n"
        "  -k, --keepalive N   extra state reports per haptic frame, 0-3\n"
        "                      (default 1; raise if haptics stutter, 0 saves\n"
        "                      airtime on adapters that deliver evenly)\n"
        "  -h, --help          this help\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "socket",       required_argument, NULL, 's' },
        { "verbose",      no_argument,       NULL, 'v' },
        { "no-auto-dkms", no_argument,       NULL, 'n' },
        { "keepalive",    required_argument, NULL, 'k' },
        { "help",         no_argument,       NULL, 'h' },
        { 0 }
    };

    const char *sockpath = NULL;
    int verbose = 0, auto_dkms = 1, keepalive = 1;

    int c;
    while ((c = getopt_long(argc, argv, "s:vnk:h", opts, NULL)) != -1) {
        switch (c) {
        case 's': sockpath = optarg; break;
        case 'v': verbose = 1; break;
        case 'n': auto_dkms = 0; break;
        case 'k': keepalive = atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!sockpath)
        sockpath = ds4l_ctl_default_path();

    ds4l_mgr *m = ds4l_mgr_create(log_cb, &verbose);
    if (!m) {
        fprintf(stderr, "ds4ld: error: cannot create manager\n");
        return 1;
    }

    ds4l_mgr_set_haptic_keepalive(m, keepalive);

    int ctl_fd = ds4l_ctl_listen(sockpath);
    if (ctl_fd < 0) {
        fprintf(stderr, "ds4ld: error: cannot listen on %s: %s\n",
                sockpath, strerror(errno));
        ds4l_mgr_destroy(m);
        return 1;
    }

    int udev_fd = ds4l_mgr_hotplug_fd(m);
    if (udev_fd < 0)
        fprintf(stderr, "ds4ld: warn: udev monitor unavailable; "
                "falling back to periodic rescan\n");

    /* Also rescan every 2 s: covers missed udev events and hidraw nodes that
     * died and came back under the same name. */
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd >= 0) {
        struct itimerspec its = {
            .it_interval = { .tv_sec = 2 },
            .it_value    = { .tv_sec = 2 },
        };
        timerfd_settime(timer_fd, 0, &its, NULL);
    }

    fprintf(stderr, "ds4ld: info: listening on %s%s\n", sockpath,
            ds4l_pw_available() ? ", PipeWire nodes" : ", no PipeWire (audio off)");

    /* Initial device set */
    ds4l_mgr_rescan(m);
    maybe_auto_dkms(m, auto_dkms);

    for (;;) {
        struct pollfd pfds[3];
        int n = 0;
        int idx_ctl = -1, idx_udev = -1, idx_timer = -1;

        if (ctl_fd >= 0) {
            idx_ctl = n;
            pfds[n].fd = ctl_fd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (udev_fd >= 0) {
            idx_udev = n;
            pfds[n].fd = udev_fd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (timer_fd >= 0) {
            idx_timer = n;
            pfds[n].fd = timer_fd;
            pfds[n].events = POLLIN;
            n++;
        }

        int rc = poll(pfds, (nfds_t)n, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (idx_ctl >= 0 && (pfds[idx_ctl].revents & POLLIN))
            ds4l_ctl_serve(m, ctl_fd);
        if (idx_udev >= 0 && (pfds[idx_udev].revents & POLLIN))
            ds4l_mgr_hotplug_handle(m);
        if (idx_timer >= 0 && (pfds[idx_timer].revents & POLLIN)) {
            uint64_t t;
            (void)read(timer_fd, &t, sizeof(t));
            ds4l_mgr_rescan(m);
        }
    }

    if (timer_fd >= 0)
        close(timer_fd);
    if (udev_fd >= 0)
        close(udev_fd);
    close(ctl_fd);
    unlink(sockpath);

    ds4l_pw_shutdown();
    ds4l_mgr_destroy(m);
    return 0;
}
