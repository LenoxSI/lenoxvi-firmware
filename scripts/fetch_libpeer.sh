#!/usr/bin/env bash
# Fetch + patch libpeer into deps/libpeer (gitignored). The firmware does NOT
# build against plain upstream: patches/libpeer/*.patch are load-bearing
# (0001: DTLS init + DTLS 1.2 pin; 0002: SCTP/DTLS lock, design note H2;
#  0003: SCTP INIT retransmit, design note).
# Idempotent: builds the expected tree (pin + every patch, in order) in a temp
# dir and compares — identical = already applied; clean pinned tree = patch it;
# anything else = refuse (someone edited deps/libpeer by hand). Per-patch
# reverse checks don't work once later patches touch the same hunks.
set -euo pipefail
cd "$(dirname "$0")/.."
PIN=5b849de378545c31d34759a145413846953e1366   # sepfy/libpeer main, 2026-08-23
URL=https://github.com/sepfy/libpeer.git
D=deps/libpeer
ROOT=$PWD
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

PATCHES=("$ROOT"/patches/libpeer/*.patch)
EXPECT=$(mktemp -d)
trap 'rm -rf "$EXPECT"' EXIT
git -C "$D" archive "$PIN" | tar -x -C "$EXPECT"
same() { diff -rq --exclude=.git "$EXPECT" "$D" >/dev/null; }

# Find how much of the stack deps/libpeer already has: pin + first k patches.
applied=-1
same && applied=0
for i in "${!PATCHES[@]}"; do
  (cd "$EXPECT" && git apply "${PATCHES[$i]}") || {
    echo "libpeer: $(basename "${PATCHES[$i]}") does not apply on pin + earlier patches" >&2; exit 1; }
  same && applied=$((i + 1))
done
# EXPECT is now pin + the FULL stack.

if [ "$applied" -lt 0 ]; then
  echo "libpeer: deps/libpeer has edits that are not a prefix of the patch stack — refusing." >&2
  echo "         Diff it against the pin and turn the change into a patches/libpeer/*.patch." >&2
  diff -rq --exclude=.git "$EXPECT" "$D" | head -5 >&2
  exit 1
fi
if [ "$applied" -eq "${#PATCHES[@]}" ]; then
  echo "libpeer: already at pin + all ${#PATCHES[@]} patches"
else
  for ((i = applied; i < ${#PATCHES[@]}; i++)); do
    git -C "$D" apply "${PATCHES[$i]}"
    echo "libpeer: applied $(basename "${PATCHES[$i]}")"
  done
  same || { echo "libpeer: result != expected stack" >&2; exit 1; }
fi
echo "libpeer: $(git -C "$D" rev-parse --short HEAD) + $(ls "$ROOT"/patches/libpeer/*.patch | wc -l) patch(es)"
