#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Macauley Lim
#
# install.sh: build DS4L, install it for the current user, enable the
# systemd user service. No root needed: the DKMS module is built by the
# daemon on first run, which asks for authorization then.
#
# Usage: ./install.sh [prefix]     (default: ~/.local)
# Undo with ./uninstall.sh [prefix].
set -euo pipefail

cd "$(dirname "$0")"
PREFIX="$(realpath -m "${1:-$HOME/.local}")"

if [ -f build/meson-private/coredata.dat ]; then
    meson configure build --prefix "$PREFIX"
else
    meson setup build --prefix "$PREFIX"
fi
meson compile -C build
meson install -C build

# Older versions copied the unit here; it would shadow the installed one.
OLD_UNIT="$HOME/.config/systemd/user/ds4l.service"
if [ -f "$OLD_UNIT" ] && [ ! -L "$OLD_UNIT" ]; then
    echo "removing old unit copy $OLD_UNIT"
    rm -f "$OLD_UNIT"
fi

# meson put the unit in $PREFIX/share/systemd/user, which systemd searches
# for ~/.local, /usr/local and /usr. Other prefixes get linked in.
UNIT="$PREFIX/share/systemd/user/ds4l.service"
systemctl --user daemon-reload
if systemctl --user cat ds4l.service >/dev/null 2>&1; then
    systemctl --user enable --now ds4l
else
    systemctl --user enable --now "$UNIT"
fi

echo "installed to $PREFIX; service enabled (systemctl --user status ds4l)"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "NOTE: $PREFIX/bin is not in your PATH; add it to use 'ds4l'." ;;
esac
