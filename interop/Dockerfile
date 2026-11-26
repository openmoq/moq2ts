# syntax=docker/dockerfile:1
# moq2ts client for the MoQ interop runner's MPEG-TS scenarios.
#
# The build context is the repository root: use interop/build.sh.

ARG UBUNTU=ubuntu:24.04

# moq2ts-cli on the prebuilt moqxr SDK, as CI builds it.
FROM ${UBUNTU} AS publisher
ARG MOQXR_VERSION
RUN set -eu; apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl build-essential cmake ninja-build pkg-config \
        qt6-base-dev libgl1-mesa-dev libssl-dev libopenh264-dev libavcodec-dev libavdevice-dev libavformat-dev \
        libavutil-dev libswresample-dev libswscale-dev libopus-dev libsrt-openssl-dev
RUN set -eu; test -n "$MOQXR_VERSION"; mkdir -p /sdk; \
    curl -fsSL "https://github.com/openmoq/moqxr/releases/download/${MOQXR_VERSION}/openmoq-publisher-${MOQXR_VERSION}-Linux.tar.gz" \
        | tar xz -C /sdk --strip-components=1
COPY CMakeLists.txt /src/CMakeLists.txt
COPY src /src/src
COPY tests /src/tests
COPY cmake /src/cmake
RUN set -eu; cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF -DMOQXR_SDK_DIR=/sdk; \
    cmake --build /build --target moq2ts-cli --parallel

# moq5 with the picoquic raw QUIC and WebTransport adapters, and moq2ts-receiver
# on it.
FROM ${UBUNTU} AS receiver
# A moq5 with moq_endpoint_cfg_t.done_wait_timeout_us (openmoq/moq5#74).
ARG MOQ5_COMMIT=573955c166c999d35886ca34cbe2f05fbc1c4de2
RUN set -eu; apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates git build-essential cmake pkg-config libssl-dev
RUN set -eu; git init -q /moq5; cd /moq5; \
    git fetch -q --depth 1 https://github.com/openmoq/moq5.git "$MOQ5_COMMIT"; git checkout -q FETCH_HEAD; \
    bash scripts/setup_picoquic_deps.sh; \
    . .deps/picoquic-ci/picoquic_deps.env; \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOQ_BUILD_ADAPTER_PICOQUIC=ON -DMOQ_BUILD_PQ_THREADED=ON \
        -DMOQ_BUILD_ADAPTER_PICO_WT=ON -DMOQ_BUILD_PICO_WT_MANAGED=ON \
        -DMOQ_BUILD_SERVICE=ON -DMOQ_BUILD_MPEG2TS=ON -DMOQ_BUILD_MSF=ON -DMOQ_BUILD_TESTS=OFF \
        -DMOQ_BUILD_EXAMPLES=OFF -DMOQ_PICOQUIC_SOURCE_DIR="$MOQ_PICOQUIC_SOURCE_DIR" \
        -DMOQ_PICOTLS_PREFIX="$MOQ_PICOTLS_PREFIX" -DCMAKE_INSTALL_PREFIX=/moq5-install; \
    cmake --build build --parallel "$(nproc)"; cmake --install build
COPY src/receive /src/src/receive
COPY tests/receiver_stats_test.c tests/receiver-cli-smoke.sh /src/tests/
RUN set -eu; . /moq5/.deps/picoquic-ci/picoquic_deps.env; \
    cmake -S /src/src/receive -B /build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/moq5-install \
        -DMOQ_PICOQUIC_SOURCE_DIR="$MOQ_PICOQUIC_SOURCE_DIR" -DMOQ_PICOTLS_PREFIX="$MOQ_PICOTLS_PREFIX"; \
    cmake --build /build --parallel "$(nproc)"; ctest --test-dir /build --output-on-failure

# What the runner's fixtures and checker need.
FROM ${UBUNTU} AS tools
ARG TSDUCK_VERSION=3.45-4798
ARG TSDUCK_SHA256=0023689f76e75b64e45253771208bcf8ca95b91fbfd0900da097c729f3c39fde
RUN set -eu; apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl ffmpeg python3; \
    deb=/tmp/tsduck.deb; \
    curl -fsSL -o "$deb" "https://github.com/tsduck/tsduck/releases/download/v${TSDUCK_VERSION}/tsduck_${TSDUCK_VERSION}.ubuntu24_amd64.deb"; \
    echo "${TSDUCK_SHA256}  $deb" | sha256sum -c -; \
    apt-get install -y --no-install-recommends "$deb"; rm -f "$deb"; \
    tsp --version

# The runner's fixtures and checker, from a pinned runner commit. Until
# englishm/moq-interop-runner#209 merges, that commit is on its author's fork.
FROM tools AS fixtures
ARG RUNNER_REPO=t0ms/moq-interop-runner
ARG RUNNER_COMMIT=d2fbb0221c70aca01b42754b9924c007c8cb26e1
RUN set -eu; base="https://raw.githubusercontent.com/${RUNNER_REPO}/${RUNNER_COMMIT}/lib/media/mpeg-ts"; \
    mkdir -p /runner/test /fixture; cd /runner; \
    for f in make-fixture.sh check-ts.py splice-step.py test/test-check-ts.sh; do curl -fsSL -o "$f" "$base/$f"; done; \
    bash make-fixture.sh /fixture/fixture.ts; \
    bash make-fixture.sh --step /fixture/fixture-step.ts; \
    bash make-fixture.sh --mpts /fixture/fixture-mpts.ts; \
    bash test/test-check-ts.sh

FROM tools AS runtime
RUN set -eu; \
    apt-get install -y --no-install-recommends libqt6core6t64 libqt6gui6t64 libavcodec60 libavdevice60 \
        libavformat60 libavutil58 libswresample4 libswscale7 libopus0 libopenh264-7 libsrt1.5-openssl libssl3t64; \
    apt-get purge -y curl; apt-get autoremove -y; rm -rf /var/lib/apt/lists/*
COPY --from=publisher /build/moq2ts-cli /app/moq2ts-cli
COPY --from=receiver /build/moq2ts-receiver /app/moq2ts-receiver
COPY --from=fixtures /fixture /app/fixture
COPY --from=fixtures /runner/check-ts.py /app/check-ts.py
COPY interop/client.py interop/scenarios.py interop/session.py interop/tsfile.py /app/
# Every shared library of both binaries is present.
RUN set -eu; ! ldd /app/moq2ts-cli /app/moq2ts-receiver | grep -q 'not found'; /app/moq2ts-cli --version
ENTRYPOINT ["python3", "/app/client.py"]
