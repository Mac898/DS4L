#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate the vendored hid-playstation.c from an in-tree kernel source:
# copy the base file, apply ds4l.patch, vendor hid-ids.h next to it. The
# checked-in hid-playstation.c / hid-ids.h are the same artifacts, committed
# so users can build (kernel-headers packages ship neither file).
# Run when rebasing onto a new kernel. Pass a base source path as $1.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

SRC="${1:-}"
if [ -z "$SRC" ]; then
    for c in \
        "/lib/modules/$(uname -r)/source/drivers/hid/hid-playstation.c" \
        /usr/src/linux-source-*/drivers/hid/hid-playstation.c \
        "$HOME/Software/linux/drivers/hid/hid-playstation.c"; do
        for f in $c; do [ -f "$f" ] && { SRC="$f"; break 2; }; done
    done
fi
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "ERROR: base hid-playstation.c not found; pass it as arg 1"; exit 1; }

echo "base source   : $SRC"
echo "running kernel: $(uname -r)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/drivers/hid"
cp "$SRC" "$TMP/drivers/hid/hid-playstation.c"
# hid-ids.h is a drivers/hid/-private header (not in kernel-headers packages);
# hid-playstation.c includes it with quotes, so vendor it next to our source.
IDS="$(dirname "$SRC")/hid-ids.h"
[ -f "$IDS" ] || { echo "ERROR: $IDS not found (needed for the build)"; exit 1; }

( cd "$TMP" && patch -p1 --no-backup-if-mismatch < "$HERE/ds4l.patch" )
cp "$TMP/drivers/hid/hid-playstation.c" "$HERE/hid-playstation.c"
cp "$IDS" "$HERE/hid-ids.h"
echo "regenerated: $HERE/hid-playstation.c + $HERE/hid-ids.h (GPL, from the kernel source)"
