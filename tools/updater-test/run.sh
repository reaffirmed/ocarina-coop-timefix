#!/bin/sh
# Builds and runs the auto updater tests (Linux). Pass --network to also download and check the newest real release.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/story-coop-updater-test-bin"
c++ -std=c++20 -O1 -Wall -I"$ROOT/server/src" -o "$OUT" \
    "$ROOT/tools/updater-test/updater_test.cpp" "$ROOT/soh/soh/Updater/UpdaterCore.cpp"
"$OUT" "$@"
