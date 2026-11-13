/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — manager.c : multi-controller device manager.
 * Copyright (c) 2026 Macauley Lim
 *
 * Owns the set of connected controllers: udev hotplug, per-device hidraw
 * transport and identity (MAC, firmware), the output thread (ds5 core) and,
 * for Bluetooth pads only, the PipeWire nodes (USB pads already show up as a
 * real sound card). Also the audio gate: audio reports are only sent while the
 * DS4L kernel module is loaded.
 */
#define _GNU_SOURCE

#include "manager.h"

#include "pw_nodes.h"
#include "ds5/ds5.h"

#ifdef DS4L_HAVE_UDEV
#include <libudev.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { LOG_ERR = 'e', LOG_WARN = 'w', LOG_INFO = 'i', LOG_DBG = 'd' };

struct ds4l_dev {
    struct ds4l_devinfo info;
    ds5_device     *dev;
    ds4l_pw_nodes  *pw;     /* per-controller PipeWire nodes (BT) */
};

struct ds4l_mgr {
    pthread_mutex_t lock;
    struct ds4l_dev *devs[DS4L_MAX_DEVS];
    int             n;

    ds4l_log_fn     log;
    void           *log_ud;

#ifdef DS4L_HAVE_UDEV
    struct udev            *udev;
    struct udev_monitor    *mon;
#endif

    int             dkms_busy;      /* install thread running               */
    int             auto_dkms;      /* auto-install allowed                 */
    int             keepalive;      /* default haptic keepalive, see ds5.h  */
};

/* logging */

static void mlog(ds4l_mgr *m, int lvl, const char *fmt, ...)
{
    if (!m->log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m->log(lvl, buf, m->log_ud);
}

/* audio-mode gate */

int ds4l_mgr_audio_safe(ds4l_mgr *m)
{
    (void)m;
    return ds4l_dkms_probe() == DS4L_DKMS_FORK_LOADED;
}

/* Open or close a device's audio gate. Caller holds m->lock. */
static void set_audio(ds4l_mgr *m, struct ds4l_dev *d, int on)
{
    on = on && d->info.bus == DS5_BUS_BT;
    if (on == d->info.audio_mode)
        return;
    if (on) {
        /* The PipeWire node volume is the user-facing control, so pin the
         * hardware volume high. Mute is left alone: the kernel driver owns
         * the mute button, its LED and the mic-mute bit. */
        ds5_set_audio_output(d->dev, DS5_AUDIO_OUT_SPEAKER);
        ds5_set_volume(d->dev, 0xff, 0x7f);
    }
    ds5_set_audio_mode(d->dev, on);
    d->info.audio_mode = on;
    mlog(m, LOG_INFO, "%s: audio %s", d->info.id, on ? "enabled" : "disabled");
}

/* lifecycle */

static void dev_destroy(ds4l_mgr *m, struct ds4l_dev *d)
{
    (void)m;
    /* PipeWire nodes first: their callbacks reference the device. */
    ds4l_pw_stop(d->pw);
    /* Then the device core: joins its thread and sends mic-off before closing. */
    ds5_close(d->dev);
    free(d);
}

/* Attach the PipeWire nodes (Bluetooth only). Call without m->lock held: this
 * takes the PipeWire loop lock, and a callback on that thread may be waiting
 * for m->lock. */
static void dev_pw_attach(ds4l_mgr *m, struct ds4l_dev *d)
{
    if (d->info.bus != DS5_BUS_BT) {
        mlog(m, LOG_INFO, "%s: USB controller — native USB sound card in use, "
             "no virtual audio nodes", d->info.id);
        return;
    }
    if (!ds4l_pw_available() || ds4l_pw_init() != 0)
        return;
    if (ds4l_pw_start(&d->pw, d->dev, d->info.id) != 0)
        mlog(m, LOG_WARN, "%s: PipeWire nodes failed (audio disabled for "
             "this device; control still works)", d->info.id);
}

static struct ds4l_dev *dev_create(ds4l_mgr *m, const ds4l_hid_info *hi)
{
    int fd = ds4l_hidraw_open_dev(hi->dev);
    if (fd < 0) {
        mlog(m, LOG_WARN, "%s: cannot open %s: %s", hi->node, hi->dev,
             strerror(errno));
        return NULL;
    }

    struct ds4l_dev *d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return NULL;
    }
    d->info.bus = (hi->bus == 3) ? DS5_BUS_USB : DS5_BUS_BT;
    snprintf(d->info.node, sizeof(d->info.node), "%s", hi->node);

    /* Identity: MAC (fallback: hidraw node name), firmware info (optional). */
    if (ds4l_get_mac(fd, d->info.mac) == 0) {
        d->info.have_mac = 1;
        snprintf(d->info.id, sizeof(d->info.id),
                 "%02x%02x%02x%02x%02x%02x",
                 d->info.mac[0], d->info.mac[1], d->info.mac[2],
                 d->info.mac[3], d->info.mac[4], d->info.mac[5]);
    } else {
        snprintf(d->info.id, sizeof(d->info.id), "%s", hi->node);
    }
    if (ds4l_get_fw(fd, &d->info.hw_version, &d->info.fw_version,
                    &d->info.update_version) == 0)
        d->info.have_fw = 1;

    d->dev = ds5_open(fd, d->info.bus);   /* owns fd from here on */
    if (!d->dev) {
        mlog(m, LOG_WARN, "%s: ds5_open failed", d->info.id);
        free(d);
        return NULL;
    }

    ds5_set_haptic_keepalive(d->dev, m->keepalive);
    mlog(m, LOG_INFO, "%s: connected (%s)", d->info.id,
         d->info.bus == DS5_BUS_USB ? "USB" : "Bluetooth");
    if (ds4l_mgr_audio_safe(m))
        set_audio(m, d, 1);
    else if (d->info.bus == DS5_BUS_BT)
        mlog(m, LOG_WARN, "%s: audio disabled, the stock hid-playstation "
             "module is loaded (run: ds4l dkms install)", d->info.id);
    return d;
}

