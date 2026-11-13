/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — ctl.h : line-oriented Unix-socket control protocol.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_CTL_H
#define DS4L_CTL_H

#include "manager.h"

/* Default socket path: $XDG_RUNTIME_DIR/ds4l.sock (or /tmp for root). */
const char *ds4l_ctl_default_path(void);

/* Create + listen. Returns fd or -1. Unlinks a stale socket file first. */
int  ds4l_ctl_listen(const char *path);

/* Accept ONE pending connection (non-blocking accept) and serve it: read
 * command lines, write "ok ..."/"err ..."/"dev ..." reply lines until the
 * client closes or times out (~5 s). Returns when the connection is done. */
void ds4l_ctl_serve(ds4l_mgr *m, int listen_fd);

/* Client side (used by the ds4l CLI): connect, send one command line, print
 * every reply line to stdout. Returns 0 if the last line was "ok ...". */
int  ds4l_ctl_client(const char *path, int argc, char **argv);

/* Does `prefix` uniquely name a connected device (via a "list" round-trip)?
 * 1 yes, 0 no/ambiguous, -1 daemon unreachable. */
int  ds4l_ctl_resolve_device(const char *path, const char *prefix);

#endif /* DS4L_CTL_H */
