#!/usr/bin/env bash
# Fetch + patch libpeer into deps/libpeer (gitignored). The firmware does NOT
# build against plain upstream: patches/libpeer/*.patch are load-bearing
# (0001: DTLS init + DTLS 1.2 pin; 0002: SCTP/DTLS lock, design note H2).
# Idempotent: re-running on an already-patched tree is a no-op.
set -euo pipefail
cd "$(dirname "$0")/.."
PIN=5b849de378545c31d34759a145413846953e1366   # sepfy/libpeer main, 2026-08-23
URL=https://github.com/sepfy/libpeer.git
D=deps/libpeer
mkdir -p deps
if [ ! -d "$D/.git" ]; then
  git clone --quiet "$URL" "$D"
fi
if [ "$(git -C "$D" rev-parse HEAD)" != "$PIN" ]; then
  if [ -n "$(git -C "$D" status --porcelain)" ]; then
    echo "deps/libpeer: local changes on a non-pinned commit — refusing to touch it" >&2
    exit 1
  fi
  git -C "$D" fetch --quiet origin
  git -C "$D" checkout --quiet "$PIN"
fi
for p in patches/libpeer/*.patch; do
  name=$(basename "$p")
  if git -C "$D" apply --reverse --check "$PWD/$p" 2>/dev/null; then
    echo "libpeer: $name already applied"
  elif git -C "$D" apply --check "$PWD/$p" 2>/dev/null; then
    git -C "$D" apply "$PWD/$p"
    echo "libpeer: applied $name"
  else
    echo "libpeer: $name does NOT apply cleanly — tree diverged; fix by hand" >&2
    exit 1
  fi
done
echo "libpeer: $(git -C "$D" rev-parse --short HEAD) + $(ls patches/libpeer/*.patch | wc -l) patch(es)"
