#!/bin/bash
# Cross-compile the YJIT Rust core for an Android ABI and produce the
# symbol-localized relocatable object the engine links (prebuilt/<ABI>/libyjit.o).
#
# Usage: localize_yjit.sh <rust-target> <out-obj>
#   e.g.  localize_yjit.sh x86_64-linux-android /mnt/d/urge-251116/binding/mri/third_party/ruby_android/prebuilt/x86_64/libyjit.o
#
# Requires: rustup target add <rust-target>, GNU ld/objcopy (WSL or Linux),
# and the ruby source tree that matches the engine's ruby_android version.
set -e

TARGET="$1"
OUT="$2"
SRC=/mnt/d/ruby-build/ruby-4.0.6-win
WORK=/root/yjit_build

test -n "$TARGET" -a -n "$OUT"
mkdir -p "$WORK"
cd "$SRC"

# 1) compile the YJIT crate exactly like ruby's YJIT_RUSTC_ARGS, plus --target
rustc --crate-name=yjit \
  --crate-type=staticlib --cfg 'feature="stats_allocator"' \
  --edition=2021 -g -C lto=thin -C opt-level=3 -C overflow-checks=on \
  --target "$TARGET" \
  --out-dir="$WORK" \
  yjit/src/lib.rs

cd "$WORK"

# 2) partial link the staticlib into one relocatable object
ld -r --whole-archive libyjit.a -o libyjit_raw.o

# 3) localize everything except the rb_* API symbols (they are the only ones
#    the engine may bind; the rest would clash with ruby's missing/*.c)
nm --defined-only --extern-only libyjit_raw.o | awk '{print $3}' | grep '^rb_' | sort -u > keep.txt
echo "rb_* globals: $(wc -l < keep.txt)"
objcopy --keep-global-symbols=keep.txt libyjit_raw.o libyjit.o
echo "globals after localize: $(nm --defined-only --extern-only libyjit.o | wc -l)"

mkdir -p "$(dirname "$OUT")"
cp libyjit.o "$OUT"
ls -la "$OUT"
echo "DONE $OUT"
