/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — hidraw.c : enumerate every connected DualSense over hidraw.
 * Copyright (c) 2026 Macauley Lim
 *
 * Lists every connected DualSense (the daemon manages several at once) and
 * reads the feature reports needed for identity: MAC (0x09) and firmware
 * info (0x20), as hid-playstation does.
 */
#define _GNU_SOURCE

#include "hidraw.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/hidraw.h>

#define DS5_VID   0x054Cu
#define DS5_PID_A 0x0CE6u   /* DualSense      */
#define DS5_PID_B 0x0DF2u   /* DualSense Edge */

/* Feature report ids / layouts (DualSense wire format). */
#define DS5_FEATURE_PAIRING_INFO         0x09
#define DS5_FEATURE_PAIRING_INFO_SIZE    20
#define DS5_FEATURE_FIRMWARE_INFO        0x20
#define DS5_FEATURE_FIRMWARE_INFO_SIZE   64

int ds4l_hidraw_open_dev(const char *devnode)
{
    return open(devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* sysfs enumeration */

/* Parse "HID_ID=<bus>:<vendor>:<product>" (hex) from a uevent blob. */
static int parse_hid_id(const char *uevent, unsigned *bus,
                        unsigned *vendor, unsigned *product)
{
    const char *p = uevent;
    while (p && *p) {
        if (strncmp(p, "HID_ID=", 7) == 0) {
            unsigned long b, v, pr;
            char *end;
            const char *q = p + 7;

            b = strtoul(q, &end, 16);
            if (end == q || *end != ':')
                return 0;
            q = end + 1;
            v = strtoul(q, &end, 16);
            if (end == q || *end != ':')
                return 0;
            q = end + 1;
            pr = strtoul(q, &end, 16);
            if (end == q)
                return 0;

            *bus    = (unsigned)b;
            *vendor = (unsigned)v;
            *product = (unsigned)pr;
            return 1;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    size_t total = 0;
    while (total + 1 < bufsz) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return 0;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    close(fd);
    return 1;
}

int ds4l_hidraw_list(ds4l_hid_info *out, int max)
{
    if (!out || max <= 0)
        return -1;

    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        int idx;
        if (sscanf(de->d_name, "hidraw%d", &idx) != 1 || idx < 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent",
                 de->d_name);

        char uevent[1024];
        if (!read_sysfs(path, uevent, sizeof(uevent)))
            continue;

        unsigned bus, vid, pid;
        if (!parse_hid_id(uevent, &bus, &vid, &pid))
            continue;
        if (vid != DS5_VID || (pid != DS5_PID_A && pid != DS5_PID_B))
            continue;

        ds4l_hid_info *info = &out[n];
        snprintf(info->node, sizeof(info->node), "hidraw%d", idx);
        snprintf(info->dev,  sizeof(info->dev),  "/dev/hidraw%d", idx);
        info->bus = bus;
        info->vid = vid;
        info->pid = pid;
        n++;
    }

    closedir(d);
    return n;
}

/* feature reports */

/* HIDIOCGFEATURE with report id in buf[0]. Bluetooth feature reports carry a
 * trailing CRC32 (the in-kernel driver's ps_get_report accounts for it), so on
 * BT try one byte longer first; fall back until the controller answers with
 * enough payload. Returns reply length or -1. */
static int feature_get(int fd, uint8_t report_id, uint8_t *buf, size_t bufsz,
                       size_t min_payload)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        size_t len = bufsz + (size_t)attempt;   /* 20, 21, 22 on BT etc. */
        memset(buf, 0, len);
        buf[0] = report_id;
        int rc = ioctl(fd, HIDIOCGFEATURE((int)len), buf);
        if (rc >= (int)min_payload)
            return rc;
        if (rc < 0 && errno != EPIPE && errno != EIO && errno != EINVAL)
            return -1;
    }
    return -1;
}

int ds4l_get_mac(int fd, uint8_t mac[6])
{
    uint8_t buf[DS5_FEATURE_PAIRING_INFO_SIZE + 4];
    int n = feature_get(fd, DS5_FEATURE_PAIRING_INFO, buf,
                        DS5_FEATURE_PAIRING_INFO_SIZE,
                        1 + 6);          /* report id + MAC */
    if (n < 0)
        return -1;

    /* MAC follows the report id: buf[1..6] (same order hid-playstation uses). */
    memcpy(mac, buf + 1, 6);
    return 0;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ds4l_get_fw(int fd, uint32_t *hw_version, uint32_t *fw_version,
                uint16_t *update_version)
{
    uint8_t buf[DS5_FEATURE_FIRMWARE_INFO_SIZE + 4];
    int n = feature_get(fd, DS5_FEATURE_FIRMWARE_INFO, buf,
                        DS5_FEATURE_FIRMWARE_INFO_SIZE,
                        1 + 46);         /* report id + ... + update ver @44 */
    if (n < 0)
        return -1;

    if (hw_version)
        *hw_version = get_le32(buf + 24);
    if (fw_version)
        *fw_version = get_le32(buf + 28);
    if (update_version)
        *update_version = (uint16_t)(buf[44] | ((uint16_t)buf[45] << 8));
    return 0;
}
