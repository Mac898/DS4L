/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — audio codec: 48 kHz quad PCM -> controller audio frames, and
 * mic Opus -> PCM.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS5_AUDIO_H
#define DS5_AUDIO_H

#include <stdint.h>
#include "ds5/ds5_protocol.h"

typedef struct ds5_audio ds5_audio;
typedef void (*ds5_frame_fn)(void *ctx, const struct ds5_audio_frame *f);

ds5_audio *ds5_audio_create(void);
void       ds5_audio_destroy(ds5_audio *a);

/* Feed interleaved S16 FL FR RL RR at 48 kHz (the wired DualSense layout):
 * FL/FR become the Opus speaker frame, RL/RR the haptics. `cb` is called once
 * per completed frame, i.e. every 512 input samples. */
void ds5_audio_push(ds5_audio *a, const int16_t *quad, int frames,
                    ds5_frame_fn cb, void *ctx);
/* Forget the partially built frame (call after a gap in the stream). */
void ds5_audio_reset(ds5_audio *a);

/* Decode one mic Opus frame to mono S16 at 48 kHz. Returns samples or <0. */
int ds5_audio_decode_mic(ds5_audio *a, const uint8_t *opus, int len,
                         int16_t *out, int max_frames);

#endif
