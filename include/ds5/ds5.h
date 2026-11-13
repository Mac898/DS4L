/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — per-controller device core.
 * Copyright (c) 2026 Macauley Lim
 *
 * Every function is thread-safe unless noted. Output is written by the
 * device's own I/O thread; setters only queue the change and wake it.
 */
#ifndef DS5_H
#define DS5_H

#include <stddef.h>
#include <stdint.h>
#include "ds5/ds5_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds5_device ds5_device;

typedef enum { DS5_BUS_BT = 0, DS5_BUS_USB = 1 } ds5_bus;
typedef enum { DS5_SIDE_LEFT = 0, DS5_SIDE_RIGHT = 1 } ds5_side;

/* Take ownership of an open, nonblocking hidraw fd and start the device's
 * I/O thread. Returns NULL on failure (the fd is closed either way). */
ds5_device *ds5_open(int fd, ds5_bus bus);
void        ds5_close(ds5_device *d);
/* 0 once the hidraw node has gone away (unplugged / link dropped). */
int         ds5_alive(const ds5_device *d);

/* Audio mode (speaker / haptics / mic over Bluetooth), default off.
 *
 * Audio reports make the controller interleave mic data into its 0x31 input
 * reports, which the stock hid-playstation driver misparses as gamepad state.
 * Only enable this while the DS4L kernel module (driver/) is loaded; the
 * manager enforces that. Triggers, LEDs, rumble and volume work regardless. */
void        ds5_set_audio_mode(ds5_device *d, int enable);

/* Extra state reports per haptic frame (0..3, default 1). The controller
 * stops its actuators about 17 ms after the last state report, so how many
 * are needed depends on how evenly the Bluetooth adapter delivers: 0 is
 * enough on a jitter-free link, 1 rides out the usual 11 ms scan stalls. */
void        ds5_set_haptic_keepalive(ds5_device *d, int n);

/* State setters. Each sends one 0x31/0x02 report carrying only the
 * fields that changed, so nothing the kernel driver set gets overwritten. */
typedef struct ds5_trigger_effect {
    uint8_t mode;        /* DS5_TRIGGER_*  */
    uint8_t param[10];
} ds5_trigger_effect;

void ds5_set_trigger(ds5_device *d, ds5_side side, const ds5_trigger_effect *fx);
void ds5_set_rumble(ds5_device *d, uint8_t left, uint8_t right);
void ds5_set_lightbar(ds5_device *d, uint8_t r, uint8_t g, uint8_t b);
void ds5_set_player_leds(ds5_device *d, uint8_t mask);   /* low 5 bits */
void ds5_set_mic_mute_led(ds5_device *d, int on);
/* Attenuate the rumble/trigger motors (0 = full power .. 7 = most reduced). */
void ds5_set_motor_power(ds5_device *d, uint8_t rumble_reduce,
                         uint8_t trigger_reduce);
void ds5_set_volume(ds5_device *d, uint8_t speaker, uint8_t headphone); /* 0..255, 0..127 */
void ds5_set_mic_volume(ds5_device *d, uint8_t volume);                 /* 0..64 */
typedef enum {
    DS5_AUDIO_OUT_HEADPHONE = 0, /* L+R -> headphone, speaker muted */
    DS5_AUDIO_OUT_SPEAKER,       /* internal speaker                */
    DS5_AUDIO_OUT_BOTH,          /* both sinks                      */
} ds5_audio_out;
void ds5_set_audio_output(ds5_device *d, ds5_audio_out out);
void ds5_set_mute(ds5_device *d, int mic_mute, int speaker_mute, int headphone_mute);

/* Audio (Bluetooth, audio mode on) */
/* Interleaved S16 FL FR RL RR at 48 kHz: FL/FR -> speaker (or the 3.5 mm
 * jack when a headset is plugged in), RL/RR -> haptics. Digital silence is
 * not transmitted. Call from one thread only. */
void ds5_push_audio(ds5_device *d, const int16_t *quad, int frames);

void ds5_set_mic_enabled(ds5_device *d, int on);
/* Fill buf with exactly `frames` mono S16 mic samples, or return 0 if that
 * much has not been buffered yet. */
int  ds5_pull_mic(ds5_device *d, int16_t *buf, int frames);

/* Input state, parsed from the controller's input reports */
/* Bits in ds5_input_state.buttons. */
enum {
    DS5_BTN_SQUARE   = 1u << 0,  DS5_BTN_CROSS    = 1u << 1,
    DS5_BTN_CIRCLE   = 1u << 2,  DS5_BTN_TRIANGLE = 1u << 3,
    DS5_BTN_L1       = 1u << 4,  DS5_BTN_R1       = 1u << 5,
    DS5_BTN_L2       = 1u << 6,  DS5_BTN_R2       = 1u << 7,
    DS5_BTN_CREATE   = 1u << 8,  DS5_BTN_OPTIONS  = 1u << 9,
    DS5_BTN_L3       = 1u << 10, DS5_BTN_R3       = 1u << 11,
    DS5_BTN_PS       = 1u << 12, DS5_BTN_TOUCHPAD = 1u << 13,
    DS5_BTN_MUTE     = 1u << 14,
};
typedef struct ds5_input_state {
    uint8_t  lx, ly, rx, ry;       /* sticks, 0..255 (128 = center)       */
    uint8_t  l2, r2;               /* analog triggers, 0..255             */
    uint8_t  dpad;                 /* 0..7 = N..NW, 8 = released          */
    uint32_t buttons;              /* DS5_BTN_* bitfield                  */
    int16_t  gyro[3];              /* raw x, y, z                         */
    int16_t  accel[3];             /* raw x, y, z                         */
    uint8_t  battery_percent;      /* 0..100                              */
    uint8_t  charging;             /* 0 discharging, 1 charging, 2 full   */
    uint8_t  headphones;           /* headphone jack present              */
    uint8_t  mic_plugged;          /* external mic present                */
    uint8_t  mic_muted;            /* mic muted at the hardware level      */
    /* adaptive-trigger telemetry (effect + status nibbles) */
    uint8_t  left_trigger_effect,  left_trigger_status;
    uint8_t  right_trigger_effect, right_trigger_status;
} ds5_input_state;
/* Copy the most recently parsed input state into *out. Returns 1 if at least
 * one input report has been seen since open, else 0. */
int ds5_get_input(ds5_device *d, ds5_input_state *out);

typedef struct ds5_stats {
    unsigned frames_sent;    /* audio frames written                */
    unsigned underruns;      /* stream ran dry mid-playback         */
    unsigned dropped;        /* frames dropped, queue full          */
    unsigned write_fail;
    unsigned state_late;     /* state reached the pad >20 ms apart while
                              * haptics played: each one is a felt dropout.
                              * If this climbs, raise the haptic keepalive. */
    unsigned state_gap_max;  /* worst such gap, ms                  */
    unsigned send_late;      /* audio reports written >2 ms after due */
    unsigned send_late_max;  /* worst lateness, us                  */
    unsigned in_reports;
} ds5_stats;
void ds5_get_stats(ds5_device *d, ds5_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* DS5_H */
