/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L — ctl.h : line-oriented Unix-socket control protocol.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_CTL_H
#define DS4L_CTL_H

#include "ctl_client.h"
#include "manager.h"

/* Create + listen. Returns fd or -1. Unlinks a stale socket file first. */
int  ds4l_ctl_listen(const char *path);

/* Accept ONE pending connection (non-blocking accept) and serve it: read
 * command lines, write "ok ..."/"err ..."/"dev ..." reply lines until the
 * client closes or times out (~5 s). Returns when the connection is done. */
void ds4l_ctl_serve(ds4l_mgr *m, int listen_fd);

#endif /* DS4L_CTL_H */
