# syntax=docker/dockerfile:1
#
# rana-socketd image (Task 1).
# Multi-stage: build the statically-linked daemon + flatc from flatbuffers
# v24.3.25 (same pin as predep.toml), then ship a slim runtime that also
# carries scripts/rana-ask.sh (the LLM hop).

############################ builder ############################
# trixie (glibc 2.41 / GLIBCXX_3.4.32) is required: the `predep` release binary
# needs GLIBC_2.38+, which bookworm (2.36) does not provide.
FROM debian:trixie-20260824-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++ cmake git curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# premake5 — generates the Makefiles used by predep's premake5 build stage.
ARG PREMAKE_VER=5.0.0-beta2
RUN curl -fsSL "https://github.com/premake/premake-core/releases/download/v${PREMAKE_VER}/premake-${PREMAKE_VER}-linux.tar.gz" -o /tmp/premake.tgz \
    && mkdir -p /tmp/pm && tar -xzf /tmp/premake.tgz -C /tmp/pm \
    && find /tmp/pm -name premake5 -type f -exec install -m 0755 {} /usr/local/bin/premake5 \; \
    && rm -rf /tmp/pm /tmp/premake.tgz \
    && premake5 --version

# predep — stage processor that vendors deps and builds rana-socketd per
# predep.toml. Pulled from the latest GitHub release (linux x86_64 binary).
RUN curl -fsSL "https://github.com/10per5/predep/releases/latest/download/predep-linux-x86_64.tar.gz" -o /tmp/predep.tgz \
    && mkdir -p /tmp/pd && tar -xzf /tmp/predep.tgz -C /tmp/pd \
    && find /tmp/pd -name predep -type f -exec install -m 0755 {} /usr/local/bin/predep \; \
    && rm -rf /tmp/pd /tmp/predep.tgz \
    && predep --version

# flatc (flatbuffers compiler) v24.3.25 — same pin as predep.toml; consumed by
# the premake gen_schema_client prebuild to generate the FlatBuffers headers.
# Also install the matching flatbuffers runtime headers to /usr/local/include so
# the daemon/client/serializer compiles find "flatbuffers/flatbuffers.h" without
# depending on a system flatbuffers package or predep's (often-empty) vendored
# copy. Using the same source as flatc keeps the version exact.
ARG FLATBUFFERS_VER=24.3.25
RUN curl -fsSL "https://github.com/google/flatbuffers/archive/refs/tags/v${FLATBUFFERS_VER}.tar.gz" -o /tmp/fb.tgz \
    && mkdir -p /tmp/fb && tar -xzf /tmp/fb.tgz -C /tmp/fb \
    && cmake -S "/tmp/fb/flatbuffers-${FLATBUFFERS_VER}" -B /tmp/fb/build \
    -DCMAKE_BUILD_TYPE=Release -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATC=ON \
    && cmake --build /tmp/fb/build -j"$(nproc)" --target flatc \
    && install -m 0755 /tmp/fb/build/flatc /usr/local/bin/flatc \
    && mkdir -p /usr/local/include/flatbuffers \
    && cp -r "/tmp/fb/flatbuffers-${FLATBUFFERS_VER}/include/flatbuffers/." /usr/local/include/flatbuffers/ \
    && rm -rf /tmp/fb /tmp/fb.tgz

WORKDIR /src
COPY . .

# predep refuses to run as root. Create a normal build user and run it as that
# user; predep only self-sudo's for install/uninstall stages, which the build
# stage does not use.
RUN useradd -m -s /bin/bash bldr && chown -R bldr /src
USER bldr
ENV HOME=/home/bldr

# Vendor dependencies and build the statically-linked daemon + ask-hop encoder
# via predep (stages declared in predep.toml: `main` = `build` depends on
# `vendor`).
RUN predep

############################ runtime ############################
FROM debian:bookworm-20260824-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ca-certificates jq espeak-ng python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/socket/bin/Release/rana-socketd /usr/local/bin/rana-socketd
COPY --from=builder /src/scripts/rana-ask.sh /usr/local/bin/rana-ask.sh
COPY --from=builder /src/scripts/rana-stt.sh /usr/local/bin/rana-stt.sh
COPY --from=builder /src/scripts/gen_system_prompt.py /usr/local/bin/gen_system_prompt.py
COPY --from=builder /src/serializer/bin/Release/rana-serializer /usr/local/bin/rana-serializer
# The daemon build regenerates command_generated.py from command.fbs via flatc;
# export it so the agent's Python client stays in lockstep with the daemon's
# compiled C++ schema (same union ordinals / status codes) and never drifts.
COPY --from=builder /src/schema/generated/command_generated.py /opt/rana/client_command_generated.py
RUN chmod 0755 /usr/local/bin/rana-socketd /usr/local/bin/rana-ask.sh \
    /usr/local/bin/rana-stt.sh /usr/local/bin/gen_system_prompt.py \
    /usr/local/bin/rana-serializer

# HEALTHCHECK dials port 9000 with a zero-length frame.
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD sh -c 'echo -n | curl -s -o /dev/null telnet://127.0.0.1:9000'

EXPOSE 9000
ENTRYPOINT ["rana-socketd"]
CMD ["/etc/rana/rana-socket.toml"]
