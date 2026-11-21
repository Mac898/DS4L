/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L dkms.c: hid-playstation DS4L module detection + auto-install.
 * Copyright (c) 2026 Macauley Lim
 *
 * The DS4L kernel module (driver/) is hid-playstation plus a filter for the
 * audio-mode input reports, so the kernel keeps providing the gamepad while
 * DS4L streams audio over hidraw. This file detects whether it is the loaded
 * module and, if not, builds, installs and reloads it, escalating to root.
 *
 * Only the DS4L module has the ds5_drop_all_input parameter, so its presence
 * in /sys/module/hid_playstation/parameters/ means ours is loaded.
 */
#define _GNU_SOURCE

#include "dkms.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char FORK_PARAM[] =
    "/sys/module/hid_playstation/parameters/ds5_drop_all_input";
static const char MODULE_SYSFS[] = "/sys/module/hid_playstation";

#ifndef DS4L_DATADIR
#define DS4L_DATADIR "/usr/local/share/ds4l"   /* set by meson */
#endif
#ifndef DS4L_SRCDIR
#define DS4L_SRCDIR "."
#endif

const char *ds4l_dkms_state_str(ds4l_dkms_state s)
{
    switch (s) {
    case DS4L_DKMS_FORK_LOADED: return "fork-loaded";
    case DS4L_DKMS_STOCK_LOADED: return "stock-loaded";
    case DS4L_DKMS_NOT_LOADED:   return "not-loaded";
    }
    return "?";
}

const char *ds4l_dkms_install_str(ds4l_dkms_install i)
{
    switch (i) {
    case DS4L_DKMS_INSTALLED_FORK: return "fork-installed";
    case DS4L_DKMS_INSTALLED_NONE:   return "stock-only";
    case DS4L_DKMS_INSTALLED_UNKNOWN:return "unknown";
    }
    return "?";
}

ds4l_dkms_state ds4l_dkms_probe(void)
{
    struct stat st;
    if (stat(MODULE_SYSFS, &st) != 0)
        return DS4L_DKMS_NOT_LOADED;
    if (stat(FORK_PARAM, &st) == 0)
        return DS4L_DKMS_FORK_LOADED;
    return DS4L_DKMS_STOCK_LOADED;
}

ds4l_dkms_install ds4l_dkms_installed(void)
{
    FILE *f = popen("modinfo -n hid_playstation 2>/dev/null", "r");
    if (!f)
        return DS4L_DKMS_INSTALLED_UNKNOWN;

    char line[512];
    int found = 0, ours = 0;
    while (fgets(line, sizeof(line), f)) {
        found = 1;
        if (strstr(line, "/updates/dkms/"))
            ours = 1;
    }
    pclose(f);

    if (!found)
        return DS4L_DKMS_INSTALLED_UNKNOWN;
    return ours ? DS4L_DKMS_INSTALLED_FORK : DS4L_DKMS_INSTALLED_NONE;
}

/* Where the root helper and the driver tree live: $DS4L_TOOLS_DIR and
 * $DS4L_DRIVER_DIR if set, else the installed share/ds4l copy, else the
 * source tree (an uninstalled dev build). */
static void helper_paths(char *script, size_t slen, char *driver, size_t dlen)
{
    const char *tools = getenv("DS4L_TOOLS_DIR");
    const char *drv = getenv("DS4L_DRIVER_DIR");
    const char *base = DS4L_DATADIR;

    snprintf(script, slen, "%s/tools/ds4l-dkms-ensure.sh", base);
    if (access(script, X_OK) != 0)
        base = DS4L_SRCDIR;

    if (tools && *tools)
        snprintf(script, slen, "%s/ds4l-dkms-ensure.sh", tools);
    else
        snprintf(script, slen, "%s/tools/ds4l-dkms-ensure.sh", base);
    if (drv && *drv)
        snprintf(driver, dlen, "%s", drv);
    else
        snprintf(driver, dlen, "%s/driver", base);
}

/* The helper runs as root. Say so when someone other than root could have
 * changed it (a ~/.local install, a checkout). */
static void warn_if_writable(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 &&
        (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))))
        fprintf(stderr, "DS4L: note: %s is not root-owned; it runs as root "
                        "once you authorize it\n", path);
}

static int in_path(const char *name)
{
    const char *p = getenv("PATH");
    if (!p || !*p)
        p = "/usr/bin:/bin";
    char buf[512];
    while (*p) {
        size_t n = strcspn(p, ":");
        if (n > 0 && n < sizeof(buf) - strlen(name) - 2) {
            snprintf(buf, sizeof(buf), "%.*s/%s", (int)n, p, name);
            if (access(buf, X_OK) == 0)
                return 1;
        }
        p += n;
        if (*p == ':')
            p++;
    }
    return 0;
}

/* Run argv and wait. Returns the exit code, 127 if it could not start. */
static int run(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        /* Child: keep stdio so pkexec/sudo can prompt. The daemon blocks
         * SIGINT/SIGTERM for its signalfd; don't pass that on. */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execvp(argv[0], argv);
        _exit(127);
    }

    int st;
    do {
        if (waitpid(pid, &st, 0) < 0) {
            if (errno == EINTR)
                continue;
            return 127;
        }
    } while (!WIFEXITED(st) && !WIFSIGNALED(st));

    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int ds4l_dkms_ensure(void)
{
    if (ds4l_dkms_probe() == DS4L_DKMS_FORK_LOADED)
        return 0;

    char script[512], driver[512];
    helper_paths(script, sizeof(script), driver, sizeof(driver));

    if (access(script, X_OK) != 0) {
        fprintf(stderr, "DS4L: helper script not found/executable: %s\n",
                script);
        return -1;
    }

    if (geteuid() == 0) {
        char *const av[] = { script, driver, NULL };
        run(av);
    } else {
        warn_if_writable(script);
        /* pkexec for a graphical prompt, else sudo. Both wait, so the probe
         * below sees the result. */
        if (in_path("pkexec")) {
            char *const av[] = { (char *)"pkexec", script, driver, NULL };
            run(av);
        } else {
            char *const av[] = { (char *)"sudo", script, driver, NULL };
            run(av);
        }
    }

    return ds4l_dkms_probe() == DS4L_DKMS_FORK_LOADED ? 0 : -1;
}
