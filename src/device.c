/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — device.c : per-controller device core.
 * Copyright (c) 2026 Macauley Lim
 *
 * One I/O thread per controller sleeps in ppoll() on the hidraw fd and an
 * eventfd. It parses input reports as they arrive and writes output reports:
 * paced audio frames, mic on/off, and state changes. d->lock guards all
 * shared state and is never held across a syscall that can block.
 */
#define _GNU_SOURCE

#include "ds5/ds5.h"
#include "ds5/ds5_audio.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define FRAME_NS      (1000000000LL * 512 / 48000)   /* 10.67 ms */
#define QUEUE_FRAMES  16
#define PRIME_FRAMES  4      /* queued before a stream starts            */
#define TARGET_FRAMES 2      /* queue level the send clock steers toward */
#define SILENCE_HOLD  24000  /* samples of silence before we stop sending */

#define MIC_RING      4096   /* mono samples, ~85 ms */
#define MIC_FRAME     480
#define MIC_OPUS_LEN  71

#define MUTE_MIC        (1u << 4)   /* power_save_control bits */
#define MUTE_SPEAKER    (1u << 5)
#define MUTE_HEADPHONE  (1u << 6)

struct ds5_device {
    int     fd, wake_fd;
    ds5_bus bus;

    pthread_t  thread;
    atomic_int running, dead, audio_mode;

    pthread_mutex_t lock;

    /* Pending state: nonzero valid flags mean "send me". */
    struct ds5_output_common state;
    uint8_t seq, counter;

    /* Audio frames waiting to go out. due == 0 while the queue is priming. */
    struct ds5_audio_frame q[QUEUE_FRAMES];
    int     q_head, q_count;
    int64_t due;
    int64_t state_due;   /* next bare state report while haptics play       */
    int64_t state_gap;   /* spacing between them                            */
    int     state_left;  /* how many more of them this frame                */
    int     keepalive;   /* extra state reports per haptic frame            */

    /* Each state report carries a fresh host timestamp, which the controller
     * echoes in its input reports: the time between echo changes shows how
     * evenly state is arriving while haptics play. */
    uint32_t stamp, echo;
    int64_t  echo_ns;    /* when the echo last changed; 0 = not tracking    */
    int      hap_live;   /* the last frame sent carried haptics             */

    int mic_on;      /* an app is capturing                  */
    int mic_told;    /* what the controller was last told    */
    int16_t mic_ring[MIC_RING];
    unsigned mic_rd, mic_wr;         /* free-running, index % MIC_RING */
    int mic_started;

    ds5_input_state input;
    int             have_input;
    ds5_stats       stats;