ds4l_mgr *ds4l_mgr_create(ds4l_log_fn log, void *log_ud)
{
    ds4l_mgr *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    pthread_mutex_init(&m->lock, NULL);
    m->log    = log;
    m->log_ud = log_ud;
    m->auto_dkms = 1;
    m->keepalive = 1;
    return m;
}

void ds4l_mgr_set_haptic_keepalive(ds4l_mgr *m, int n)
{
    m->keepalive = n;
}

void ds4l_mgr_destroy(ds4l_mgr *m)
{
    if (!m)
        return;
    for (int i = 0; i < m->n; i++)
        dev_destroy(m, m->devs[i]);
    m->n = 0;
#ifdef DS4L_HAVE_UDEV
    if (m->mon)
        udev_monitor_unref(m->mon);
    if (m->udev)
        udev_unref(m->udev);
#endif
    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* hotplug */

#ifndef DS4L_HAVE_UDEV

int ds4l_mgr_hotplug_fd(ds4l_mgr *m)
{
    (void)m;
    return -1;  /* caller falls back to periodic rescan */
}

void ds4l_mgr_hotplug_handle(ds4l_mgr *m)
{
    (void)m;
}

#else /* DS4L_HAVE_UDEV */

int ds4l_mgr_hotplug_fd(ds4l_mgr *m)
{
    if (!m->mon) {
        m->udev = udev_new();
        if (!m->udev)
            return -1;
        m->mon = udev_monitor_new_from_netlink(m->udev, "udev");
        if (!m->mon || udev_monitor_filter_add_match_subsystem_devtype(
                           m->mon, "hidraw", NULL) < 0 ||
            udev_monitor_enable_receiving(m->mon) < 0) {
            if (m->mon) udev_monitor_unref(m->mon);
            m->mon = NULL;
            udev_unref(m->udev);
            m->udev = NULL;
            return -1;
        }
    }
    return udev_monitor_get_fd(m->mon);
}

void ds4l_mgr_hotplug_handle(ds4l_mgr *m)
{
    if (!m->mon)
        return;
    struct udev_device *ud;
    while ((ud = udev_monitor_receive_device(m->mon)) != NULL)
        udev_device_unref(ud);
    /* Coalesce: just rescan. */
    ds4l_mgr_rescan(m);
}

#endif /* DS4L_HAVE_UDEV */

void ds4l_mgr_rescan(ds4l_mgr *m)
{
    ds4l_hid_info present[DS4L_MAX_DEVS];
    int np = ds4l_hidraw_list(present, DS4L_MAX_DEVS);
    if (np < 0)
        np = 0;

    pthread_mutex_lock(&m->lock);

    /* Remove devices whose hidraw node is gone. Detach under the lock but
     * destroy outside it (teardown takes the PipeWire loop lock; see above). */
    struct ds4l_dev *victims[DS4L_MAX_DEVS];
    int nv = 0;
    for (int i = 0; i < m->n; ) {
        /* A dead device whose node name is present again was replaced by a
         * reconnect that reused the name: drop the stale handle. */
        int found = 0;
        for (int j = 0; j < np && ds5_alive(m->devs[i]->dev); j++)
            if (strcmp(m->devs[i]->info.node, present[j].node) == 0)
                { found = 1; break; }
        if (!found) {
            struct ds4l_dev *d = m->devs[i];
            mlog(m, LOG_INFO, "%s: disconnected", d->info.id);
            victims[nv++] = d;
            memmove(&m->devs[i], &m->devs[i + 1],
                    (size_t)(m->n - i - 1) * sizeof(m->devs[0]));
            m->n--;
        } else {
            i++;
        }
    }

    /* Add new devices. dev_create takes no PipeWire locks, so it is safe under
     * m->lock; the PipeWire attachment waits until after the unlock. */
    struct ds4l_dev *created[DS4L_MAX_DEVS];
    int nc = 0;
    for (int j = 0; j < np && m->n < DS4L_MAX_DEVS; j++) {
        int known = 0;
        for (int i = 0; i < m->n; i++)
            if (strcmp(m->devs[i]->info.node, present[j].node) == 0)
                { known = 1; break; }
        if (known)
            continue;

        struct ds4l_dev *d = dev_create(m, &present[j]);
        if (d) {
            m->devs[m->n++] = d;
            created[nc++] = d;
        }
    }

    pthread_mutex_unlock(&m->lock);

    /* PipeWire attachment and deferred teardown, outside m->lock. d->pw is only
     * touched on this thread; pw callbacks read d->dev only. */
    for (int i = 0; i < nc; i++)
        dev_pw_attach(m, created[i]);

    for (int i = 0; i < nv; i++)
        dev_destroy(m, victims[i]);
}

int ds4l_mgr_list(ds4l_mgr *m, struct ds4l_devinfo *out, int max)
{
    pthread_mutex_lock(&m->lock);
    int n = m->n < max ? m->n : max;
    for (int i = 0; i < n; i++)
        out[i] = m->devs[i]->info;
    pthread_mutex_unlock(&m->lock);
    return n;
}

struct ds4l_dev *ds4l_mgr_find(ds4l_mgr *m, const char *id)
{
    struct ds4l_dev *hit = NULL;
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->n; i++) {
        const char *did = m->devs[i]->info.id;
        if (!id || !*id) {
            hit = m->devs[0];          /* default: first device */
            break;
        }
        if (strcmp(did, id) == 0) {
            hit = m->devs[i];          /* exact */
            break;
        }
        if (strncasecmp(did, id, strlen(id)) == 0) {
            if (hit) { hit = (void *)-1; break; }  /* ambiguous prefix */
            hit = m->devs[i];
        }
    }
    pthread_mutex_unlock(&m->lock);
    return hit == (void *)-1 ? NULL : hit;
}

