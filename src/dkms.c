/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — dkms.c : hid-playstation DS4L module detection + auto-install.
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

#ifndef DS4L_DRIVER_DIR
#define DS4L_DRIVER_DIR "."    /* set by meson */
#endif
#ifndef DS4L_TOOLS_DIR
#define DS4L_TOOLS_DIR "."
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
    char *cmd = NULL;
    if (asprintf(&cmd,
                 "modinfo -n hid_playstation 2>/dev/null") < 0 || !cmd)
        return DS4L_DKMS_INSTALLED_UNKNOWN;

    FILE *f = popen(cmd, "r");
    free(cmd);
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

int ds4l_dkms_audio_safe(void)
{
    return ds4l_dkms_probe() == DS4L_DKMS_FORK_LOADED;
}

/* install / reload */

static const char *driver_dir(void)
{
    const char *env = getenv("DS4L_DRIVER_DIR");
    return env && *env ? env : DS4L_DRIVER_DIR;
}

static const char *tools_dir(void)
{
    const char *env = getenv("DS4L_TOOLS_DIR");
    return env && *env ? env : DS4L_TOOLS_DIR;
}

/* Run argv, capturing nothing, logging the exit status. Returns the exit code
 * (or 127 on spawn failure). */
static int run(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        /* Child: keep stdio so pkexec/sudo can prompt. */
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
        return 0;   /* already there */

    /* Build the helper command line: tools/ds4l-dkms-ensure.sh <driver-dir> */
    char script[512], driver[512];
    snprintf(script, sizeof(script), "%s/ds4l-dkms-ensure.sh", tools_dir());
    snprintf(driver,  sizeof(driver),  "%s", driver_dir());

    if (access(script, X_OK) != 0) {
        fprintf(stderr, "DS4L: helper script not found/executable: %s\n",
                script);
        return -1;
    }

    if (geteuid() == 0) {
        char *const av[] = { (char *)"/bin/sh", (char *)script, driver, NULL };
        run(av);
    } else {
        /* Prefer pkexec (graphical/Polkit auth), fall back to sudo. */
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "exec '%s' '%s'", script, driver);

        char *const pk[] = { (char *)"pkexec", (char *)"sh",
                             (char *)"-c", cmd, NULL };
        int rc = run(pk);
        if (rc == 127) {   /* pkexec not present */
            char *const su[] = { (char *)"sudo", (char *)"-b",
                                 (char *)script, driver, NULL };
            rc = run(su);
        }
    }

    return ds4l_dkms_probe() == DS4L_DKMS_FORK_LOADED ? 0 : -1;
}
