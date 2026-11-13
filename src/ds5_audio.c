/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — audio codec.
 * Copyright (c) 2026 Macauley Lim
 *
 * The controller plays one audio frame every 10.67 ms: 32 haptic samples at
 * 3 kHz and a 480-sample Opus frame, which therefore runs at 45 kHz. Both are
 * whole ratios of 48 kHz, so input is handled 16 samples at a time: each
 * block yields one haptic sample (the block average) and 15 speaker samples
 * (linear interpolation), and 32 blocks make a frame.
 */
#include "ds5/ds5_audio.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK        16
#define SPK_PER_BLK  15
#define SPK_FRAME    480

struct ds5_audio {
    OpusEncoder *enc;
    OpusDecoder *dec;

    int16_t blk[BLOCK][4];
    int     nblk;                    /* samples in blk */
    int16_t spk[SPK_FRAME * 2];
    int     nspk;                    /* stereo samples in spk */
    struct ds5_audio_frame frame;
    int     nhap;                    /* bytes in frame.haptics */
    int     spk_signal;              /* any non-zero speaker input this frame */
};

ds5_audio *ds5_audio_create(void)
{
    ds5_audio *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;

    /* CELT only, 200-byte CBR frames. */
#ifdef OPUS_APPLICATION_RESTRICTED_CELT
    int app = OPUS_APPLICATION_RESTRICTED_CELT;
#else
    int app = OPUS_APPLICATION_RESTRICTED_LOWDELAY;   /* libopus < 1.5 */
#endif
    int err;
    a->enc = opus_encoder_create(48000, 2, app, &err);
    a->dec = opus_decoder_create(48000, 1, &err);
    if (!a->enc || !a->dec) {
        ds5_audio_destroy(a);
        return NULL;
    }
    opus_encoder_ctl(a->enc, OPUS_SET_BITRATE(DS5_SPEAKER_SEG_BYTES * 8 * 100));
    opus_encoder_ctl(a->enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(a->enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(a->enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    return a;
}

void ds5_audio_destroy(ds5_audio *a)
{
    if (!a)
        return;
    if (a->enc)
        opus_encoder_destroy(a->enc);
    if (a->dec)
        opus_decoder_destroy(a->dec);
    free(a);
}

void ds5_audio_reset(ds5_audio *a)
{
    a->nblk = a->nspk = a->nhap = a->spk_signal = 0;
}

static int8_t to_s8(int v)
{
    v >>= 8;
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static void block_done(ds5_audio *a, ds5_frame_fn cb, void *ctx)
{
    int l = 0, r = 0;
    for (int i = 0; i < BLOCK; i++) {
        l += a->blk[i][2];
        r += a->blk[i][3];
        a->spk_signal |= a->blk[i][0] | a->blk[i][1];
    }
    a->frame.haptics[a->nhap++] = to_s8(l / BLOCK);
    a->frame.haptics[a->nhap++] = to_s8(r / BLOCK);

    for (int k = 0; k < SPK_PER_BLK; k++) {
        int pos = k * BLOCK;                  /* in 1/15ths of a sample */
        int i = pos / SPK_PER_BLK, frac = pos % SPK_PER_BLK;
        for (int c = 0; c < 2; c++) {
            int s0 = a->blk[i][c], s1 = a->blk[i + 1][c];
            a->spk[a->nspk * 2 + c] =
                (int16_t)(s0 + (s1 - s0) * frac / SPK_PER_BLK);
        }
        a->nspk++;
    }
    a->nblk = 0;

    if (a->nspk < SPK_FRAME)
        return;
    a->frame.has_speaker = a->spk_signal != 0;
    a->frame.has_haptics = 0;
    for (int i = 0; i < DS5_HAPTIC_SEG_BYTES; i++)
        a->frame.has_haptics |= a->frame.haptics[i] != 0;
    a->nspk = a->nhap = a->spk_signal = 0;
    memset(a->frame.opus, 0, sizeof a->frame.opus);
    if (opus_encode(a->enc, a->spk, SPK_FRAME, a->frame.opus,
                    sizeof a->frame.opus) > 0)
        cb(ctx, &a->frame);
}

void ds5_audio_push(ds5_audio *a, const int16_t *quad, int frames,
                    ds5_frame_fn cb, void *ctx)
{
    for (int f = 0; f < frames; f++) {
        memcpy(a->blk[a->nblk], quad + f * 4, sizeof a->blk[0]);
        if (++a->nblk == BLOCK)
            block_done(a, cb, ctx);
    }
}

int ds5_audio_decode_mic(ds5_audio *a, const uint8_t *opus, int len,
                         int16_t *out, int max_frames)
{
    return opus_decode(a->dec, opus, len, out, max_frames, 0);
}
