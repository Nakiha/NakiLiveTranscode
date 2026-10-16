FROM ubuntu:24.04 AS build
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    build-essential cmake pkg-config nasm libx264-dev libevent-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENV NAKI_BUILD_ROOT=/opt/naki-build
COPY scripts/build-service.sh scripts/build-service.sh
COPY experiments/decoder-20261016/ffmpeg-7.0.3/ experiments/decoder-20261016/ffmpeg-7.0.3/
RUN NAKI_FFMPEG_ONLY=1 bash scripts/build-service.sh
COPY CMakeLists.txt ./
COPY app/ app/
COPY third_party/yyjson/src/ third_party/yyjson/src/
COPY tests/logger_test.c tests/network_probe.c tests/
RUN bash scripts/build-service.sh

FROM ubuntu:24.04 AS runtime
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    libevent-2.1-7t64 libcurl4t64 libx264-164 ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 naki && useradd --uid 10001 --gid 10001 --no-create-home naki \
    && mkdir -p /data /var/log/naki && chown 10001:10001 /data /var/log/naki
COPY --from=build /src/out/bin/naki-live /src/out/bin/naki-live-ctl /usr/local/bin/
COPY --from=build /opt/naki-build/install/lib/ /opt/naki-build/install/lib/
COPY scripts/maintain-logs.py /usr/local/lib/naki/maintain-logs.py
COPY third_party/yyjson/LICENSE /usr/share/doc/naki/yyjson-LICENSE
COPY experiments/decoder-20261016/ffmpeg-7.0.3/COPYING* /usr/share/doc/naki/
USER 10001:10001
WORKDIR /data
EXPOSE 8090/tcp 15000/udp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/naki-live"]
CMD ["--bind", "0.0.0.0", "--run-dir", "/var/log/naki", "--status-interval", "10"]
