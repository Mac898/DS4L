/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — pw_route.c : send each pad's tagged playback stream to its sink.
 * Copyright (c) 2026 Macauley Lim
 *
 * Every pad's sink has the same description, the wired card's, and SDL
 * shows apps nothing else. So an app that drives several pads cannot pick
 * the right device itself. Instead it names its stream after the pad (SDL:
 * SDL_HINT_AUDIO_DEVICE_STREAM_NAME, which becomes media.name), and we move
 * the stream to that pad's sink: its ds4l_output_<id> over Bluetooth, the
 * 4-channel sink of its USB sound card when wired.
 *
 * The move is a target.object entry in the "default" metadata, as
 * pavucontrol and pw-metadata write it; the session manager does the
 * relinking. The value is the sink's node.name, not its serial: WirePlumber
 * remembers a moved stream's target per media.role (SDL: "Game"), and
 * replays it on later streams with no target of their own. It only
 * remembers targets given by serial, so a name leaves no pad sink behind.
 * Each stream is moved once per target sink, so a later manual move sticks
 * until the pad's sink changes (reconnect).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pw_route.h"

#include <ctype.h>
#include <string.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Six hex pairs at p, separated by one ':' or '-' each, or by nothing.
 * Returns the length matched, or 0. */
static size_t parse_addr(const char *p, uint8_t out[6])
{
    const char *q = p;
    char sep = 0;
    for (int i = 0; i < 6; i++) {
        if (i > 0) {
            if (i == 1 && (*q == ':' || *q == '-'))
                sep = *q;
            if (sep) {
                if (*q != sep)
                    return 0;
                q++;
            }
        }
        int hi = hexval((unsigned char)q[0]);
        int lo = hi < 0 ? -1 : hexval((unsigned char)q[1]);
        if (lo < 0)
            return 0;
        out[i] = (uint8_t)(hi << 4 | lo);
        q += 2;
    }
    if (hexval((unsigned char)*q) >= 0 || (sep && *q == sep))
        return 0;
    return (size_t)(q - p);
}

int ds4l_route_name_has_addr(const char *s, const uint8_t addr[6])
{
    if (!s)
        return 0;
    for (const char *p = s; *p; p++) {
        /* Not inside a longer run of hex pairs. */
        if (p > s && (hexval((unsigned char)p[-1]) >= 0 ||
                      ((p[-1] == ':' || p[-1] == '-') && p - 1 > s &&
                       hexval((unsigned char)p[-2]) >= 0)))
            continue;
        uint8_t a[6];
        if (parse_addr(p, a) && memcmp(a, addr, 6) == 0)
            return 1;
    }
    return 0;
}

#ifdef DS4L_HAVE_PIPEWIRE

#include "hidraw.h"
#include "pw_nodes.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_PADS 16

enum { KIND_STREAM = 1, KIND_SINK = 2 };

struct rnode {
    struct spa_list  link;
    uint32_t         id;
    int              kind;
    struct pw_proxy *proxy;
    struct spa_hook  node_l;
    int              have_info;
    uint64_t         serial;      /* object.serial                      */
    char             name[128];   /* node.name                          */
    char             media[256];  /* media.name (streams)               */
    char             target[128]; /* target.object property (streams)   */
    int              card;        /* alsa.card (sinks), -1 if none      */
    int              channels;    /* audio.channels (sinks)             */
    uint64_t         applied;     /* sink serial we last moved it to    */
};

/* target.object entries of the "default" metadata, by subject id. */
struct rmeta {
    struct spa_list link;
    uint32_t        subject;
    char            value[128];
};

struct rpad {
    char    id[36];
    uint8_t addr[6];
    char    sink[128];   /* node.name of its own sink (Bluetooth) */
    char    usb[256];    /* USB device in sysfs (wired)           */
};