/* DKMS admin */

static void *dkms_install_thread(void *arg)
{
    ds4l_mgr *m = arg;
    mlog(m, LOG_INFO, "dkms: building + installing the DS4L module "
         "(may prompt for admin auth and take a minute)...");

    int rc = ds4l_dkms_ensure();

    mlog(m, rc == 0 ? LOG_INFO : LOG_ERR,
         rc == 0 ? "dkms: DS4L hid-playstation installed and loaded"
                 : "dkms: install/reload failed (see the helper output)");

    /* Reloading the module normally re-creates every hidraw node (and so
     * every device); this covers the ones that survived. */
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->n; i++)
        set_audio(m, m->devs[i], ds4l_mgr_audio_safe(m));
    pthread_mutex_unlock(&m->lock);

    m->dkms_busy = 0;
    return NULL;
}

/* command handling */

static int parse_u8(const char *s, uint8_t *out)
{
    char *end;
    long v = strtol(s, &end, 0);
    if (*s == '\0' || *end != '\0' || v < 0 || v > 255)
        return -1;
    *out = (uint8_t)v;
    return 0;
}

static int parse_onoff(const char *s, int *out)
{
    if (!strcasecmp(s, "on") || !strcmp(s, "1"))  { *out = 1; return 0; }
    if (!strcasecmp(s, "off") || !strcmp(s, "0")) { *out = 0; return 0; }
    return -1;
}

