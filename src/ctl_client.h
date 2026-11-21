/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L ctl_client.h: client side of the control socket.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_CTL_CLIENT_H
#define DS4L_CTL_CLIENT_H

/* Default socket path: $XDG_RUNTIME_DIR/ds4l.sock, else
 * /tmp/ds4l-<uid>/ds4l.sock in a directory only the user can open. */
const char *ds4l_ctl_default_path(void);

/* Connect, send one command line, print every reply line to stdout.
 * Returns 0 if the last line was "ok ...". */
int  ds4l_ctl_client(const char *path, int argc, char **argv);

/* Shortest id prefix the CLI accepts. Shorter tokens are taken as command
 * arguments: "0" or "255" could otherwise prefix a pad's id. */
#define DS4L_ID_PREFIX_MIN 4

/* Does `token` name a connected device (via a "list" round-trip): an exact
 * id, or a prefix of at least DS4L_ID_PREFIX_MIN characters, any case?
 * 1 one device, 2 several (ambiguous prefix), 0 none, -1 daemon
 * unreachable. */
int  ds4l_ctl_resolve_device(const char *path, const char *token);

#endif /* DS4L_CTL_CLIENT_H */
