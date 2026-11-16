#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate the vendored hid-playstation.c from a kernel source tree: copy
# the base file, apply ds4l.patch, vendor hid-ids.h next to it. The
# checked-in hid-playstation.c / hid-ids.h are the same files, committed so
# users can build (kernel-headers packages ship neither).
#
# Usage: ./prepare.sh <kernel-tree | path/to/hid-playstation.c>
#    or: KERNEL_SRC=<kernel-tree> ./prepare.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

SRC="${1:-${KERNEL_SRC:-}}"
if [ -z "$SRC" ]; then
    echo "usage: $0 <kernel-tree | hid-playstation.c>  (or set KERNEL_SRC)" >&2
    exit 1
fi
[ -d "$SRC" ] && SRC="$SRC/drivers/hid/hid-playstation.c"
[ -f "$SRC" ] || { echo "ERROR: $SRC not found" >&2; exit 1; }

echo "base source   : $SRC"
echo "running kernel: $(uname -r)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/drivers/hid"
cp "$SRC" "$TMP/drivers/hid/hid-playstation.c"
# hid-ids.h is private to drivers/hid/ and included with quotes, so it is
# vendored next to our source.
IDS="$(dirname "$SRC")/hid-ids.h"
[ -f "$IDS" ] || { echo "ERROR: $IDS not found (needed for the build)" >&2; exit 1; }

( cd "$TMP" && patch -p1 --no-backup-if-mismatch < "$HERE/ds4l.patch" )
cp "$TMP/drivers/hid/hid-playstation.c" "$HERE/hid-playstation.c"
cp "$IDS" "$HERE/hid-ids.h"
echo "regenerated: $HERE/hid-playstation.c + $HERE/hid-ids.h"
