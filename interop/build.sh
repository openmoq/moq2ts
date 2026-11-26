#!/usr/bin/env bash
# Builds the interop client image from this checkout, on the moqxr release that
# CI pins. IMAGE overrides the tag.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
moqxr="$(sed -n 's/^  MOQXR_VERSION: *//p' "$root/.github/workflows/build.yml")"
test -n "$moqxr"
DOCKER_BUILDKIT=1 docker build -f "$root/interop/Dockerfile" --build-arg MOQXR_VERSION="$moqxr" \
  -t "${IMAGE:-moq2ts-interop-client:latest}" "$@" "$root"
