/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — pw_nodes.c : per-controller PipeWire virtual audio devices.
 * Copyright (c) 2026 Macauley Lim
 *
 * The nodes mirror the wired DualSense USB sound card, so Bluetooth and USB
 * controllers look the same to games (Proton matches the Windows endpoint by
 * name and format). Taken from /proc/asound/cardX/stream0 of a wired pad:
 *
 *   Playback: S16_LE, 4 ch @ 48000, map FL FR RL RR (FL/FR speaker or jack,
 *             RL/RR haptics)
 *   Capture:  S16_LE, 2 ch @ 48000, FL FR
 *   Description: "DualSense wireless controller (PS5)"
 *
 * Over Bluetooth the device core turns those channels into the controller's
 * audio frames.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pw_nodes.h"

#ifdef DS4L_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <stdio.h>
#include <string.h>

#define DS4L_PW_RATE 48000
#define DS4L_MAX_FRAMES 8192   /* largest capture cycle we fill */

/* Playback: the wired USB endpoint spec (quad S16). */
#define DS4L_OUT_CHANNELS 4
#define DS4L_OUT_FMT      SPA_AUDIO_FORMAT_S16_LE

/* Capture: the wired USB endpoint spec (stereo S16). */
#define DS4L_IN_CHANNELS 2
#define DS4L_IN_FMT      SPA_AUDIO_FORMAT_S16_LE

/* The wired card's description, reused so apps can't tell them apart by name. */
#define DS4L_DEVICE_DESC "DualSense wireless controller (PS5)"

/* shared loop (all devices) */

static struct {
    int                    inited;
    int                    ok;
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
} g_pw;

int ds4l_pw_available(void) { return 1; }

int ds4l_pw_init(void)
{
    if (g_pw.inited)
        return g_pw.ok ? 0 : -1;
    g_pw.inited = 1;

    pw_init(NULL, NULL);

    g_pw.loop = pw_thread_loop_new("ds4l-audio", NULL);
    if (!g_pw.loop)
        return -1;

    struct pw_loop *l = pw_thread_loop_get_loop(g_pw.loop);
    g_pw.context = pw_context_new(l, NULL, 0);
    if (!g_pw.context)
        goto fail;
    g_pw.core = pw_context_connect(g_pw.context, NULL, 0);
    if (!g_pw.core)
        goto fail;
    if (pw_thread_loop_start(g_pw.loop) < 0)
        goto fail;

    g_pw.ok = 1;
    return 0;

fail:
    if (g_pw.core) { pw_core_disconnect(g_pw.core); g_pw.core = NULL; }
    if (g_pw.context) { pw_context_destroy(g_pw.context); g_pw.context = NULL; }
    if (g_pw.loop) { pw_thread_loop_destroy(g_pw.loop); g_pw.loop = NULL; }
    return -1;
}

void ds4l_pw_shutdown(void)
{
    if (!g_pw.inited || !g_pw.loop)
        return;
    pw_thread_loop_stop(g_pw.loop);
    if (g_pw.core) { pw_core_disconnect(g_pw.core); g_pw.core = NULL; }
    if (g_pw.context) { pw_context_destroy(g_pw.context); g_pw.context = NULL; }
    pw_thread_loop_destroy(g_pw.loop);
    g_pw.loop = NULL;
    g_pw.ok = 0;
    g_pw.inited = 0;
}

/* per-device node pair */

struct ds4l_pw_nodes {
    ds5_device       *dev;
    struct pw_stream *out;    /* Audio/Sink:   4ch S16 */
    struct pw_stream *in;     /* Audio/Source: 2ch S16 */
    struct spa_hook   out_l, in_l;
};

static void out_on_process(void *userdata)
{
    ds4l_pw_nodes *n = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(n->out);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        uint32_t off = SPA_MIN(d->chunk->offset, d->maxsize);
        uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - off);
        ds5_push_audio(n->dev, SPA_PTROFF(d->data, off, int16_t),
                       (int)(size / (DS4L_OUT_CHANNELS * sizeof(int16_t))));
    }
    pw_stream_queue_buffer(n->out, b);
}

/* The controller mic is mono; the wired capture endpoint is stereo. */
static void in_on_process(void *userdata)
{
    ds4l_pw_nodes *n = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(n->in);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        const uint32_t stride = DS4L_IN_CHANNELS * sizeof(int16_t);
        int frames = (int)(d->maxsize / stride);
        if (b->requested && (int)b->requested < frames)
            frames = (int)b->requested;
        if (frames > DS4L_MAX_FRAMES)
            frames = DS4L_MAX_FRAMES;

        int16_t mono[DS4L_MAX_FRAMES], *dst = d->data;
        if (!ds5_pull_mic(n->dev, mono, frames))
            memset(mono, 0, (size_t)frames * sizeof(int16_t));
        for (int f = 0; f < frames; f++)
            dst[f * 2] = dst[f * 2 + 1] = mono[f];

        d->chunk->offset = 0;
        d->chunk->stride = (int32_t)stride;
        d->chunk->size = (uint32_t)frames * stride;
    }
    pw_stream_queue_buffer(n->in, b);
}

/* Ask the controller for mic data only while an app is capturing. */
static void in_on_state(void *userdata, enum pw_stream_state old,
                        enum pw_stream_state state, const char *error)
{
    (void)old; (void)error;
    ds4l_pw_nodes *n = userdata;
    ds5_set_mic_enabled(n->dev, state == PW_STREAM_STATE_STREAMING);
}