static int parse_side(const char *s, ds5_side *out)
{
    if (!strcasecmp(s, "l") || !strcasecmp(s, "left"))  { *out = DS5_SIDE_LEFT;  return 0; }
    if (!strcasecmp(s, "r") || !strcasecmp(s, "right")) { *out = DS5_SIDE_RIGHT; return 0; }
    return -1;
}

/* Player-LED masks, centered like the kernel driver maps player ids. */
static const uint8_t PLAYER_LED_MASKS[5] = {
    1u << 2,
    (1u << 3) | (1u << 1),
    (1u << 4) | (1u << 2) | (1u << 0),
    (1u << 4) | (1u << 3) | (1u << 1) | (1u << 0),
    0x1f,
};

static const char *bus_str(ds5_bus b) { return b == DS5_BUS_USB ? "usb" : "bt"; }

/* Public (used by ctl.c for "list"). */
void ds4l_devinfo_status(const struct ds4l_devinfo *i, char *out, size_t n)
{
    char mac[24] = "-";
    if (i->have_mac)
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 i->mac[0], i->mac[1], i->mac[2],
                 i->mac[3], i->mac[4], i->mac[5]);
    snprintf(out, n,
             "id=%s node=%s bus=%s mac=%s audio=%d",
             i->id, i->node, bus_str(i->bus), mac, i->audio_mode);
}

