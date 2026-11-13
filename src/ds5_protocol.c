/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — ds5_protocol.c : wire-protocol implementation.
 * Copyright (c) 2026 Macauley Lim
 */
#include "ds5/ds5_protocol.h"
#include <string.h>

/* CRC-32 (reflected, poly 0xEDB88320), running value, no init/final */
uint32_t ds5_crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

uint32_t ds5_report_crc(uint8_t seed, const uint8_t *data, size_t len)
{
    uint32_t crc = ds5_crc32_le(0xFFFFFFFFu, &seed, 1);
    crc = ds5_crc32_le(crc, data, len);
    return ~crc;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ds5_finalize_bt_report(uint8_t *report, size_t len)
{
    uint32_t crc = ds5_report_crc(DS5_CRC_SEED_OUTPUT, report, len - 4);
    put_le32(report + len - 4, crc);
}

int ds5_check_crc(uint8_t seed, const uint8_t *report, size_t len)
{
    if (len < 4)
        return 0;
    uint32_t want = get_le32(report + len - 4);
    return ds5_report_crc(seed, report, len - 4) == want;
}

/* helpers */
static uint8_t clamp_u8(int v, int lo, int hi)
{
    if (v < lo) return (uint8_t)lo;
    if (v > hi) return (uint8_t)hi;
    return (uint8_t)v;
}

/*
 * Pack the multi-zone strength encoding shared by the feedback/vibration
 * effect family: a 10-bit active-zone bitmap (2 bytes) plus a packed sequence
 * of 3-bit strength values (4 bytes), then a frequency byte.
 *   out[0]=mode  out[1..2]=active  out[3..6]=strengths  out[9]=frequency
 */
static void pack_zones(uint8_t out[DS5_TRIGGER_BLOCK_BYTES], uint8_t mode,
                       const uint8_t strength[10], uint8_t frequency)
{
    uint16_t active = 0;
    uint32_t packed = 0;
    for (int i = 0; i < 10; i++) {
        if (strength[i] > 0) {
            uint8_t v = (uint8_t)((strength[i] - 1) & 0x7u);
            packed |= (uint32_t)v << (3 * i);
            active |= (uint16_t)(1u << i);
        }
    }
    memset(out, 0, DS5_TRIGGER_BLOCK_BYTES);
    out[0] = mode;
    out[1] = (uint8_t)(active & 0xff);
    out[2] = (uint8_t)((active >> 8) & 0xff);
    out[3] = (uint8_t)(packed & 0xff);
    out[4] = (uint8_t)((packed >> 8) & 0xff);
    out[5] = (uint8_t)((packed >> 16) & 0xff);
    out[6] = (uint8_t)((packed >> 24) & 0xff);
    out[9] = frequency;
}

void ds5_trig_off(uint8_t out[DS5_TRIGGER_BLOCK_BYTES])
{
    memset(out, 0, DS5_TRIGGER_BLOCK_BYTES);
    out[0] = DS5_TRIGGER_OFF;
}

void ds5_trig_feedback(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                       uint8_t position, uint8_t strength)
{
    uint8_t s[10] = {0};
    position = clamp_u8(position, 0, 9);
    strength = clamp_u8(strength, 1, 8);
    for (int i = position; i < 10; i++)
        s[i] = strength;
    pack_zones(out, DS5_TRIGGER_FEEDBACK, s, 0);
}

void ds5_trig_weapon(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                     uint8_t start, uint8_t stop, uint8_t strength)
{
    start    = clamp_u8(start, 2, 7);
    stop     = clamp_u8(stop, (int)start + 1, 8);
    strength = clamp_u8(strength, 1, 8);
    uint16_t zones = (uint16_t)((1u << start) | (1u << stop));
    memset(out, 0, DS5_TRIGGER_BLOCK_BYTES);
    out[0] = DS5_TRIGGER_WEAPON;
    out[1] = (uint8_t)(zones & 0xff);
    out[2] = (uint8_t)((zones >> 8) & 0xff);
    out[3] = (uint8_t)(strength - 1);
}

void ds5_trig_vibration(uint8_t out[DS5_TRIGGER_BLOCK_BYTES],
                        uint8_t position, uint8_t amplitude, uint8_t frequency)
{
    /* Best-effort: fill the amplitude from `position` to the end and carry a
     * frequency, using the shared zone packing. The raw escape covers other
     * authoring schemes. */
    uint8_t s[10] = {0};
    position  = clamp_u8(position, 0, 9);
    amplitude = clamp_u8(amplitude, 1, 8);
    for (int i = position; i < 10; i++)
        s[i] = amplitude;
    pack_zones(out, DS5_TRIGGER_VIBRATION, s, frequency);
}

void ds5_trig_raw(uint8_t out[DS5_TRIGGER_BLOCK_BYTES], uint8_t mode,
                  const uint8_t *params, size_t nparams)
{
    memset(out, 0, DS5_TRIGGER_BLOCK_BYTES);
    out[0] = mode;
    if (nparams > 10)
        nparams = 10;
    if (params && nparams)
        memcpy(out + 1, params, nparams);
}

/* report assembly */
size_t ds5_build_bt_state(uint8_t *buf, const struct ds5_output_common *common,
                          uint8_t seq)
{
    memset(buf, 0, DS5_OUT_REPORT_BT_SIZE);
    buf[0] = DS5_OUT_REPORT_BT;
    buf[1] = (uint8_t)((seq & 0x0fu) << 4);
    buf[2] = DS5_OUTPUT_TAG;
    memcpy(buf + 3, common, sizeof(*common)); /* bytes 3..49 */
    ds5_finalize_bt_report(buf, DS5_OUT_REPORT_BT_SIZE);
    return DS5_OUT_REPORT_BT_SIZE;
}

size_t ds5_build_usb_state(uint8_t *buf, const struct ds5_output_common *common)
{
    /* USB output report 0x02: report id + the 47-byte common body, zero-padded
     * to 63 bytes. No seq/tag and no CRC — that framing is Bluetooth-only
     * (wire format per hid-playstation's dualsense_output_report_usb). */
    memset(buf, 0, DS5_OUT_REPORT_USB_SIZE);
    buf[0] = DS5_OUT_REPORT_USB;
    memcpy(buf + 1, common, sizeof(*common));
    return DS5_OUT_REPORT_USB_SIZE;
}

/* Audio-control segment shared by the 0x36/0x39/0x32 reports. `flags` has one
 * bit per following byte (plus bit 0 = mic enable, bit 6 = always set); the
 * bytes are buffer-length hints and, last, the frame counter. */
static size_t put_audio_ctl(uint8_t *buf, uint8_t seq, uint8_t flags,
                            int ndepth, uint8_t counter)
{
    size_t o = 1;
    buf[o++] = (uint8_t)((seq & 0x0fu) << 4);
    buf[o++] = DS5_SEG_FLAG | DS5_SEG_AUDIO_CTL;
    buf[o++] = (uint8_t)(ndepth + 2);
    buf[o++] = flags;
    for (int i = 0; i < ndepth; i++)
        buf[o++] = DS5_AUDIO_BUFFER_LEN;
    buf[o++] = counter;
    return o;
}

size_t ds5_build_bt_audio(uint8_t *buf, uint8_t seq,
                          const struct ds5_output_common *state,
                          const struct ds5_audio_frame *f, int nframes,
                          uint8_t counter, int mic, int headset)
{
    int dbl = nframes == 2;
    size_t len = dbl ? DS5_OUT_REPORT_BT_AUDIO_DBL_SIZE
                     : DS5_OUT_REPORT_BT_AUDIO_SIZE;
    uint8_t two = dbl ? 0x40u : 0;   /* segment carries two frames */

    memset(buf, 0, len);
    buf[0] = dbl ? DS5_OUT_REPORT_BT_AUDIO_DBL : DS5_OUT_REPORT_BT_AUDIO;
    size_t o = put_audio_ctl(buf, seq,
                             (uint8_t)((dbl ? 0x7eu : 0xfeu) | (mic ? 1 : 0)),
                             dbl ? 4 : 5, counter);

    if (!dbl) {
        buf[o++] = DS5_SEG_FLAG | DS5_SEG_STATE;
        buf[o++] = DS5_STATE_SEG_BYTES;
        memcpy(buf + o, state, sizeof(*state));
        o += DS5_STATE_SEG_BYTES;
    }

    buf[o++] = DS5_SEG_FLAG | two | DS5_SEG_HAPTICS;
    buf[o++] = DS5_HAPTIC_SEG_BYTES;
    for (int i = 0; i < nframes; i++, o += DS5_HAPTIC_SEG_BYTES)
        memcpy(buf + o, f[i].haptics, DS5_HAPTIC_SEG_BYTES);

    buf[o++] = (uint8_t)(DS5_SEG_FLAG | two |
                         (headset ? DS5_SEG_HEADSET : DS5_SEG_SPEAKER));
    buf[o++] = DS5_SPEAKER_SEG_BYTES;
    for (int i = 0; i < nframes; i++, o += DS5_SPEAKER_SEG_BYTES)
        memcpy(buf + o, f[i].opus, DS5_SPEAKER_SEG_BYTES);

    ds5_finalize_bt_report(buf, len);
    return len;
}

size_t ds5_build_bt_haptics(uint8_t *buf, uint8_t seq,
                            const struct ds5_audio_frame *f,
                            uint8_t counter, int mic)
{
    memset(buf, 0, DS5_OUT_REPORT_BT_CTL_SIZE);
    buf[0] = DS5_OUT_REPORT_BT_CTL;
    size_t o = put_audio_ctl(buf, seq, (uint8_t)(0x66u | (mic ? 1 : 0)), 2,
                             counter);
    buf[o++] = DS5_SEG_FLAG | DS5_SEG_HAPTICS;
    buf[o++] = DS5_HAPTIC_SEG_BYTES;
    memcpy(buf + o, f->haptics, DS5_HAPTIC_SEG_BYTES);
    ds5_finalize_bt_report(buf, DS5_OUT_REPORT_BT_CTL_SIZE);
    return DS5_OUT_REPORT_BT_CTL_SIZE;
}

size_t ds5_build_bt_mic(uint8_t *buf, uint8_t seq, int mic)
{
    memset(buf, 0, DS5_OUT_REPORT_BT_CTL_SIZE);
    buf[0] = DS5_OUT_REPORT_BT_CTL;
    buf[1] = (uint8_t)((seq & 0x0fu) << 4);
    buf[2] = DS5_SEG_FLAG | DS5_SEG_AUDIO_CTL;
    buf[3] = 1;
    buf[4] = (uint8_t)(0x02u | (mic ? 1 : 0));
    ds5_finalize_bt_report(buf, DS5_OUT_REPORT_BT_CTL_SIZE);
    return DS5_OUT_REPORT_BT_CTL_SIZE;
}
