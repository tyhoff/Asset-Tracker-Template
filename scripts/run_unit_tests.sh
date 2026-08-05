#!/usr/bin/env bash
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# Run the native_sim unit tests from a macOS (or any Docker-capable) host.
#
# Why a container: native_sim is Linux-only -- Zephyr's POSIX arch refuses to
# configure on macOS ("The POSIX architecture only works on Linux"). This script
# runs Twister inside a small Linux image so unit tests are runnable locally
# without hardware and without a Linux VM.
#
# On arm64 hosts the 64-bit variant is required: plain native_sim sets
# CONFIG_64BIT=n, which aborts with "this Aarch64 machine has a 64-bit userspace".
#
# Usage:
#   scripts/run_unit_tests.sh                      # all module tests
#   scripts/run_unit_tests.sh tests/module/location   # a single suite
#   NCS_DIR=/path/to/ncs scripts/run_unit_tests.sh
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NCS_DIR="${NCS_DIR:-/Users/tyler/junk/ncs-3.4.0}"
IMAGE="${IMAGE:-att-unit:latest}"
PLATFORM="${PLATFORM:-native_sim/native/64}"
TEST_PATH="${1:-tests/module}"

if [ ! -d "$NCS_DIR/zephyr" ]; then
    echo "ERROR: no Zephyr found at $NCS_DIR/zephyr" >&2
    echo "Set NCS_DIR to your west workspace, or create one:" >&2
    echo "  west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.0 $NCS_DIR" >&2
    echo "  cd $NCS_DIR && west update --narrow -o=--depth=1" >&2
    exit 1
fi

# Build the image only if it is missing. Pass --rebuild to force.
if [ "${1:-}" = "--rebuild" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    [ "${1:-}" = "--rebuild" ] && TEST_PATH="${2:-tests/module}"
    echo "==> Building $IMAGE"
    docker build -f "$REPO_DIR/tests/unit_docker/Dockerfile" -t "$IMAGE" \
        "$REPO_DIR/tests/unit_docker"
fi

echo "==> Running $TEST_PATH on $PLATFORM"
docker run --rm \
    -v "$NCS_DIR":/work/ncs \
    -v "$REPO_DIR":/work/project \
    -e ZEPHYR_BASE=/work/ncs/zephyr \
    -e ZEPHYR_TOOLCHAIN_VARIANT=host \
    "$IMAGE" \
    bash -lc "cd /work/ncs && python3 zephyr/scripts/twister \
        -T /work/project/${TEST_PATH#/} \
        --platform ${PLATFORM} \
        -O /tmp/twister-out \
        --inline-logs"
