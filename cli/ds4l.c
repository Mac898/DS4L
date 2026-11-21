/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L ds4l.c: the command-line client for the ds4ld daemon.
 * Copyright (c) 2026 Macauley Lim
 *
 * Connects to the daemon's control socket and forwards a command. The device
 * id may be omitted (first controller) or given as an id or a unique prefix
 * of 4 or more characters; see `ds4l list` for ids (the Bluetooth address
 * as hex, bytes reversed).
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/ctl_client.h"

#ifndef DS4L_VERSION
#define DS4L_VERSION "unknown"
#endif

static void usage(FILE *f)
{
    fprintf(f,
        "ds4l: control the DS4L daemon (DualSense userspace driver)\n"
        "\n"
        "usage: ds4l [options] <command> [args...]\n"
        "\n"
        "options:\n"
        "  -s PATH   control socket (default: $XDG_RUNTIME_DIR/ds4l.sock)\n"
        "  -V        print the version\n"
        "  -h        this help\n"
        "\n"
        "daemon commands:\n"
        "  ping\n"
        "  help                     the daemon's command list\n"
        "  list                     connected controllers + status\n"
        "  dkms status|install      kernel DS4L module state / auto-install\n"
        "\n"
        "device commands ([id] = optional device id from `list`, or a unique\n"
        "prefix of 4+ characters; without it the first controller is used):\n"
        "  status  [id]             id, bus, audio gate, I/O counters\n"
        "  input   [id]             parsed sticks/buttons/sensors/battery\n"
        "  audio   [id] on|off      manually open/close the audio gate\n"
        "  keepalive [id] 0..3      extra state reports per haptic frame\n"
        "  lightbar [id] R G B      lightbar color (0-255)\n"
        "  playerleds [id] m|p1..p5 player LEDs (mask or centered player)\n"
        "  rumble  [id] STRONG WEAK classic rumble (0-255)\n"
        "  trigger [id] l|r off\n"
        "  trigger [id] l|r feedback POS STRENGTH\n"
        "  trigger [id] l|r weapon START STOP STRENGTH\n"
        "  trigger [id] l|r vibration POS AMPLITUDE FREQ\n"
        "  trigger [id] l|r raw MODE B0..B9\n"
        "  micled  [id] on|off      mic-mute LED\n"
        "  power   [id] RR TR       motor power reduction (0-7 each)\n"
        "  volume  [id] SPK HP      hardware volumes (0-255 / 0-127)\n"
        "  micvol  [id] V           mic volume (0-64)\n"
        "  route   [id] hp|spk|both audio output routing\n"
        "  mute    [id] on|off on|off on|off   mic, speaker, headphone\n");
}

/* Commands that take NO leading device id. */
static int is_daemon_cmd(const char *s)
{
    return !strcmp(s, "ping") || !strcmp(s, "list") ||
           !strcmp(s, "dkms") || !strcmp(s, "help");
}

int main(int argc, char **argv)
{
    const char *sockpath = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "+s:Vh")) != -1) {
        switch (opt) {
        case 's': sockpath = optarg; break;
        case 'V': puts("ds4l " DS4L_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!sockpath)
        sockpath = ds4l_ctl_default_path();

    if (optind >= argc) {
        usage(stderr);
        return 2;
    }

    /* Device commands go out as "<id|-> <verb> <args...>". The token after
     * the verb is the id if it uniquely prefixes a connected device's id. */
    char *fwd[40];
    int nf = 0;
    const char *cmd = argv[optind];

    if (is_daemon_cmd(cmd)) {
        for (int i = optind; i < argc && nf < 40; i++)
            fwd[nf++] = argv[i];
    } else {
        const char *id = "-";
        int first_arg = optind + 1;

        /* If the daemon is unreachable, ds4l_ctl_client() says so below. */
        if (optind + 1 < argc) {
            const char *tok = argv[optind + 1];
            int r = strcmp(tok, "-") == 0 ? 1
                                          : ds4l_ctl_resolve_device(sockpath, tok);
            if (r == 2) {
                fprintf(stderr, "ds4l: %s matches more than one controller "
                                "(see 'ds4l list')\n", tok);
                return 1;
            }
            if (r == 1) {
                id = tok;
                first_arg = optind + 2;
            }
        }

        fwd[nf++] = (char *)id;
        fwd[nf++] = (char *)cmd;
        for (int i = first_arg; i < argc && nf < 40; i++)
            fwd[nf++] = argv[i];
    }

    return ds4l_ctl_client(sockpath, nf, fwd);
}