int ds4l_mgr_command(ds4l_mgr *m, struct ds4l_dev *dev,
                     int argc, char **argv, char *resp, size_t respsz)
{
    if (argc < 1 || !argv[0]) {
        snprintf(resp, respsz, "missing command");
        return -1;
    }
    const char *cmd = argv[0];
    ds5_device *d = dev->dev;
    resp[0] = '\0';

    /* status */
    if (!strcmp(cmd, "status")) {
        ds4l_devinfo_status(&dev->info, resp, respsz);
        ds5_stats st;
        ds5_get_stats(d, &st);
        size_t len = strlen(resp);
        snprintf(resp + len, respsz - len,
                 " frames=%u underrun=%u drop=%u wfail=%u late=%u "
                 "maxgap=%ums sendlate=%u sendmax=%uus in=%u",
                 st.frames_sent, st.underruns, st.dropped, st.write_fail,
                 st.state_late, st.state_gap_max, st.send_late,
                 st.send_late_max, st.in_reports);
        return 0;
    }

    /* input state */
    if (!strcmp(cmd, "input")) {
        ds5_input_state in;
        if (!ds5_get_input(d, &in)) {
            snprintf(resp, respsz, "no input reports yet");
            return -1;
        }
        snprintf(resp, respsz,
                 "sticks=%u,%u,%u,%u l2=%u r2=%u dpad=%u btn=0x%x "
                 "gyro=%d,%d,%d accel=%d,%d,%d bat=%u chg=%u hp=%u "
                 "mic=%u muted=%u trig=%u/%u,%u/%u",
                 in.lx, in.ly, in.rx, in.ry, in.l2, in.r2, in.dpad,
                 in.buttons,
                 in.gyro[0], in.gyro[1], in.gyro[2],
                 in.accel[0], in.accel[1], in.accel[2],
                 in.battery_percent, in.charging, in.headphones,
                 in.mic_plugged, in.mic_muted,
                 in.left_trigger_effect, in.left_trigger_status,
                 in.right_trigger_effect, in.right_trigger_status);
        return 0;
    }

    /* audio-mode manual override */
    if (!strcmp(cmd, "audio")) {
        int on;
        if (argc < 2 || parse_onoff(argv[1], &on)) {
            snprintf(resp, respsz, "usage: audio on|off");
            return -1;
        }
        if (on && dev->info.bus == DS5_BUS_USB) {
            snprintf(resp, respsz,
                     "refused: audio over USB is native — the controller's "
                     "real USB sound card is used, no virtual audio needed");
            return -1;
        }
        if (on && !ds4l_mgr_audio_safe(m)) {
            snprintf(resp, respsz,
                     "refused: stock hid-playstation loaded "
                     "(run: ds4l dkms install)");
            return -1;
        }
        pthread_mutex_lock(&m->lock);
        set_audio(m, dev, on);
        pthread_mutex_unlock(&m->lock);
        return 0;
    }

    if (!strcmp(cmd, "keepalive")) {
        uint8_t n;
        if (argc < 2 || parse_u8(argv[1], &n) || n > 3) {
            snprintf(resp, respsz, "usage: keepalive 0..3");
            return -1;
        }
        ds5_set_haptic_keepalive(d, n);
        return 0;
    }

    /* adaptive triggers */
    if (!strcmp(cmd, "trigger")) {
        if (argc < 3) {
            snprintf(resp, respsz,
                     "usage: trigger l|r off | feedback pos str | "
                     "weapon start stop str | vibration pos amp freq | "
                     "raw mode b0..b9");
            return -1;
        }
        ds5_side side;
        if (parse_side(argv[1], &side)) {
            snprintf(resp, respsz, "side must be l|r");
            return -1;
        }
        const char *mode = argv[2];
        uint8_t v[11] = {0};
        int nv = argc - 3;
        if (nv > 11)
            nv = 11;
        for (int i = 0; i < nv; i++)
            if (parse_u8(argv[3 + i], &v[i])) {
                snprintf(resp, respsz, "bad number: %s", argv[3 + i]);
                return -1;
            }

        uint8_t blk[DS5_TRIGGER_BLOCK_BYTES];
        if (!strcmp(mode, "off") && nv == 0)
            ds5_trig_off(blk);
        else if (!strcmp(mode, "feedback") && nv == 2)
            ds5_trig_feedback(blk, v[0], v[1]);
        else if (!strcmp(mode, "weapon") && nv == 3)
            ds5_trig_weapon(blk, v[0], v[1], v[2]);
        else if (!strcmp(mode, "vibration") && nv == 3)
            ds5_trig_vibration(blk, v[0], v[1], v[2]);
        else if (!strcmp(mode, "raw") && nv >= 1)
            ds5_trig_raw(blk, v[0], v + 1, (size_t)nv - 1);
        else {
            snprintf(resp, respsz, "unknown trigger mode / wrong arg count");
            return -1;
        }
        ds5_trigger_effect fx;
        fx.mode = blk[0];
        memcpy(fx.param, blk + 1, sizeof fx.param);
        ds5_set_trigger(d, side, &fx);
        return 0;
    }

    /* LEDs / rumble */
    if (!strcmp(cmd, "lightbar")) {
        uint8_t r, g, b;
        if (argc < 4 || parse_u8(argv[1], &r) || parse_u8(argv[2], &g) ||
            parse_u8(argv[3], &b)) {
            snprintf(resp, respsz, "usage: lightbar R G B (0-255)");
            return -1;
        }
        ds5_set_lightbar(d, r, g, b);
        return 0;
    }
    if (!strcmp(cmd, "playerleds")) {
        if (argc < 2) {
            snprintf(resp, respsz, "usage: playerleds mask|p1..p5");
            return -1;
        }
        uint8_t mask;
        if (!strncasecmp(argv[1], "p", 1) && strlen(argv[1]) == 2 &&
            argv[1][1] >= '1' && argv[1][1] <= '5') {
            mask = PLAYER_LED_MASKS[argv[1][1] - '1'];
        } else if (parse_u8(argv[1], &mask)) {
            snprintf(resp, respsz, "usage: playerleds mask|p1..p5");
            return -1;
        }
        ds5_set_player_leds(d, mask);
        return 0;
    }
    if (!strcmp(cmd, "rumble")) {
        uint8_t strong, weak;
        if (argc < 3 || parse_u8(argv[1], &strong) || parse_u8(argv[2], &weak)) {
            snprintf(resp, respsz, "usage: rumble strong weak (0-255)");
            return -1;
        }
        ds5_set_rumble(d, strong, weak);
        return 0;
    }
    if (!strcmp(cmd, "micled")) {
        int on;
        if (argc < 2 || parse_onoff(argv[1], &on)) {
            snprintf(resp, respsz, "usage: micled on|off");
            return -1;
        }
        ds5_set_mic_mute_led(d, on);
        return 0;
    }
    if (!strcmp(cmd, "power")) {
        uint8_t rr, tr;
        if (argc < 3 || parse_u8(argv[1], &rr) || parse_u8(argv[2], &tr)) {
            snprintf(resp, respsz, "usage: power rumble_reduce trigger_reduce (0-7)");
            return -1;
        }
        ds5_set_motor_power(d, rr, tr);
        return 0;
    }

    /* audio controls */
    if (!strcmp(cmd, "volume")) {
        uint8_t s, h;
        if (argc < 3 || parse_u8(argv[1], &s) || parse_u8(argv[2], &h)) {
            snprintf(resp, respsz, "usage: volume speaker(0-255) headphone(0-127)");
            return -1;
        }
        ds5_set_volume(d, s, h);
        return 0;
    }
    if (!strcmp(cmd, "micvol")) {
        uint8_t v;
        if (argc < 2 || parse_u8(argv[1], &v)) {
            snprintf(resp, respsz, "usage: micvol 0..64");
            return -1;
        }
        ds5_set_mic_volume(d, v);
        return 0;
    }
    if (!strcmp(cmd, "route")) {
        if (argc < 2) {
            snprintf(resp, respsz, "usage: route hp|speaker|both");
            return -1;
        }
        if (!strcmp(argv[1], "hp") || !strcmp(argv[1], "headphone"))
            ds5_set_audio_output(d, DS5_AUDIO_OUT_HEADPHONE);
        else if (!strcmp(argv[1], "spk") || !strcmp(argv[1], "speaker"))
            ds5_set_audio_output(d, DS5_AUDIO_OUT_SPEAKER);
        else if (!strcmp(argv[1], "both"))
            ds5_set_audio_output(d, DS5_AUDIO_OUT_BOTH);
        else {
            snprintf(resp, respsz, "route must be hp|speaker|both");
            return -1;
        }
        return 0;
    }
    if (!strcmp(cmd, "mute")) {
        int mm, sm, hm;
        if (argc < 4 || parse_onoff(argv[1], &mm) || parse_onoff(argv[2], &sm) ||
            parse_onoff(argv[3], &hm)) {
            snprintf(resp, respsz, "usage: mute mic|spk|hp on|off (x3)");
            return -1;
        }
        ds5_set_mute(d, mm, sm, hm);
        return 0;
    }

    snprintf(resp, respsz, "unknown command: %s", cmd);
    return -1;
}

