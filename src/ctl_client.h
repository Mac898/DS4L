/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L ctl_client.h: client side of the control socket.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_CTL_CLIENT_H
#define DS4L_CTL_CLIENT_H

/* Default socket path: $XDG_RUNTIME_DIR/ds4l.sock (or /tmp). */
const char *ds4l_ctl_default_path(void);

/* Connect, send one command line, print every reply line to stdout.
 * Returns 0 if the last line was "ok ...". */
int  ds4l_ctl_client(const char *path, int argc, char **argv);

/* Does `prefix` uniquely name a connected device (via a "list" round-trip)?
 * 1 yes, 0 no/ambiguous, -1 daemon unreachable. */
int  ds4l_ctl_resolve_device(const char *path, const char *prefix);

#endif /* DS4L_CTL_CLIENT_H */
