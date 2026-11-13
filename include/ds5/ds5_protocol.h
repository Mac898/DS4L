/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * DS4L — DualSense wire protocol: report layouts, segment framing, CRC and
 * adaptive-trigger effect encoders.
 * Copyright (c) 2026 Macauley Lim
 *
 * Written from the publicly documented output-report protocol and the
 * mainline hid-playstation driver. DualSense is a trademark of Sony
 * Interactive Entertainment; this project is unaffiliated.
 */
#ifndef DS5_PROTOCOL_H
#define DS5_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB identity */
#define DS5_VENDOR_ID                 0x054Cu
#define DS5_PRODUCT_DUALSENSE         0x0CE6u
#define DS5_PRODUCT_DUALSENSE_EDGE    0x0DF2u

/* BT HID transaction CRC seeds (the seed folds in the header byte) */
#define DS5_CRC_SEED_INPUT            0xA1u  /* device->host DATA input   */
#define DS5_CRC_SEED_OUTPUT           0xA2u  /* host->device DATA output  */
#define DS5_CRC_SEED_FEATURE          0xA3u  /* DATA feature              */

/* Report ids and sizes */
#define DS5_OUT_REPORT_USB            0x02u
#define DS5_OUT_REPORT_USB_SIZE       63
#define DS5_OUT_REPORT_BT             0x31u  /* compact, state-only        */
#define DS5_OUT_REPORT_BT_SIZE        78
#define DS5_OUT_REPORT_BT_AUDIO       0x36u  /* state + haptics + speaker  */
#define DS5_OUT_REPORT_BT_AUDIO_SIZE  398
#define DS5_OUT_REPORT_BT_AUDIO_DBL   0x39u  /* 2x speaker, no state       */
#define DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE 547
#define DS5_OUT_REPORT_BT_CTL         0x32u  /* audio control only         */
#define DS5_OUT_REPORT_BT_CTL_SIZE    142
#define DS5_IN_REPORT_BT              0x31u

/* Magic tag byte that must follow the seq byte in BT output reports. */
#define DS5_OUTPUT_TAG                0x10u

/* Segment markers in audio reports (marker = DS5_SEG_FLAG | type) */
#define DS5_SEG_FLAG                  0x80u
#define DS5_SEG_STATE                 0x10u  /* DS5EffectsState body        */
#define DS5_SEG_AUDIO_CTL             0x11u  /* mic-enable + buffer + count */
#define DS5_SEG_HAPTICS               0x12u  /* 3 kHz int8 stereo PCM       */
#define DS5_SEG_SPEAKER               0x13u  /* Opus, internal speaker      */
#define DS5_SEG_HEADSET               0x16u  /* Opus, 3.5 mm headset        */

/* The state segment is 63 bytes: the 47-byte common body, zero padded. */
#define DS5_STATE_SEG_BYTES           63
/* Haptic segment is 64 bytes = 32 stereo int8 frames. */
#define DS5_HAPTIC_SEG_BYTES          64
/* Speaker Opus frame size used on the wire. */
#define DS5_SPEAKER_SEG_BYTES         200
/* Native haptic sample rate the controller's BT audio path expects. */
#define DS5_HAPTIC_RATE_HZ            3000
/* Controller-side audio buffer length hint. */
#define DS5_AUDIO_BUFFER_LEN          48