static struct {
    int                 started;
    int                 ready;       /* initial state read */
    int                 sync_seq;
    int                 syncs_left;
    struct pw_core     *core;
    struct pw_registry *registry;
    struct spa_hook     registry_l, core_l;
    struct pw_proxy    *meta;
    uint32_t            meta_id;
    struct spa_hook     meta_l;
    struct spa_list     nodes, metas;
    struct rpad         pads[MAX_PADS];
    int                 npads;
    ds4l_route_log_fn   log;
    void               *log_ud;
} R;

static void rlog(int lvl, const char *fmt, ...)
{
    if (!R.log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    R.log(lvl, buf, R.log_ud);
}

static void copy_prop(char *dst, size_t n, const char *v)
{
    snprintf(dst, n, "%s", v ? v : "");
}

static const char *meta_get(uint32_t subject)
{
    struct rmeta *e;
    spa_list_for_each(e, &R.metas, link)
        if (e->subject == subject)
            return e->value;
    return NULL;
}

static void meta_drop(uint32_t subject)
{
    struct rmeta *e, *t;
    spa_list_for_each_safe(e, t, &R.metas, link)
        if (subject == PW_ID_ANY || e->subject == subject) {
            spa_list_remove(&e->link);
            free(e);
        }
}

/* routing */

static struct rnode *find_sink(struct rpad *p)
{
    struct rnode *n, *best = NULL;
    int card = -1;
    if (!p->sink[0] && (card = ds4l_usb_sound_card(p->usb)) < 0)
        return NULL;

    spa_list_for_each(n, &R.nodes, link) {
        if (n->kind != KIND_SINK || !n->have_info)
            continue;
        if (p->sink[0]) {
            if (strcmp(n->name, p->sink) == 0)
                return n;
        } else if (n->card == card) {
            /* The pad's playback is 4 channels; with the pro-audio profile
             * the card can have more than one sink. */
            if (n->channels == 4)
                return n;
            if (!best)
                best = n;
        }
    }
    return best;
}

static void route_stream(struct rnode *s)
{
    struct rpad *p = NULL;
    for (int i = 0; i < R.npads && !p; i++)
        if (ds4l_route_name_has_addr(s->media, R.pads[i].addr))
            p = &R.pads[i];
    if (!p)
        return;

    struct rnode *sink = find_sink(p);
    if (!sink || s->applied == sink->serial)
        return;

    char serial[24];
    snprintf(serial, sizeof(serial), "%" PRIu64, sink->serial);

    /* Already headed there: by metadata (an earlier run, or the user), or
     * by the stream's own target when there is no metadata entry. */
    const char *cur = meta_get(s->id);
    if (!cur)
        cur = s->target;
    if (strcmp(cur, serial) == 0 || strcmp(cur, sink->name) == 0) {
        s->applied = sink->serial;
        return;
    }
    if (!R.meta)
        return;

    pw_metadata_set_property((struct pw_metadata *)R.meta, s->id,
                             "target.object", NULL, sink->name);
    s->applied = sink->serial;
    rlog('i', "%s: stream %u \"%s\" -> %s", p->id, s->id, s->media,
         sink->name);
}

static void route_all(void)
{
    if (!R.ready)
        return;
    struct rnode *n;
    spa_list_for_each(n, &R.nodes, link)
        if (n->kind == KIND_STREAM && n->have_info)
            route_stream(n);
}

/* nodes */

static void node_info(void *data, const struct pw_node_info *info)
{
    struct rnode *n = data;
    if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props)
        return;

    const struct spa_dict *d = info->props;
    const char *v;
    copy_prop(n->name, sizeof(n->name), spa_dict_lookup(d, PW_KEY_NODE_NAME));
    copy_prop(n->media, sizeof(n->media), spa_dict_lookup(d, PW_KEY_MEDIA_NAME));
    copy_prop(n->target, sizeof(n->target),
              spa_dict_lookup(d, PW_KEY_TARGET_OBJECT));
    n->serial = (v = spa_dict_lookup(d, PW_KEY_OBJECT_SERIAL))
                ? strtoull(v, NULL, 10) : 0;
    if (!(v = spa_dict_lookup(d, "alsa.card")))
        v = spa_dict_lookup(d, "api.alsa.card");
    n->card = v ? atoi(v) : -1;
    n->channels = (v = spa_dict_lookup(d, PW_KEY_AUDIO_CHANNELS)) ? atoi(v) : 0;
    n->have_info = 1;

    if (n->kind == KIND_STREAM) {
        if (R.ready)
            route_stream(n);
    } else {
        route_all();
    }
}