int ds4l_mgr_admin(ds4l_mgr *m, int argc, char **argv,
                   char *resp, size_t respsz)
{
    if (argc < 1) {
        snprintf(resp, respsz, "missing admin command");
        return -1;
    }

    if (!strcmp(argv[0], "dkms")) {
        const char *sub = argc > 1 ? argv[1] : "status";
        if (!strcmp(sub, "status")) {
            snprintf(resp, respsz, "loaded=%s installed=%s audio_safe=%d",
                     ds4l_dkms_state_str(ds4l_dkms_probe()),
                     ds4l_dkms_install_str(ds4l_dkms_installed()),
                     ds4l_mgr_audio_safe(m));
            return 0;
        }
        if (!strcmp(sub, "install")) {
            if (ds4l_mgr_audio_safe(m)) {
                snprintf(resp, respsz, "already installed and loaded");
                return 0;
            }
            if (m->dkms_busy) {
                snprintf(resp, respsz, "install already running");
                return -1;
            }
            pthread_t th;
            m->dkms_busy = 1;
            if (pthread_create(&th, NULL, dkms_install_thread, m) != 0) {
                m->dkms_busy = 0;
                snprintf(resp, respsz, "cannot spawn install thread");
                return -1;
            }
            pthread_detach(th);
            snprintf(resp, respsz,
                     "install started (admin auth may be required; "
                     "check the daemon log)");
            return 0;
        }
        snprintf(resp, respsz, "usage: dkms status|install");
        return -1;
    }

    snprintf(resp, respsz, "unknown admin command: %s", argv[0]);
    return -1;
}
