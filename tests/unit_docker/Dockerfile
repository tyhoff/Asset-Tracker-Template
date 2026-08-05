# Minimal Linux image for running Zephyr native_sim unit tests.
#
# Why this exists: native_sim is Linux-only ("The POSIX architecture only works on
# Linux"), so unit tests cannot run directly on macOS. native_sim builds with the
# HOST compiler (ZEPHYR_TOOLCHAIN_VARIANT=host), so the full Zephyr SDK is NOT
# required -- which keeps this image small and lets it run natively on arm64
# instead of emulating the x86 CI image.
#
# Python 3.12 base: Zephyr 4.4 requires Python >= 3.12 (Debian bookworm ships 3.11).
FROM python:3.12-slim-bookworm

ENV DEBIAN_FRONTEND=noninteractive
ENV ZEPHYR_TOOLCHAIN_VARIANT=host

# ruby: required by NCS's test_runner_generate (Unity/CMock generator scripts)
RUN apt-get -y update && apt-get -y install --no-install-recommends \
        build-essential cmake ninja-build git ruby \
        device-tree-compiler ca-certificates \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Twister + Zephyr build/test python dependencies.
RUN pip3 install --no-cache-dir \
        west pyelftools pyyaml packaging pykwalify natsort progress anytree \
        intelhex colorama ply pyserial junitparser tabulate jsonschema \
        psutil filelock GitPython unidiff pytest mock tqdm \
        "zcbor==0.9.1" "cbor2==5.6.5"

WORKDIR /work
