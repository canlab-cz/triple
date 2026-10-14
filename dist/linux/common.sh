# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Shared steps of the Linux and Debian distribution builds (sourced).
#
# Environment:
#   SAVVYCAN_GIT  SavvyCAN repository to clone (default GitHub; a local clone works)
#   SAVVYCAN_REV  upstream master commit the changes are tested on (default e439670)
#   WORK          build directory, preferably on a Linux file system
#                 (default ~/.cache/usb2can-triple-dist)
#   JOBS          parallel build jobs (default: number of CPUs)

set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=$(cat "$REPO/dist/VERSION")
OUT="$REPO/dist/out"
WORK=${WORK:-$HOME/.cache/usb2can-triple-dist}
SAVVYCAN_GIT=${SAVVYCAN_GIT:-https://github.com/collin80/SavvyCAN.git}
SAVVYCAN_REV=${SAVVYCAN_REV:-e439670}
JOBS=${JOBS:-$(nproc)}
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)

mkdir -p "$OUT" "$WORK"

log() { printf '\n=== %s\n' "$*"; }

# SavvyCAN (Qt 5, qmake) with the Triple connection and tools applied.
# Result: $WORK/savvycan/build/SavvyCAN
build_savvycan() {
    local src="$WORK/savvycan"
    if [ ! -d "$src/.git" ]; then
        log "cloning SavvyCAN from $SAVVYCAN_GIT"
        git clone -q "$SAVVYCAN_GIT" "$src"
    fi
    log "SavvyCAN at $SAVVYCAN_REV with gui/savvycan applied"
    git -C "$src" checkout -q -f "$SAVVYCAN_REV"
    git -C "$src" clean -q -fdx -e build
    python3 "$REPO/gui/savvycan/apply.py" "$src" >/dev/null
    mkdir -p "$src/build"
    (cd "$src/build" && qmake ../SavvyCAN.pro CONFIG+=release >/dev/null && make -s -j"$JOBS" >/dev/null)
    test -x "$src/build/SavvyCAN"
    strip "$src/build/SavvyCAN"
}

# tools/canre/canre_cli. Result: $WORK/canre_cli
build_canre() {
    log "canre_cli"
    g++ -std=c++17 -O2 -Wall -Wextra -I "$REPO/gui/savvycan/re" -o "$WORK/canre_cli" "$REPO/tools/canre/canre_cli.cpp"
    strip "$WORK/canre_cli"
}

# Driver sources as DKMS / make expects them, into $1
copy_driver_source() {
    mkdir -p "$1/include"
    cp "$REPO"/driver/Makefile "$REPO"/driver/dkms.conf "$REPO"/driver/*.c "$1/"
    cp "$REPO"/driver/include/*.h "$1/include/"
}

# Python tools into $1
copy_python_tools() {
    mkdir -p "$1"
    cp "$REPO"/tools/triple_proto.py "$REPO"/tools/triple_dump.py "$REPO"/tools/canopen_sim.py \
       "$REPO"/tools/canopen_sim.eds "$REPO"/tools/re_demo.py "$1/"
}

# User documentation into $1
copy_docs() {
    mkdir -p "$1/img"
    cp "$REPO/README.md" "$1/"
    cp "$REPO"/doc/linux-driver.md "$REPO"/doc/savvycan-guide.md "$REPO"/doc/reverse-engineering.md \
       "$REPO"/doc/protocol.md "$1/"
    cp "$REPO"/doc/img/*.png "$1/img/"
    # the README links into doc/; inside the package everything sits in one folder
    sed -i 's|(doc/img/|(img/|g; s|(doc/\([a-z-]*\.md\)|(\1|g' "$1/README.md"
}
