/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — pw_route.h : send each pad's tagged playback stream to its sink.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_PW_ROUTE_H
#define DS4L_PW_ROUTE_H

#include <stdint.h>

/* Log callback, same levels as the manager's ('e', 'w', 'i', 'd'). Called
 * from the PipeWire thread. */
typedef void (*ds4l_route_log_fn)(int level, const char *msg, void *ud);

/* Register a pad. addr is its Bluetooth address in the usual order (as
 * bluetoothctl and SDL's gamepad serial print it). A Bluetooth pad gives the
 * node.name of its own sink; a wired pad gives NULL and the sysfs directory
 * of its USB device, whose sound card is then the target.
 *
 * A playback stream whose media.name contains the address (hex pairs,
 * separated by ':' or '-' or not at all, any case) is moved to that sink
 * through the "default" metadata, as pavucontrol does. Starts the PipeWire
 * watcher on first use. Call without holding the PipeWire loop lock. */
int  ds4l_route_add(const char *id, const uint8_t addr[6], const char *sink,
                    const char *usb_path, ds4l_route_log_fn log, void *ud);
void ds4l_route_remove(const char *id);

/* Drop the watcher. Called by ds4l_pw_shutdown(). */
void ds4l_route_shutdown(void);

/* 1 if s contains addr in one of the forms above. Exposed for tests. */
int  ds4l_route_name_has_addr(const char *s, const uint8_t addr[6]);

#endif /* DS4L_PW_ROUTE_H */
