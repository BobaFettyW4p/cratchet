# Sandbox image for unattended coding agents (isengard `agent new --sandbox`): the base agent
# image plus the cratchet toolchain, so an agent can configure, build and test without docker.
# Mirrors docker/dev.Dockerfile; keep VCPKG_COMMIT in sync with it and with the
# "builtin-baseline" in vcpkg.json.
#
# Build from the repo root, after `agent image build` has produced the base image:
#   podman build -t cratchet-agent -f docker/agent.Dockerfile .
# Use:
#   AGENT_IMAGE=cratchet-agent agent new --sandbox BobaFettyW4p/cratchet <name> "<task>"
ARG BASE_IMAGE=isengard-agent
FROM ${BASE_IMAGE}

USER root

ARG VCPKG_COMMIT=8f363bdc11359ede4bc51e5eb32a2617538049c8
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-13 gcc-13 \
      clang-18 clang-tidy-18 clang-format-18 libclang-rt-18-dev \
      cmake ninja-build pkg-config \
      zip unzip tar gdb \
    && rm -rf /var/lib/apt/lists/*

# The tree and binary cache are world-writable because the container runs as the host user
# mapped onto uid 1000.
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache

RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && git -C ${VCPKG_ROOT} checkout ${VCPKG_COMMIT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics \
    && mkdir -p ${VCPKG_DEFAULT_BINARY_CACHE} \
    && chmod -R a+rwX ${VCPKG_ROOT} ${VCPKG_DEFAULT_BINARY_CACHE}

ENV CC=clang-18 \
    CXX=clang++-18

# Warm the binary cache with the project's dependencies so the agent's first configure is fast.
COPY vcpkg.json /tmp/vcpkg-warm/vcpkg.json
RUN ${VCPKG_ROOT}/vcpkg install --x-manifest-root=/tmp/vcpkg-warm \
      --x-install-root=/tmp/vcpkg-warm/installed \
    && rm -rf /tmp/vcpkg-warm \
    && chmod -R a+rwX ${VCPKG_DEFAULT_BINARY_CACHE}

USER node
WORKDIR /work