static const struct pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info    = node_info,
};

static void node_free(struct rnode *n)
{
    spa_list_remove(&n->link);
    spa_hook_remove(&n->node_l);
    pw_proxy_destroy(n->proxy);
    free(n);
}

/* metadata */

static int meta_property(void *data, uint32_t subject, const char *key,
                         const char *type, const char *value)
{
    (void)data; (void)type;
    if (key && strcmp(key, "target.object") != 0)
        return 0;
    meta_drop(subject);   /* a NULL key clears all of the subject's keys */
    if (key && value) {
        struct rmeta *e = calloc(1, sizeof(*e));
        if (!e)
            return 0;
        e->subject = subject;
        copy_prop(e->value, sizeof(e->value), value);
        spa_list_append(&R.metas, &e->link);
    }
    return 0;
}

static const struct pw_metadata_events meta_events = {
    .version  = PW_VERSION_METADATA_EVENTS,
    .property = meta_property,
};

static void meta_free(void)
{
    if (!R.meta)
        return;
    spa_hook_remove(&R.meta_l);
    pw_proxy_destroy(R.meta);
    R.meta = NULL;
    meta_drop(PW_ID_ANY);
}

/* registry */

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props)
{
    (void)data; (void)permissions; (void)version;
    if (!props)
        return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        int kind = !cls ? 0
                 : strcmp(cls, "Stream/Output/Audio") == 0 ? KIND_STREAM
                 : strcmp(cls, "Audio/Sink") == 0 ? KIND_SINK : 0;
        if (!kind)
            return;
        struct rnode *n = calloc(1, sizeof(*n));
        if (!n)
            return;
        n->proxy = pw_registry_bind(R.registry, id, PW_TYPE_INTERFACE_Node,
                                    PW_VERSION_NODE, 0);
        if (!n->proxy) {
            free(n);
            return;
        }
        n->id = id;
        n->kind = kind;
        n->card = -1;
        pw_node_add_listener((struct pw_node *)n->proxy, &n->node_l,
                             &node_events, n);
        spa_list_append(&R.nodes, &n->link);
    } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !R.meta) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || strcmp(name, "default") != 0)
            return;
        R.meta = pw_registry_bind(R.registry, id, PW_TYPE_INTERFACE_Metadata,
                                  PW_VERSION_METADATA, 0);
        if (!R.meta)
            return;
        R.meta_id = id;
        pw_metadata_add_listener((struct pw_metadata *)R.meta, &R.meta_l,
                                 &meta_events, NULL);
        /* Session manager restarted: read the new entries, then route. */
        if (R.ready) {
            R.ready = 0;
            R.syncs_left = 1;
            R.sync_seq = pw_core_sync(R.core, PW_ID_CORE, 0);
        }
    }
}

static void registry_global_remove(void *data, uint32_t id)
{
    (void)data;
    struct rnode *n, *t;
    spa_list_for_each_safe(n, t, &R.nodes, link)
        if (n->id == id) {
            node_free(n);
            break;
        }
    meta_drop(id);
    if (R.meta && id == R.meta_id)
        meta_free();
}

