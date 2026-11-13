/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — pw_nodes.h : per-controller PipeWire virtual audio devices.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_PW_NODES_H
#define DS4L_PW_NODES_H

#include "ds5/ds5.h"

typedef struct ds4l_pw_nodes ds4l_pw_nodes;

int  ds4l_pw_init(void);
void ds4l_pw_shutdown(void);
int  ds4l_pw_available(void);   /* 1 if built with PipeWire */

/* Create the node pair for one Bluetooth controller, mirroring the wired
 * DualSense USB sound card so apps see the same device either way:
 *   - sink:   S16_LE, 4 ch @ 48 kHz, FL FR RL RR (FL/FR speaker, RL/RR haptics)
 *   - source: S16_LE, 2 ch @ 48 kHz (mono mic on both channels)
 * Both are described "DualSense wireless controller (PS5)"; node.name is
 * ds4l_output_<id> / ds4l_input_<id>. */
int  ds4l_pw_start(ds4l_pw_nodes **out, ds5_device *dev, const char *id);

/* Destroy the pair. MUST be called before ds5_close(dev). Idempotent. */
void ds4l_pw_stop(ds4l_pw_nodes *n);

#endif /* DS4L_PW_NODES_H */
