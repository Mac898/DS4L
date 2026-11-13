/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — hidraw.h : multi-device hidraw enumeration + DualSense identity.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_HIDRAW_H
#define DS4L_HIDRAW_H

#include "ds5/ds5.h"

#include <stdint.h>

#define DS4L_MAX_HIDRAW 16

typedef struct ds4l_hid_info {
    char     node[32];   /* "hidraw3"                          */
    char     dev[40];    /* "/dev/hidraw3"                     */
    unsigned bus;        /* HID bus (3 = USB, 5 = Bluetooth)   */
    unsigned vid, pid;
} ds4l_hid_info;

/* Scan /sys/class/hidraw for ALL connected DualSense controllers (VID 0x054C,
 * PID 0x0CE6 / 0x0DF2). Returns count written into out[] (<= max), or -1. */
int ds4l_hidraw_list(ds4l_hid_info *out, int max);

/* Open a specific /dev/hidrawN (O_RDWR|O_NONBLOCK). Returns fd or -1. */
int ds4l_hidraw_open_dev(const char *devnode);

/* Feature reports, as read by hid-playstation (see driver/hid-playstation.c). */

/* Get the controller Bluetooth MAC (feature report 0x09). Returns 0 on
 * success; mac[] is in report order (buf[1..6]). */
int ds4l_get_mac(int fd, uint8_t mac[6]);

/* Get firmware/hardware/update versions (feature report 0x20). Returns 0. */
int ds4l_get_fw(int fd, uint32_t *hw_version, uint32_t *fw_version,
                uint16_t *update_version);

#endif /* DS4L_HIDRAW_H */