    /* ds5_push_audio() caller's thread only. */
    ds5_audio *audio;
    int        silence;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void wake(ds5_device *d)
{
    eventfd_write(d->wake_fd, 1);
}

/* input */

static int16_t le16(const uint8_t *p)
{
    return (int16_t)(p[0] | p[1] << 8);
}

/* `s` is the input struct shared by the BT 0x31 and USB 0x01 reports. */
static void parse_input(ds5_input_state *in, const uint8_t *s)
{
    memset(in, 0, sizeof *in);
    in->lx = s[0];
    in->ly = s[1];
    in->rx = s[2];
    in->ry = s[3];
    in->l2 = s[4];
    in->r2 = s[5];
    in->dpad = s[7] & 0x0f;
    /* The wire bit order matches the DS5_BTN_* order. */
    in->buttons = (s[7] >> 4) | s[8] << 4 | (s[9] & 7) << 12;
    for (int i = 0; i < 3; i++) {
        in->gyro[i]  = le16(s + 15 + i * 2);
        in->accel[i] = le16(s + 21 + i * 2);
    }
    in->right_trigger_status = s[41] >> 4;
    in->left_trigger_status  = s[42] >> 4;
    in->right_trigger_effect = s[47] & 0x0f;
    in->left_trigger_effect  = s[47] >> 4;

    int level = s[52] & 0x0f, charge = s[52] >> 4;
    int pct = level * 10 + 5;
    if (charge == 2)
        pct = 100;                 /* full */
    else if (charge > 2)
        pct = 0;                   /* charge error / temperature fault */
    in->battery_percent = pct > 100 ? 100 : pct;
    in->charging = charge <= 2 ? charge : 0;

    in->headphones  = s[53] & 1;
    in->mic_plugged = s[53] >> 1 & 1;
    in->mic_muted   = s[53] >> 2 & 1;
}

static void read_input(ds5_device *d)
{
    uint8_t r[128];
    ssize_t n;

    while ((n = read(d->fd, r, sizeof r)) > 0) {
        int bt = d->bus == DS5_BUS_BT;

        /* In audio mode some 0x31 reports carry a mic Opus frame instead of
         * the gamepad struct. */
        if (bt && n >= 3 + MIC_OPUS_LEN && r[0] == 0x31 && (r[1] & 2)) {
            int16_t pcm[MIC_FRAME];
            int got = ds5_audio_decode_mic(d->audio, r + 3, MIC_OPUS_LEN,
                                           pcm, MIC_FRAME);
            pthread_mutex_lock(&d->lock);
            for (int i = 0; i < got; i++)
                d->mic_ring[d->mic_wr++ % MIC_RING] = pcm[i];
            if (d->mic_wr - d->mic_rd > MIC_RING)      /* overrun: drop oldest */
                d->mic_rd = d->mic_wr - MIC_RING;
            pthread_mutex_unlock(&d->lock);
            continue;
        }

        int base = bt ? 2 : 1;
        if (r[0] != (bt ? 0x31 : 0x01) || n < base + 54)
            continue;
        uint32_t echo;
        memcpy(&echo, r + base + 43, sizeof echo);
        pthread_mutex_lock(&d->lock);
        parse_input(&d->input, r + base);
        d->have_input = 1;
        d->stats.in_reports++;
        if (echo != d->echo) {
            int64_t t = now_ns();
            unsigned ms = (unsigned)((t - d->echo_ns) / 1000000);
            if (d->echo_ns && d->hap_live) {
                d->stats.state_late += ms > 20;
                if (ms > d->stats.state_gap_max)
                    d->stats.state_gap_max = ms;
            }
            d->echo = echo;
            d->echo_ns = d->hap_live ? t : 0;
        }
        pthread_mutex_unlock(&d->lock);
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        d->dead = 1;
}

/* output */

/* Write at most one report. Returns 1 if something was sent; *deadline is
 * when the next audio report is due (0 = none scheduled). */
static int send_output(ds5_device *d, int64_t *deadline)
{
    uint8_t buf[DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE];
    size_t  len = 0;
    int     bt = d->bus == DS5_BUS_BT;

    pthread_mutex_lock(&d->lock);
    int64_t now = now_ns();
    int mic = d->mic_on && d->audio_mode;
    struct ds5_output_common *st = &d->state;

    /* The controller drives its actuators only for about 17 ms after a state
     * report, and delivery over the radio is uneven: the adapter's scan
     * windows stall it for 11 ms a few times a second, and big reports make
     * it worse. So a frame is sent in the smallest form that carries it:
     *   haptics only    0x32, then state reports spread over the frame
     *   haptics+speaker 0x36 with the pending state embedded, then
     *                   `keepalive` more state reports
     *   speaker only    paired into 0x39 at half the report rate, no state;
     *                   a leftover frame is flushed once no partner shows up
     */
    struct ds5_audio_frame *f0 = &d->q[d->q_head];
    struct ds5_audio_frame *f1 = &d->q[(d->q_head + 1) % QUEUE_FRAMES];
    int lone = d->q_count && (f0->has_haptics ||
                              (d->q_count >= 2 && f1->has_haptics));
    int nf = 0;
    if (!d->due && d->q_count >= PRIME_FRAMES)
        d->due = now;
    if (d->due && now >= d->due) {
        if (d->q_count == 0) {
            d->due = 0;                       /* ran dry: prime again */
            d->hap_live = 0;
            d->stats.underruns++;
        } else if (lone || (d->q_count == 1 && now >= d->due + FRAME_NS)) {
            nf = 1;
        } else if (d->q_count >= 2) {
            nf = 2;
        }
    }

    if (nf) {
        struct ds5_audio_frame f[2] = { *f0, *f1 };
        int small = f[0].has_haptics && !f[0].has_speaker;
        d->q_head = (d->q_head + nf) % QUEUE_FRAMES;
        d->q_count -= nf;

        /* The counter leads a double report and trails a single one. */
        if (nf == 2)
            d->counter += 2;
        d->hap_live = nf == 1 && f[0].has_haptics;
        unsigned late_us = (unsigned)((now - d->due) / 1000);
        d->stats.send_late += late_us > 2000;
        if (late_us > d->stats.send_late_max)
            d->stats.send_late_max = late_us;
        st->host_timestamp = ++d->stamp;
        if (small)
            len = ds5_build_bt_haptics(buf, d->seq++, f, d->counter, mic);
        else
            len = ds5_build_bt_audio(buf, d->seq++, st, f, nf, d->counter,
                                     mic, d->input.headphones);
        if (nf == 1) {
            d->counter++;
            if (!small)
                st->valid_flag0 = st->valid_flag1 = st->valid_flag2 = 0;
            d->state_gap = FRAME_NS / (d->keepalive + 1);
            d->state_left = f[0].has_haptics ? d->keepalive + small : 0;
            d->state_due = !d->state_left ? 0 : small ? now : now + d->state_gap;
        }
        d->mic_told = mic;
        d->stats.frames_sent += nf;

        /* Frames are produced on the audio graph's clock, not ours: lean
         * the period 0.4% per frame of queue error so the two stay locked. */
        d->due += nf * (FRAME_NS +
                        (TARGET_FRAMES - d->q_count) * (FRAME_NS / 256));
        if (d->due < now - 4 * FRAME_NS)
            d->due = now;                     /* stalled: don't burst */
    } else if (bt && mic != d->mic_told) {
        len = ds5_build_bt_mic(buf, d->seq++, mic);
        d->mic_told = mic;
    } else if (st->valid_flag0 | st->valid_flag1 | st->valid_flag2 ||
               (d->state_due && now >= d->state_due)) {
        st->host_timestamp = ++d->stamp;
        len = bt ? ds5_build_bt_state(buf, st, d->seq++)
                 : ds5_build_usb_state(buf, st);
        st->valid_flag0 = st->valid_flag1 = st->valid_flag2 = 0;
        if (d->state_due && now >= d->state_due)
            d->state_due = --d->state_left ? now + d->state_gap : 0;
    }

    /* A lone speaker-only frame waits one more period for a partner. */
    int wait = d->q_count == 1 && !d->q[d->q_head].has_haptics;
    *deadline = !d->due ? 0 : d->due + (wait ? FRAME_NS : 0);
    if (d->state_due && (!*deadline || d->state_due < *deadline))
        *deadline = d->state_due;
    pthread_mutex_unlock(&d->lock);

    if (len && write(d->fd, buf, len) != (ssize_t)len) {
        pthread_mutex_lock(&d->lock);
        d->stats.write_fail++;
        pthread_mutex_unlock(&d->lock);
    }
    return len > 0;
}

static void *io_thread(void *arg)
{
    ds5_device *d = arg;
    struct pollfd p[2] = {
        { .fd = d->fd,      .events = POLLIN },
        { .fd = d->wake_fd, .events = POLLIN },
    };
    int64_t deadline = 0;

    while (d->running && !d->dead) {
        struct timespec ts, *tsp = NULL;
        if (deadline) {
            int64_t wait = deadline - now_ns();
            if (wait < 0)
                wait = 0;
            ts.tv_sec = wait / 1000000000;
            ts.tv_nsec = wait % 1000000000;
            tsp = &ts;
        }
        if (ppoll(p, 2, tsp, NULL) < 0 && errno != EINTR)
            break;

        if (p[1].revents) {
            eventfd_t v;
            eventfd_read(d->wake_fd, &v);
        }
        if (p[0].revents & POLLIN)
            read_input(d);
        else if (p[0].revents)
            d->dead = 1;                      /* POLLHUP/POLLERR: node gone */

        while (send_output(d, &deadline))
            ;
    }
    return NULL;
}

/* lifecycle */

ds5_device *ds5_open(int fd, ds5_bus bus)
{
    ds5_device *d = calloc(1, sizeof *d);
    if (!d) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    d->bus = bus;
    d->silence = SILENCE_HOLD;
    d->keepalive = 1;
    d->running = 1;
    d->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    d->audio = ds5_audio_create();
    pthread_mutex_init(&d->lock, NULL);

    if (d->wake_fd < 0 || !d->audio ||
        pthread_create(&d->thread, NULL, io_thread, d) != 0) {
        d->running = 0;
        ds5_close(d);
        return NULL;
    }
    return d;
}

void ds5_close(ds5_device *d)
{
    if (d->running) {
        d->running = 0;
        wake(d);
        pthread_join(d->thread, NULL);
    }
    /* Don't leave the controller streaming mic into its input reports. */
    if (d->mic_told && !d->dead) {
        uint8_t buf[DS5_OUT_REPORT_BT_CTL_SIZE];
        size_t len = ds5_build_bt_mic(buf, d->seq, 0);
        (void)!write(d->fd, buf, len);
    }
    close(d->fd);
    if (d->wake_fd >= 0)
        close(d->wake_fd);
    ds5_audio_destroy(d->audio);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int ds5_alive(const ds5_device *d)
{
    return !d->dead;
}

void ds5_set_audio_mode(ds5_device *d, int enable)
{
    pthread_mutex_lock(&d->lock);
    d->audio_mode = enable && d->bus == DS5_BUS_BT;
    if (!d->audio_mode)
        d->q_count = d->due = d->state_due = 0;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_haptic_keepalive(ds5_device *d, int n)
{
    pthread_mutex_lock(&d->lock);
    d->keepalive = n < 0 ? 0 : n > 3 ? 3 : n;
    pthread_mutex_unlock(&d->lock);
}

/* state setters */

void ds5_set_trigger(ds5_device *d, ds5_side side, const ds5_trigger_effect *fx)
{
    int left = side == DS5_SIDE_LEFT;

    pthread_mutex_lock(&d->lock);
    uint8_t *blk = left ? d->state.left_trigger : d->state.right_trigger;
    blk[0] = fx->mode;
    memcpy(blk + 1, fx->param, sizeof fx->param);
    d->state.valid_flag0 |= left ? DS5_VF0_LEFT_TRIGGER_FFB
                                 : DS5_VF0_RIGHT_TRIGGER_FFB;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_rumble(ds5_device *d, uint8_t left, uint8_t right)
{
    pthread_mutex_lock(&d->lock);
    d->state.motor_left = left;
    d->state.motor_right = right;
    d->state.valid_flag0 |= DS5_VF0_COMPATIBLE_VIBRATION | DS5_VF0_HAPTICS_SELECT;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_lightbar(ds5_device *d, uint8_t r, uint8_t g, uint8_t b)
{
    pthread_mutex_lock(&d->lock);
    d->state.lightbar_red = r;
    d->state.lightbar_green = g;
    d->state.lightbar_blue = b;
    d->state.valid_flag1 |= DS5_VF1_LIGHTBAR_CONTROL;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_player_leds(ds5_device *d, uint8_t mask)
{
    pthread_mutex_lock(&d->lock);
    d->state.player_leds = mask & 0x1f;
    d->state.valid_flag1 |= DS5_VF1_PLAYER_INDICATOR;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_mic_mute_led(ds5_device *d, int on)
{
    pthread_mutex_lock(&d->lock);
    d->state.mute_button_led = !!on;
    d->state.valid_flag1 |= DS5_VF1_MIC_MUTE_LED;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_motor_power(ds5_device *d, uint8_t rumble_reduce,
                         uint8_t trigger_reduce)
{
    pthread_mutex_lock(&d->lock);
    d->state.reduce_motor_power =
        (rumble_reduce & 0x0f) | (trigger_reduce & 0x0f) << 4;
    d->state.valid_flag1 |= DS5_VF1_MOTOR_POWER_LEVEL;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_volume(ds5_device *d, uint8_t speaker, uint8_t headphone)
{
    pthread_mutex_lock(&d->lock);
    d->state.speaker_volume = speaker;
    d->state.headphone_volume = headphone;
    d->state.valid_flag0 |= DS5_VF0_SPEAKER_VOLUME | DS5_VF0_HEADPHONE_VOLUME;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_mic_volume(ds5_device *d, uint8_t volume)
{
    pthread_mutex_lock(&d->lock);
    d->state.mic_volume = volume;
    d->state.valid_flag0 |= DS5_VF0_MIC_VOLUME;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_audio_output(ds5_device *d, ds5_audio_out out)
{
    /* Output path select, audio_control bits 4-5. */
    static const uint8_t path[] = {
        [DS5_AUDIO_OUT_HEADPHONE] = 0,
        [DS5_AUDIO_OUT_SPEAKER]   = 3,
        [DS5_AUDIO_OUT_BOTH]      = 2,
    };

    pthread_mutex_lock(&d->lock);
    d->state.audio_control = (d->state.audio_control & ~0x30) | path[out] << 4;
    d->state.valid_flag0 |= DS5_VF0_AUDIO_CONTROL;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_set_mute(ds5_device *d, int mic, int speaker, int headphone)
{
    pthread_mutex_lock(&d->lock);
    d->state.power_save_control =
        (d->state.power_save_control &
         ~(MUTE_MIC | MUTE_SPEAKER | MUTE_HEADPHONE)) |
        (mic ? MUTE_MIC : 0) | (speaker ? MUTE_SPEAKER : 0) |
        (headphone ? MUTE_HEADPHONE : 0);
    d->state.valid_flag1 |= DS5_VF1_POWER_SAVE;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

/* audio */

/* Sent ahead of every stream: without it the actuators keep whatever power
 * reduction, mute and filter setting the last host left behind, which can
 * leave haptics imperceptible. valid_flag0 is deliberately untouched; its
 * rumble bits would take the actuators out of audio mode. The mic-mute bit
 * shares the byte, so carry over the controller's current mute state.
 * Caller holds d->lock. */
static void haptics_config(ds5_device *d)
{
    struct ds5_output_common *st = &d->state;

    st->power_save_control = d->input.mic_muted ? MUTE_MIC : 0;
    st->reduce_motor_power = 0;
    st->haptics_flags = 1;                    /* low-pass filter on */
    st->valid_flag1 |= DS5_VF1_POWER_SAVE | DS5_VF1_MOTOR_POWER_LEVEL |
                       DS5_VF1_HAPTIC_LOW_PASS;
}

static void queue_frame(void *ctx, const struct ds5_audio_frame *f)
{
    ds5_device *d = ctx;

    pthread_mutex_lock(&d->lock);
    if (!d->due && !d->q_count)
        haptics_config(d);
    if (d->q_count == QUEUE_FRAMES) {         /* full: drop the oldest */
        d->q_head = (d->q_head + 1) % QUEUE_FRAMES;
        d->q_count--;
        d->stats.dropped++;
    }
    d->q[(d->q_head + d->q_count++) % QUEUE_FRAMES] = *f;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

void ds5_push_audio(ds5_device *d, const int16_t *quad, int frames)
{
    if (!d->audio_mode)
        return;

    /* An idle sink still delivers silence every cycle. Sending it would keep
     * the radio and the controller's amplifier busy for nothing, so stop
     * half a second after the last non-zero sample. */
    int silent = 1;
    for (int i = 0; i < frames * 4 && silent; i++)
        silent = !quad[i];
    if (!silent) {
        d->silence = 0;
    } else if (d->silence >= SILENCE_HOLD) {
        return;
    } else if ((d->silence += frames) >= SILENCE_HOLD) {
        ds5_audio_reset(d->audio);
        return;
    }
    ds5_audio_push(d->audio, quad, frames, queue_frame, d);
}

void ds5_set_mic_enabled(ds5_device *d, int on)
{
    pthread_mutex_lock(&d->lock);
    d->mic_on = !!on;
    d->mic_rd = d->mic_wr;
    d->mic_started = 0;
    pthread_mutex_unlock(&d->lock);
    wake(d);
}

int ds5_pull_mic(ds5_device *d, int16_t *buf, int frames)
{
    pthread_mutex_lock(&d->lock);
    int have = (int)(d->mic_wr - d->mic_rd);
    /* Start (and restart after an underrun) with one spare frame buffered,
     * so the 10 ms bursts off the radio don't underrun every other cycle. */
    int ok = have >= frames + (d->mic_started ? 0 : MIC_FRAME);
    d->mic_started = ok;
    for (int i = 0; ok && i < frames; i++)
        buf[i] = d->mic_ring[d->mic_rd++ % MIC_RING];
    pthread_mutex_unlock(&d->lock);
    return ok ? frames : 0;
}

/* queries */

int ds5_get_input(ds5_device *d, ds5_input_state *out)
{
    pthread_mutex_lock(&d->lock);
    int have = d->have_input;
    *out = d->input;
    pthread_mutex_unlock(&d->lock);
    return have;
}

void ds5_get_stats(ds5_device *d, ds5_stats *out)
{
    pthread_mutex_lock(&d->lock);
    *out = d->stats;
    pthread_mutex_unlock(&d->lock);
}
