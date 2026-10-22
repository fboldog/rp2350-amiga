#!/usr/bin/env bash
# Build a bootable RDB hard-disk image for the emulated IDE drive
# (omega/Gayle.c) from a Workbench ADF, as an installer would: one FFS
# partition DH0 holding the disk's files. Needs amitools (rdbtool, xdftool).
#
# usage: native/make_hdf.sh <out.hdf> <volume> <ffs|ffs+intl> <workbench.adf>
#                           [modules.adf]
#   modules.adf  Kickstart 3.1.4's Modules disk: its LIBS/*.library go to
#                Libs (3.1.4 loads icon.library and workbench.library there)
#
# The geometry matches what the drive reports: CYLINDERS (default 130,
# 63 MB) x 16 heads x 63 sectors.
set -euo pipefail

if [ $# -lt 4 ]; then
    sed -n '2,13p' "$0" >&2
    exit 2
fi
out=$1 volume=$2 fs=$3 workbench=$4 modules=${5:-}
cylinders=${CYLINDERS:-130}
case "$fs" in
    ffs) dostype=DOS1 ;;
    ffs+intl) dostype=DOS3 ;;
    *) echo "filesystem must be ffs or ffs+intl" >&2; exit 2 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

rm -f "$out"
rdbtool "$out" create chs=$cylinders,16,63 + init + \
    add size=100% name=DH0 dostype=$dostype bootable=1 >/dev/null
xdftool "$out" open part=DH0 + format "$volume" "$fs" >/dev/null

# unpack puts the files in <dir>/<volume name>/ when <dir> exists
mkdir "$tmp/wb" "$tmp/mod"
xdftool "$workbench" unpack "$tmp/wb" >/dev/null
for f in "$tmp"/wb/*/*; do
    xdftool "$out" open part=DH0 + write "$f" >/dev/null
done
if [ -n "$modules" ]; then
    xdftool "$modules" unpack "$tmp/mod" >/dev/null
    for f in "$tmp"/mod/*/LIBS/*.library; do
        xdftool "$out" open part=DH0 + write "$f" Libs >/dev/null
    done
fi
echo "built $out ($volume, $fs, $cylinders cylinders)"
