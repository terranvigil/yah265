# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The linux/amd64 build image (docs/x86-plan.md), adapted from yah264's
# docker/x86 image. gcc and clang to build the tree, meson and ninja,
# ffmpeg and libde265's dec265 as the two conformance decoders, python3
# for the gate scripts.
#
# The image carries no source and no corpus: scripts/x86_docker.sh
# bind-mounts the worktree read-write and the corpus read-only.
FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        ffmpeg \
        libde265-examples \
        meson \
        ninja-build \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# the scripts run as the invoking uid, which has no passwd entry and so
# no home; meson wants one
ENV HOME=/tmp
ENV XDG_CACHE_HOME=/tmp/.cache

WORKDIR /work
CMD ["/bin/bash"]
