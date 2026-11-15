# Toolchain image shared by the Dev Container and CI.
# Pinned: Ubuntu 24.04, Clang 18, GCC 13, vcpkg at the same commit as the
# "builtin-baseline" in vcpkg.json (keep the two in sync).
FROM ubuntu:24.04

ARG VCPKG_COMMIT=8f363bdc11359ede4bc51e5eb32a2617538049c8

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-13 gcc-13 \
      clang-18 clang-tidy-18 clang-format-18 libclang-rt-18-dev \
      cmake ninja-build pkg-config \
      git curl zip unzip tar ca-certificates \
      python3 \
      gdb \
    && rm -rf /var/lib/apt/lists/*

# vcpkg is checked out at the pinned commit. On aarch64 vcpkg cannot download its
# own cmake/ninja, so it must use the system ones.
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache

RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && git -C ${VCPKG_ROOT} checkout ${VCPKG_COMMIT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics \
    && mkdir -p ${VCPKG_DEFAULT_BINARY_CACHE} \
    && chmod -R a+rwX ${VCPKG_ROOT} ${VCPKG_DEFAULT_BINARY_CACHE}

# Default compiler; CI overrides CC/CXX for the GCC leg.
ENV CC=clang-18 \
    CXX=clang++-18

WORKDIR /workspace