/* valid_flag0: which fields of the common body to apply */
#define DS5_VF0_COMPATIBLE_VIBRATION  (1u << 0)
#define DS5_VF0_HAPTICS_SELECT        (1u << 1)
#define DS5_VF0_RIGHT_TRIGGER_FFB     (1u << 2)
#define DS5_VF0_LEFT_TRIGGER_FFB      (1u << 3)
#define DS5_VF0_HEADPHONE_VOLUME      (1u << 4)
#define DS5_VF0_SPEAKER_VOLUME        (1u << 5)
#define DS5_VF0_MIC_VOLUME            (1u << 6)
#define DS5_VF0_AUDIO_CONTROL         (1u << 7)
/* valid_flag1 */
#define DS5_VF1_MIC_MUTE_LED          (1u << 0)
#define DS5_VF1_POWER_SAVE            (1u << 1)
#define DS5_VF1_LIGHTBAR_CONTROL      (1u << 2)
#define DS5_VF1_RELEASE_LEDS          (1u << 3)
#define DS5_VF1_PLAYER_INDICATOR      (1u << 4)
#define DS5_VF1_HAPTIC_LOW_PASS       (1u << 5)
#define DS5_VF1_MOTOR_POWER_LEVEL     (1u << 6)
#define DS5_VF1_AUDIO_CONTROL2        (1u << 7)
/* valid_flag2 */
#define DS5_VF2_LED_BRIGHTNESS        (1u << 0)
#define DS5_VF2_LIGHTBAR_SETUP        (1u << 1)
#define DS5_VF2_COMPATIBLE_VIBRATION2 (1u << 2)

/* Adaptive-trigger effect modes (raw register values) */
#define DS5_TRIGGER_OFF               0x05u
#define DS5_TRIGGER_FEEDBACK          0x21u  /* resist beyond a position    */
#define DS5_TRIGGER_BOW               0x22u
#define DS5_TRIGGER_GALLOPING         0x23u
#define DS5_TRIGGER_WEAPON            0x25u  /* resist start..stop, snap     */
#define DS5_TRIGGER_VIBRATION         0x26u
#define DS5_TRIGGER_MACHINE           0x27u

/* 11-byte trigger parameter block: mode + 10 params. */
#define DS5_TRIGGER_BLOCK_BYTES       11

/*
 * The 47-byte "common" output body, shared by the USB (0x02), BT (0x31) and
 * BT-audio (0x36) reports. Byte offsets are part of the wire format.
 */
#pragma pack(push, 1)
struct ds5_output_common {
    uint8_t  valid_flag0;            /* 0  */
    uint8_t  valid_flag1;            /* 1  */
    uint8_t  motor_right;            /* 2  classic rumble (light)   */
    uint8_t  motor_left;             /* 3  classic rumble (heavy)   */
    uint8_t  headphone_volume;       /* 4  0x00-0x7f */
    uint8_t  speaker_volume;         /* 5  0x00-0xff */
    uint8_t  mic_volume;             /* 6  0x00-0x40 */
    uint8_t  audio_control;          /* 7  */
    uint8_t  mute_button_led;        /* 8  */
    uint8_t  power_save_control;     /* 9  */
    uint8_t  right_trigger[DS5_TRIGGER_BLOCK_BYTES]; /* 10..20 mode+param[10] */
    uint8_t  left_trigger[DS5_TRIGGER_BLOCK_BYTES];  /* 21..31 mode+param[10] */
    uint32_t host_timestamp;         /* 32..35 (little-endian) */
    uint8_t  reduce_motor_power;     /* 36  rumble:0-3 | trigger:4-7 */
    uint8_t  audio_control2;         /* 37  speaker pre-gain in low 3 bits */
    uint8_t  valid_flag2;            /* 38  */
    uint8_t  haptics_flags;          /* 39  */
    uint8_t  reserved0;              /* 40  */
    uint8_t  lightbar_setup;         /* 41  */
    uint8_t  led_brightness;         /* 42  */
    uint8_t  player_leds;            /* 43  */
    uint8_t  lightbar_red;           /* 44  */
    uint8_t  lightbar_green;         /* 45  */
    uint8_t  lightbar_blue;          /* 46  */
};
#pragma pack(pop)
_Static_assert(sizeof(struct ds5_output_common) == 47,
               "ds5_output_common must be exactly 47 bytes (wire format)");

/* CRC-32 (reflected, poly 0xEDB88320) */
/* Running CRC with no init/final inversion (matches Linux crc32_le semantics
 * used by the firmware and dualsensectl). */
