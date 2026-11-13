#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Macauley Lim
#
# install.sh — build DS4L, install it for the current user, enable the
# systemd user service. No root needed: the DKMS module is built by the
# daemon on first run, which asks for authorization then.
#
# Usage: ./install.sh [prefix]     (default: ~/.local)
set -euo pipefail

cd "$(dirname "$0")"
PREFIX="${1:-$HOME/.local}"

meson setup build --prefix "$PREFIX" 2>/dev/null || meson setup --reconfigure build --prefix "$PREFIX" >/dev/null
meson compile -C build >/dev/null
meson install -C build >/dev/null

UNIT_SRC="$PREFIX/share/ds4l/systemd/user/ds4l.service"
mkdir -p "$HOME/.config/systemd/user"
install -Dm644 "$UNIT_SRC" "$HOME/.config/systemd/user/ds4l.service"
systemctl --user daemon-reload
systemctl --user enable --now ds4l

echo "installed to $PREFIX; service enabled (systemctl --user status ds4l)"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "NOTE: $PREFIX/bin is not in your PATH — add it to use 'ds4l'." ;;
esac
