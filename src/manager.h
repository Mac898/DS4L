/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — manager.h : the multi-controller device manager.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_MANAGER_H
#define DS4L_MANAGER_H

#include "dkms.h"
#include "hidraw.h"

#include <stdint.h>

#define DS4L_MAX_DEVS 16

typedef struct ds4l_mgr ds4l_mgr;

struct ds4l_devinfo {
    char     id[36];       /* MAC hex (12 chars) or "hidrawN" fallback  */
    char     node[32];     /* "hidraw3"                                 */
    ds5_bus  bus;          /* DS5_BUS_BT / DS5_BUS_USB                  */
    uint8_t  mac[6];
    int      have_mac;
    uint32_t hw_version, fw_version;
    uint16_t update_version;
    int      have_fw;
    int      audio_mode;   /* audio-mode gate currently open for it     */
};

/* Log callback: levels 'e' error, 'w' warn, 'i' info, 'd' debug. */
typedef void (*ds4l_log_fn)(int level, const char *msg, void *ud);

ds4l_mgr *ds4l_mgr_create(ds4l_log_fn log, void *log_ud);
void      ds4l_mgr_destroy(ds4l_mgr *m);
/* Default for newly connected controllers; see ds5_set_haptic_keepalive(). */
void      ds4l_mgr_set_haptic_keepalive(ds4l_mgr *m, int n);

/* udev-based hotplug: fd to poll() for readability, then call
 * ds4l_mgr_hotplug_handle(). Returns -1 if udev is unavailable (caller should
 * fall back to periodic ds4l_mgr_rescan()). */
int  ds4l_mgr_hotplug_fd(ds4l_mgr *m);
void ds4l_mgr_hotplug_handle(ds4l_mgr *m);

/* Sync the managed device set with the DualSense controllers currently
 * present: add new ones (open hidraw, query identity, start the output
 * cadence, create PipeWire nodes), remove gone ones (stop nodes, tell the
 * controller to stop streaming, close). Safe to call repeatedly. */
void ds4l_mgr_rescan(ds4l_mgr *m);

/* Snapshot of the current device infos (deep-copied into out[]). Returns
 * count. Caller-visible; used by the control socket. */
int  ds4l_mgr_list(ds4l_mgr *m, struct ds4l_devinfo *out, int max);

/* Find a managed device by id (exact match, or unique prefix, or NULL for the
 * first device). Returns the handle used by the command handlers below, or
 * NULL. */
struct ds4l_dev *ds4l_mgr_find(ds4l_mgr *m, const char *id);

/* Execute one device command. argc/argv: tokens after the device id.
 * Reply text (one line, no newline) is written into resp (nul-terminated);
 * resp[0]=='\0' means "ok, no output". Returns 0 on success, -1 on error
 * (resp then holds the error message). */
int ds4l_mgr_command(ds4l_mgr *m, struct ds4l_dev *dev,
                     int argc, char **argv, char *resp, size_t respsz);

/* Daemon-level commands (not per-device). `dkms status` / `dkms install`. */
int ds4l_mgr_admin(ds4l_mgr *m, int argc, char **argv,
                   char *resp, size_t respsz);

/* Whether audio mode may currently be opened (DS4L module loaded). */
int ds4l_mgr_audio_safe(ds4l_mgr *m);

/* One-line human status for a devinfo (used by ctl.c "list"). */
void ds4l_devinfo_status(const struct ds4l_devinfo *i, char *out, size_t n);

#endif /* DS4L_MANAGER_H */
