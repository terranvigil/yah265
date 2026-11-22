# syntax=docker/dockerfile:1
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ffmpeg with libyah265, and y265, as a small multi-arch image.
#
#   docker buildx build -f packaging/docker/Dockerfile -t yah265 .
#   docker run --rm -v "$PWD:/work" yah265 -i in.mp4 -c:v libyah265 -crf 25 out.mp4
#
# Build from the repo root. `--target artifact --output type=local,dest=out`
# writes only the static bundle and the source tarballs, which is how the
# release workflow gets its Linux binaries.

ARG ALPINE=3.21

FROM alpine:${ALPINE} AS build
ARG TARGETARCH
ARG JOBS=8
ARG YAH265_REV=unknown
ARG FFMPEG_REF=
RUN apk add --no-cache bash build-base meson ninja pkgconf git curl tar \
        zlib-dev zlib-static linux-headers python3 \
    && if [ "$TARGETARCH" = amd64 ]; then apk add --no-cache nasm; fi
WORKDIR /src
COPY . .
RUN YAH265_REV="$YAH265_REV" FFMPEG_REF="${FFMPEG_REF:-$(cat packaging/ffmpeg-ref)}" \
    packaging/build-static.sh --work /build --jobs "$JOBS" --sources \
      --name "yah265-ffmpeg-linux-$TARGETARCH" \
    && mkdir -p /out \
    && cp /build/bundle/*.tar.gz /out/ \
    && cp -r /build/sources /out/sources \
    && cp -r "/build/bundle/yah265-ffmpeg-linux-$TARGETARCH" /out/bundle

FROM scratch AS artifact
COPY --from=build /out/ /

FROM alpine:${ALPINE}
LABEL org.opencontainers.image.title="yah265" \
      org.opencontainers.image.description="ffmpeg with the libyah265 HEVC encoder, and y265" \
      org.opencontainers.image.source="https://github.com/terranvigil/yah265" \
      org.opencontainers.image.licenses="GPL-2.0-or-later"
COPY --from=build /out/bundle/bin/ /usr/local/bin/
COPY --from=build /out/bundle/LICENSE /out/bundle/SOURCE.md /out/bundle/README.txt \
     /out/bundle/NOTICE-patents.txt /usr/share/yah265/
COPY --from=build /out/bundle/licenses/ /usr/share/yah265/licenses/
COPY --from=build /out/sources/ /usr/share/yah265/source/
WORKDIR /work
ENTRYPOINT ["ffmpeg"]
CMD ["-hide_banner", "-h", "encoder=libyah265"]
