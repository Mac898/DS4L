#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Macauley Lim
#
# uninstall.sh: undo install.sh for the current user. Stops and disables the
# service and removes the installed files. The DKMS module needs root, so
# the commands to remove it are printed, not run.
#
# Usage: ./uninstall.sh [prefix]   (default: ~/.local, as for install.sh)
set -euo pipefail

cd "$(dirname "$0")"
PREFIX="${1:-$HOME/.local}"

systemctl --user disable --now ds4l 2>/dev/null || true

rm -f "$PREFIX/bin/ds4ld" "$PREFIX/bin/ds4l"
rm -f "$PREFIX/share/systemd/user/ds4l.service"
rm -f "$HOME/.config/systemd/user/ds4l.service"
if [ -d "$PREFIX/share/ds4l" ]; then
    rm -r "$PREFIX/share/ds4l"
fi
systemctl --user daemon-reload || true
echo "removed DS4L from $PREFIX"

NAME="$(sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' driver/dkms.conf)"
VER="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)"
cat <<MSG

To remove the kernel module too (needs root), run:
  sudo dkms remove ${NAME}/${VER} --all
  sudo rm -rf /usr/src/${NAME}-${VER}
  sudo depmod -a
  sudo modprobe -r hid_playstation && sudo modprobe hid_playstation
The last line puts the stock module back; disconnect the controllers first,
or reboot.
MSG
