/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — dkms.h : hid-playstation DS4L module detection + auto-install.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_DKMS_H
#define DS4L_DKMS_H

typedef enum ds4l_dkms_state {
    DS4L_DKMS_FORK_LOADED = 0,   /* DS4L module loaded: audio mode is safe  */
    DS4L_DKMS_STOCK_LOADED,      /* stock hid-playstation loaded: not safe  */
    DS4L_DKMS_NOT_LOADED,        /* hid_playstation not loaded              */
} ds4l_dkms_state;

/* Which module file modprobe would pick (updates/dkms means the DS4L one). */
typedef enum ds4l_dkms_install {
    DS4L_DKMS_INSTALLED_FORK = 0,   /* DS4L module installed (dkms)      */
    DS4L_DKMS_INSTALLED_NONE,        /* only the stock in-tree module     */
    DS4L_DKMS_INSTALLED_UNKNOWN,     /* modinfo failed / couldn't tell    */
} ds4l_dkms_install;

/* Probe the loaded hid_playstation module. Only the DS4L module has the
 * ds5_drop_all_input parameter. */
ds4l_dkms_state  ds4l_dkms_probe(void);

/* Probe which module modprobe would resolve to. */
ds4l_dkms_install ds4l_dkms_installed(void);

/* Human-readable text for the enums above. */
const char *ds4l_dkms_state_str(ds4l_dkms_state s);
const char *ds4l_dkms_install_str(ds4l_dkms_install i);

/* Build, install and reload the DKMS module, escalating via pkexec then sudo.
 * Blocks until done (a kernel build can take a minute, plus an auth prompt).
 * Returns 0 iff the DS4L module is loaded afterwards. */
int ds4l_dkms_ensure(void);

/* Is the DS4L module loaded, i.e. is audio mode safe? */
int ds4l_dkms_audio_safe(void);

#endif /* DS4L_DKMS_H */
