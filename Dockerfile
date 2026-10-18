ARG FFMPEG_STAGE=ffmpeg-local
FROM ubuntu:24.04 AS toolchain
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    build-essential cmake pkg-config nasm libx264-dev libsrt-openssl-dev libssl-dev zlib1g-dev libevent-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends libunwind-dev python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENV NAKI_BUILD_ROOT=/opt/naki-build
FROM toolchain AS ffmpeg-local
COPY scripts/build-ffmpeg.sh scripts/build-vvenc.sh scripts/archive-symbols.py scripts/sdk-identity.py scripts/
COPY Dockerfile.ffmpeg ./
COPY third_party/vvenc/ third_party/vvenc/
COPY experiments/decoder-20261016/ffmpeg-7.0.3/ experiments/decoder-20261016/ffmpeg-7.0.3/
RUN NAKI_VVENC_SYMBOLS=/opt/naki-build/vvenc-symbols bash scripts/build-ffmpeg.sh && python3 scripts/archive-symbols.py \
    --output /opt/naki-build/ffmpeg-symbols --label ffmpeg-local /opt/naki-build/ffmpeg/lib*/*.so.*
FROM scratch AS ffmpeg-prebuilt
# Extract inside Linux so Windows clients do not have to transmit Linux symlinks.
ADD out/ffmpeg-sdk.tar.gz /opt/naki-build/
FROM ${FFMPEG_STAGE} AS ffmpeg
FROM toolchain AS build
COPY --from=ffmpeg /opt/naki-build/install/ /opt/naki-build/install/
COPY --from=ffmpeg /opt/naki-build/ffmpeg-symbols/ /symbols/ffmpeg/
COPY --from=ffmpeg /opt/naki-build/vvenc-symbols/ /symbols/vvenc/
COPY scripts/build-service.sh scripts/archive-symbols.py scripts/sdk-identity.py scripts/sdk-expected.py scripts/build-ffmpeg.sh scripts/build-vvenc.sh scripts/
COPY Dockerfile.ffmpeg ./
COPY out/expected-sdk.json /src/expected-sdk.json
COPY CMakeLists.txt ./
COPY app/ app/
COPY third_party/yyjson/src/ third_party/yyjson/src/
COPY tests/unit/ tests/unit/
COPY tests/fixtures/ tests/fixtures/
COPY tests/support/ tests/support/
ARG NAKI_MEDIA_HOOK_SOURCE=
ARG DEPENDENCY_KEY=
RUN NAKI_SKIP_FFMPEG=1 NAKI_EXPECTED_SDK_MANIFEST=/src/expected-sdk.json \
    NAKI_EXPECTED_DEPENDENCY_KEY="$DEPENDENCY_KEY" NAKI_MEDIA_HOOK_SOURCE="$NAKI_MEDIA_HOOK_SOURCE" bash scripts/build-service.sh
ARG SOURCE_REVISION=unknown
RUN python3 scripts/archive-symbols.py --output /symbols/app --label application \
    --source-revision "$SOURCE_REVISION" --dependency-key "$DEPENDENCY_KEY" out/bin \
    && dpkg-query -W > /symbols/app/packages.txt \
    && mkdir -p /runtime-bin \
    && cp out/bin/naki-live out/bin/naki-live-worker out/bin/naki-live-ctl out/bin/naki-crash-collector /runtime-bin/ \
    && strip --strip-debug /runtime-bin/*
FROM build AS test
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends ffmpeg git openssl \
    && rm -rf /var/lib/apt/lists/*
COPY scripts/ scripts/
COPY tests/ tests/
COPY Dockerfile.ffmpeg ./
COPY Dockerfile compose.yaml ./
COPY .github/ .github/
# Keep probe-only fixtures outside the native source directories. A partial
# FFmpeg tree must not accidentally become a source-less SDK consumer input.
ENV NAKI_DECODER_PROBE_SOURCE=/src/tests/decoder-probes \
    NAKI_EXPECTED_SDK_MANIFEST=/src/expected-sdk.json
COPY experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async/async_probe.c \
     experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async/auto_probe.c \
     experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async/auto_lifetime_test.c \
     tests/decoder-probes/
RUN mkdir -p out
FROM scratch AS symbols
COPY --from=build /symbols/ /

FROM ubuntu:24.04 AS runtime
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    libevent-2.1-7t64 libcurl4t64 libx264-164 libsrt1.5-openssl libunwind8 libstdc++6 zlib1g ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 naki && useradd --uid 10001 --gid 10001 --no-create-home naki \
    && mkdir -p /data /var/log/naki && chown 10001:10001 /data /var/log/naki
ARG SOURCE_REVISION=unknown
ARG DEPENDENCY_KEY=local
ARG SOURCE_TREE_SHA256=unknown
ARG BUILD_RUN_ID=local
ARG BUILD_RUN_ATTEMPT=1
LABEL org.opencontainers.image.source="https://github.com/Nakiha/NakiLiveTranscode" \
    org.opencontainers.image.revision="$SOURCE_REVISION" io.naki.ffmpeg-dependency="$DEPENDENCY_KEY" \
    io.naki.source-tree="$SOURCE_TREE_SHA256" io.naki.build-run="$BUILD_RUN_ID" io.naki.build-attempt="$BUILD_RUN_ATTEMPT"
COPY --from=build /runtime-bin/ /usr/local/bin/
COPY --from=build /opt/naki-build/install/lib/ /opt/naki-build/install/lib/
COPY scripts/maintain-logs.py /usr/local/lib/naki/maintain-logs.py
COPY scripts/service-launcher.py /usr/local/lib/naki/service-launcher.py
COPY third_party/yyjson/LICENSE /usr/share/doc/naki/yyjson-LICENSE
COPY third_party/vvenc/LICENSE.txt /usr/share/doc/naki/vvenc-LICENSE
COPY experiments/decoder-20261016/ffmpeg-7.0.3/COPYING* /usr/share/doc/naki/
USER 10001:10001
WORKDIR /data
EXPOSE 8090/tcp 15000/udp
STOPSIGNAL SIGTERM
ENTRYPOINT ["python3", "/usr/local/lib/naki/service-launcher.py"]
CMD ["--bind", "0.0.0.0", "--run-dir", "/var/log/naki", "--status-interval", "10"]