static const struct pw_registry_events registry_events = {
    .version       = PW_VERSION_REGISTRY_EVENTS,
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* Two round trips: the first lists the globals, the second brings the node
 * info and metadata of the proxies bound meanwhile. Route only after that,
 * so existing metadata is known before anything is moved. */
static void core_done(void *data, uint32_t id, int seq)
{
    (void)data;
    if (id != PW_ID_CORE || seq != R.sync_seq || R.ready)
        return;
    if (--R.syncs_left > 0) {
        R.sync_seq = pw_core_sync(R.core, PW_ID_CORE, 0);
        return;
    }
    R.ready = 1;
    route_all();
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .done    = core_done,
};

/* Caller holds the loop lock. */
static int route_start(void)
{
    if (R.started)
        return 0;
    R.core = ds4l_pw_core();
    if (!R.core)
        return -1;
    spa_list_init(&R.nodes);
    spa_list_init(&R.metas);
    R.registry = pw_core_get_registry(R.core, PW_VERSION_REGISTRY, 0);
    if (!R.registry)
        return -1;
    pw_registry_add_listener(R.registry, &R.registry_l, &registry_events, NULL);
    pw_core_add_listener(R.core, &R.core_l, &core_events, NULL);
    R.syncs_left = 2;
    R.sync_seq = pw_core_sync(R.core, PW_ID_CORE, 0);
    R.started = 1;
    return 0;
}

int ds4l_route_add(const char *id, const uint8_t addr[6], const char *sink,
                   const char *usb_path, ds4l_route_log_fn log, void *ud)
{
    if (ds4l_pw_init() != 0)
        return -1;
    struct pw_thread_loop *loop = ds4l_pw_loop();
    pw_thread_loop_lock(loop);

    R.log = log;
    R.log_ud = ud;
    if (route_start() != 0) {
        pw_thread_loop_unlock(loop);
        return -1;
    }

    struct rpad *p = NULL;
    for (int i = 0; i < R.npads; i++)
        if (strcmp(R.pads[i].id, id) == 0)
            p = &R.pads[i];
    if (!p && R.npads < MAX_PADS)
        p = &R.pads[R.npads++];
    if (p) {
        memset(p, 0, sizeof(*p));
        copy_prop(p->id, sizeof(p->id), id);
        memcpy(p->addr, addr, 6);
        copy_prop(p->sink, sizeof(p->sink), sink);
        copy_prop(p->usb, sizeof(p->usb), usb_path);
        route_all();
    }

    pw_thread_loop_unlock(loop);
    return p ? 0 : -1;
}

void ds4l_route_remove(const char *id)
{
    struct pw_thread_loop *loop = ds4l_pw_loop();
    if (!loop)
        return;
    pw_thread_loop_lock(loop);
    for (int i = 0; i < R.npads; i++)
        if (strcmp(R.pads[i].id, id) == 0) {
            memmove(&R.pads[i], &R.pads[i + 1],
                    (size_t)(R.npads - i - 1) * sizeof(R.pads[0]));
            R.npads--;
            break;
        }
    pw_thread_loop_unlock(loop);
}

void ds4l_route_shutdown(void)
{
    struct pw_thread_loop *loop = ds4l_pw_loop();
    if (!R.started || !loop)
        return;
    pw_thread_loop_lock(loop);
    struct rnode *n, *t;
    spa_list_for_each_safe(n, t, &R.nodes, link)
        node_free(n);
    meta_free();
    spa_hook_remove(&R.core_l);
    spa_hook_remove(&R.registry_l);
    pw_proxy_destroy((struct pw_proxy *)R.registry);
    R.registry = NULL;
    R.started = R.ready = 0;
    R.npads = 0;
    pw_thread_loop_unlock(loop);
}

#else /* !DS4L_HAVE_PIPEWIRE */

int ds4l_route_add(const char *id, const uint8_t addr[6], const char *sink,
                   const char *usb_path, ds4l_route_log_fn log, void *ud)
{
    (void)id; (void)addr; (void)sink; (void)usb_path; (void)log; (void)ud;
    return -1;
}

void ds4l_route_remove(const char *id) { (void)id; }
void ds4l_route_shutdown(void) {}

#endif /* DS4L_HAVE_PIPEWIRE */
