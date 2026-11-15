#!/usr/bin/env bash
# Run a command inside the cratchet toolchain image with the repo mounted.
# Used by CI and handy locally:  scripts/in_container.sh cmake --preset dev
#
# Env:
#   CRATCHET_IMAGE   image tag (default: cratchet-dev)
#   VCPKG_CACHE_DIR  host dir for the vcpkg binary cache (default: .cache/vcpkg)
#   CC, CXX          compiler for this run (default: from the image, Clang 18)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${CRATCHET_IMAGE:-cratchet-dev}"
cache_dir="${VCPKG_CACHE_DIR:-${repo_root}/.cache/vcpkg}"
mkdir -p "${cache_dir}"

env_args=()
[[ -n "${CC:-}" ]] && env_args+=(-e "CC=${CC}")
[[ -n "${CXX:-}" ]] && env_args+=(-e "CXX=${CXX}")

exec docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  ${env_args[@]+"${env_args[@]}"} \
  -v "${repo_root}:/workspace" \
  -v "${cache_dir}:/vcpkg-cache" \
  -w /workspace \
  "${image}" "$@"