uint32_t ds5_crc32_le(uint32_t crc, const uint8_t *data, size_t len);
/* Full DualSense report CRC: seed byte folded in, standard final inversion.
 * ds5_report_crc(seed, data, len) == standard CRC-32 of (seed ++ data). */
uint32_t ds5_report_crc(uint8_t seed, const uint8_t *data, size_t len);

/* Write the little-endian output CRC into the last 4 bytes of a BT report
 * buffer of total length `len`. */
void ds5_finalize_bt_report(uint8_t *report, size_t len);
/* Verify the trailing CRC of a received/echoed report (seed selectable). */
int  ds5_check_crc(uint8_t seed, const uint8_t *report, size_t len);

/* Trigger-effect encoders (fill an 11-byte mode+params block) */
/* Off / neutral. */
void ds5_trig_off(uint8_t out[DS5_TRIGGER_BLOCK_BYTES]);
/* Constant resistance from `position` (0-9) to the end, `strength` 1-8. */
void ds5_trig_feedback(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                       uint8_t position, uint8_t strength);
/* Resistance over [start..stop] that releases past stop (gun trigger). */
void ds5_trig_weapon(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                     uint8_t start, uint8_t stop, uint8_t strength);
/* Vibrate around `position` at `amplitude` (1-8) and `frequency` Hz. */
void ds5_trig_vibration(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                        uint8_t position, uint8_t amplitude, uint8_t frequency);
/* Raw escape: caller supplies mode + up to 10 params. */
void ds5_trig_raw(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                  uint8_t mode, const uint8_t *params, size_t nparams);

/* Report assembly */
/* Build a compact state-only BT 0x31 report into `buf` (must be >= 78 bytes).
 * `seq` is the low 4 bits of the sequence counter. Returns bytes written. */
size_t ds5_build_bt_state(uint8_t *buf, const struct ds5_output_common *common,
                          uint8_t seq);
/* Build a state-only USB 0x02 report into `buf` (must be >= 63 bytes): no
 * seq/tag, no CRC. Returns bytes written (63). */
size_t ds5_build_usb_state(uint8_t *buf, const struct ds5_output_common *common);

/* One controller audio frame: 32 stereo haptic samples (3 kHz int8) plus one
 * Opus speaker frame. The controller consumes 93.75 of these per second
 * (10.67 ms each), so the 480-sample Opus frame spans 512 samples of 48 kHz
 * source audio — the speaker path runs at 45 kHz. */
struct ds5_audio_frame {
    int8_t  haptics[DS5_HAPTIC_SEG_BYTES];
    uint8_t opus[DS5_SPEAKER_SEG_BYTES];
    uint8_t has_haptics, has_speaker;   /* not on the wire: any signal? */
};

/* Build a BT audio report from `nframes` (1 or 2) frames. `counter` is the
 * running frame counter.
 *   1 frame:  0x36 (398 bytes), with `state` embedded. The controller only
 *             drives its actuators from a frame that arrives with state, so
 *             this is the form haptics need (an all-zero state will do).
 *   2 frames: 0x39 (547 bytes), no state; half the report rate, speaker only. */
size_t ds5_build_bt_audio(uint8_t *buf, uint8_t seq,
                          const struct ds5_output_common *state,
                          const struct ds5_audio_frame *f, int nframes,
                          uint8_t counter, int mic, int headset);

/* Build a 0x32 report (142 bytes) carrying one frame's haptics and no
 * speaker audio. Much smaller than 0x36, so it gets through a busy link more
 * evenly; state has to follow in separate 0x31 reports. */
size_t ds5_build_bt_haptics(uint8_t *buf, uint8_t seq,
                            const struct ds5_audio_frame *f,
                            uint8_t counter, int mic);

/* Build the 0x32 report (142 bytes) that turns mic streaming on or off. */
size_t ds5_build_bt_mic(uint8_t *buf, uint8_t seq, int mic);

#ifdef __cplusplus
}
#endif
#endif /* DS5_PROTOCOL_H */
