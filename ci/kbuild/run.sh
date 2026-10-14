#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Compile the driver and the daemon against several kernel header sets.
# Usage: sh ci/kbuild/run.sh            (all targets)
#        sh ci/kbuild/run.sh u2404      (one target: u2204 | u2404 | fedora)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TARGETS=${*:-"u2204 u2404 fedora"}
status=0
for t in $TARGETS; do
  echo "#### $t"
  docker build -q -t "triple-kbuild:$t" -f "$HERE/Dockerfile.$t" "$HERE" || { status=1; continue; }
  docker run --rm -v "$REPO:/src:ro" "triple-kbuild:$t" sh /src/ci/kbuild/build.sh || status=1
done
exit $status
