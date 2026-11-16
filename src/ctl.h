/* SPDX-License-Identifier: LGPL-3.0-or-later
 * DS4L ctl.h: line-oriented Unix-socket control protocol.
 * Copyright (c) 2026 Macauley Lim
 */
#ifndef DS4L_CTL_H
#define DS4L_CTL_H

#include "ctl_client.h"
#include "manager.h"

/* Listen on `path`, replacing a stale socket. Returns fd or -1. */
int  ds4l_ctl_listen(const char *path);

/* Accept one pending connection and serve it until the client closes or
 * goes quiet for 5 s. */
void ds4l_ctl_serve(ds4l_mgr *m, int listen_fd);

#endif /* DS4L_CTL_H */