static const struct pw_stream_events out_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = out_on_process,
};
static const struct pw_stream_events in_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = in_on_state,
    .process = in_on_process,
};

/* Format pods matching the USB endpoints. */
static const struct spa_pod *build_out_pod(uint8_t *buffer, size_t size)
{
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buffer, (uint32_t)size);
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = DS4L_OUT_FMT,
        .rate = DS4L_PW_RATE,
        .channels = DS4L_OUT_CHANNELS);
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_RL;
    info.position[3] = SPA_AUDIO_CHANNEL_RR;
    return spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info);
}

static const struct spa_pod *build_in_pod(uint8_t *buffer, size_t size)
{
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buffer, (uint32_t)size);
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = DS4L_IN_FMT,
        .rate = DS4L_PW_RATE,
        .channels = DS4L_IN_CHANNELS);
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    return spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info);
}

int ds4l_pw_start(ds4l_pw_nodes **out, ds5_device *dev, const char *id)
{
    if (!g_pw.ok && ds4l_pw_init() != 0)
        return -1;

    ds4l_pw_nodes *n = calloc(1, sizeof(*n));
    if (!n)
        return -1;
    n->dev = dev;

    struct pw_loop *l = pw_thread_loop_get_loop(g_pw.loop);

    /* The shared loop is already running (other devices may be streaming), so
     * all stream create/connect/destroy here must hold the loop lock. */
    pw_thread_loop_lock(g_pw.loop);

    char name[128];

    /* 4-channel playback sink. node.description matches the wired card's so
     * games cannot tell them apart; node.name stays unique per controller.
     * node.latency asks for a quantum close to the controller's 10.67 ms
     * frame; larger quanta work too, the device core paces the frames. */
    snprintf(name, sizeof(name), "ds4l_output_%s", id);
    n->out = pw_stream_new_simple(
        l, name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,        "Audio",
            PW_KEY_MEDIA_CATEGORY,    "Playback",
            PW_KEY_MEDIA_CLASS,       "Audio/Sink",
            PW_KEY_NODE_NAME,         name,
            PW_KEY_NODE_DESCRIPTION,  DS4L_DEVICE_DESC,
            PW_KEY_NODE_NICK,         DS4L_DEVICE_DESC,
            PW_KEY_DEVICE_DESCRIPTION, DS4L_DEVICE_DESC,
            PW_KEY_NODE_LATENCY,      "480/48000",
            NULL),
        &out_events, n);
    if (!n->out)
        goto fail;

    /* 2-channel capture source (created without Opus too, for spec parity —
     * it just stays silent if the mic can't decode). */
    snprintf(name, sizeof(name), "ds4l_input_%s", id);
    n->in = pw_stream_new_simple(
        l, name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,        "Audio",
            PW_KEY_MEDIA_CATEGORY,    "Capture",
            PW_KEY_MEDIA_CLASS,       "Audio/Source",
            PW_KEY_NODE_NAME,         name,
            PW_KEY_NODE_DESCRIPTION,  DS4L_DEVICE_DESC,
            PW_KEY_NODE_NICK,         DS4L_DEVICE_DESC,
            PW_KEY_DEVICE_DESCRIPTION, DS4L_DEVICE_DESC,
            NULL),
        &in_events, n);
    if (!n->in)
        goto fail;

    const enum pw_stream_flags flags =
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS;

    {
        uint8_t pod[1024];
        const struct spa_pod *params[1] = { build_out_pod(pod, sizeof pod) };
        if (!params[0] ||
            pw_stream_connect(n->out, PW_DIRECTION_INPUT, PW_ID_ANY,
                              flags, params, 1) < 0)
            goto fail;
    }
    {
        uint8_t pod[1024];
        const struct spa_pod *params[1] = { build_in_pod(pod, sizeof pod) };
        if (!params[0] ||
            pw_stream_connect(n->in, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              flags, params, 1) < 0)
            goto fail;
    }

    pw_thread_loop_unlock(g_pw.loop);

    *out = n;
    return 0;

fail:
    if (n->out) pw_stream_destroy(n->out);
    if (n->in)  pw_stream_destroy(n->in);
    pw_thread_loop_unlock(g_pw.loop);
    free(n);
    return -1;
}

void ds4l_pw_stop(ds4l_pw_nodes *n)
{
    if (!n)
        return;
    if (g_pw.loop)
        pw_thread_loop_lock(g_pw.loop);
    if (n->out) { pw_stream_destroy(n->out); n->out = NULL; }
    if (n->in)  { pw_stream_destroy(n->in);  n->in  = NULL; }
    if (g_pw.loop)
        pw_thread_loop_unlock(g_pw.loop);

    free(n);
}

#else /* !DS4L_HAVE_PIPEWIRE: stubs */

int  ds4l_pw_available(void) { return 0; }
int  ds4l_pw_init(void) { return -1; }
void ds4l_pw_shutdown(void) {}

int ds4l_pw_start(ds4l_pw_nodes **out, ds5_device *dev, const char *id)
{
    (void)out; (void)dev; (void)id;
    return -1;
}

void ds4l_pw_stop(ds4l_pw_nodes *n) { (void)n; }

#endif /* DS4L_HAVE_PIPEWIRE */
